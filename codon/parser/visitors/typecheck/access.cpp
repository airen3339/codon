#include <string>
#include <tuple>

#include "codon/parser/ast.h"
#include "codon/parser/cache.h"
#include "codon/parser/common.h"
#include "codon/parser/visitors/simplify/simplify.h"
#include "codon/parser/visitors/typecheck/typecheck.h"

using fmt::format;

namespace codon::ast {

using namespace types;

/// Typecheck identifiers. If an identifier is a static variable, evaluate it and
/// replace it with its value (e.g., a @c IntExpr ). Also ensure that the identifier of
/// a generic function or a type is fully qualified (e.g., replace `Ptr` with
/// `Ptr[byte]`).
/// For tuple identifiers, generate appropriate class. See @c generateTupleStub for
/// details.
void TypecheckVisitor::visit(IdExpr *expr) {
  // Generate tuple stubs if needed
  if (isTuple(expr->value))
    generateTupleStub(std::stoi(expr->value.substr(sizeof(TYPE_TUPLE) - 1)));

  // Handle empty callable references
  if (expr->value == TYPE_CALLABLE) {
    auto typ = ctx->addUnbound(expr, ctx->typecheckLevel);
    typ->getLink()->trait = std::make_shared<CallableTrait>(std::vector<TypePtr>{});
    unify(expr->type, typ);
    expr->markType();
    return;
  }

  // Replace identifiers that have been superseded by domination analysis during the
  // simplification
  while (auto s = in(ctx->cache->replacements, expr->value))
    expr->value = s->first;

  auto val = ctx->find(expr->value);
  if (!val) {
    // Handle overloads
    if (auto i = in(ctx->cache->overloads, expr->value)) {
      if (i->size() == 1) {
        val = ctx->forceFind(i->front().name);
      } else {
        auto d = getDispatch(expr->value);
        val = ctx->forceFind(d->ast->name);
      }
    }
    if (!val)
      seqassert(expr, "cannot find '{}'", expr->value);
  }
  unify(expr->type, ctx->instantiate(expr, val->type));

  if (val->type->isStaticType()) {
    // Evaluate static expression if possible
    expr->staticValue.type = StaticValue::Type(val->type->isStaticType());
    auto s = val->type->getStatic();
    seqassert(!expr->staticValue.evaluated, "expected unevaluated expression: {}",
              expr->toString());
    if (s && s->expr->staticValue.evaluated) {
      // Replace the identifier with static expression
      if (s->expr->staticValue.type == StaticValue::STRING)
        resultExpr = transform(N<StringExpr>(s->expr->staticValue.getString()));
      else
        resultExpr = transform(N<IntExpr>(s->expr->staticValue.getInt()));
    }
    return;
  }

  if (val->isType())
    expr->markType();

  // Realize a type or a function if possible and replace the identifier with the fully
  // typed identifier (e.g., `foo` -> `foo[int]`)
  if (realize(expr->type)) {
    if (!val->isVar())
      expr->value = expr->type->realizedName();
    expr->setDone();
  }
}

/// See @c transformDot for details
void TypecheckVisitor::visit(DotExpr *expr) {
  // Make sure to unify the current type with the transformed type
  if ((resultExpr = transformDot(expr)))
    unify(expr->type, resultExpr->type);
  else if (!expr->type)
    unify(expr->type, ctx->addUnbound(expr, ctx->typecheckLevel));
}

/// Find an overload dispatch function for a given overload. If it does not exist,
/// generate it. Dispatch functions ensure that a function call is being routed to the
/// correct overload even when dealing with partial functions and decorators.
/// @example This is how dispatch looks like:
///   ```def foo:dispatch(*args, **kwargs):
///        return foo(*args, **kwargs)```
types::FuncTypePtr TypecheckVisitor::getDispatch(const std::string &fn) {
  // Check if dispatch exists
  for (auto &m : ctx->cache->overloads[fn])
    if (endswith(ctx->cache->functions[m.name].ast->name, ":dispatch"))
      return ctx->cache->functions[m.name].type;

  // Dispatch does not exist. Generate it
  auto name = fn + ":dispatch";
  ExprPtr root; // Root function name used for calling
  auto a = ctx->cache->functions[ctx->cache->overloads[fn][0].name].ast;
  if (!a->attributes.parentClass.empty())
    root = N<DotExpr>(N<IdExpr>(a->attributes.parentClass),
                      ctx->cache->reverseIdentifierLookup[fn]);
  else
    root = N<IdExpr>(fn);
  root = N<CallExpr>(root, N<StarExpr>(N<IdExpr>("args")),
                     N<KeywordStarExpr>(N<IdExpr>("kwargs")));
  auto ast = N<FunctionStmt>(
      name, nullptr, std::vector<Param>{Param("*args"), Param("**kwargs")},
      N<SuiteStmt>(N<ReturnStmt>(root)), Attr({"autogenerated"}));
  ctx->cache->reverseIdentifierLookup[name] = ctx->cache->reverseIdentifierLookup[fn];

  auto baseType = getFuncTypeBase(2);
  auto typ = std::make_shared<FuncType>(baseType, ast.get());
  typ = std::static_pointer_cast<FuncType>(typ->generalize(ctx->typecheckLevel));
  ctx->add(TypecheckItem::Func, name, typ);

  ctx->cache->overloads[fn].insert(ctx->cache->overloads[fn].begin(), {name, 0});
  ctx->cache->functions[name].ast = ast;
  ctx->cache->functions[name].type = typ;
  prependStmts->push_back(ast);
  return typ;
}

/// Transform a dot expression. Select the best method overload if possible.
/// @param args (optional) list of class method arguments used to select the best
///             overload. nullptr if not available.
/// @example
///   `obj.__class__`   -> `type(obj)`
///   `cls.__name__`    -> `"class"` (same for functions)
///   `obj.method`      -> `cls.method(obj, ...)` or
///                        `cls.method(obj)` if method has `@property` attribute
///   @c getClassMember examples:
///   `obj.GENERIC`     -> `GENERIC` (IdExpr with generic/static value)
///   `optional.member` -> `unwrap(optional).member`
///   `pyobj.member`    -> `pyobj._getattr("member")`
/// @return nullptr if no transformation was made
/// See @c getClassMember and @c getBestClassMethod
ExprPtr TypecheckVisitor::transformDot(DotExpr *expr,
                                       std::vector<CallExpr::Arg> *args) {
  // Special case: obj.__class__
  if (expr->member == "__class__") {
    /// TODO: prevent cls.__class__ and type(cls)
    return transformType(NT<CallExpr>(NT<IdExpr>("type"), expr->expr));
  }

  expr->expr = transform(expr->expr, true);

  // Special case: fn.__name__
  // Should go before cls.__name__ to allow printing generic functions
  if (expr->expr->type->getFunc() && expr->member == "__name__") {
    return transform(N<StringExpr>(expr->expr->type->toString()));
  }
  // Special case: cls.__name__
  if (expr->expr->isType() && expr->member == "__name__") {
    if (realize(expr->expr->type))
      return transform(N<StringExpr>(expr->expr->type->toString()));
    return nullptr;
  }

  // Ensure that the type is known (otherwise wait until it becomes known)
  auto typ = expr->expr->getType()->getClass();
  if (!typ)
    return nullptr;

  // Check if this is a method or member access
  auto methods = ctx->findMethod(typ->name, expr->member);
  if (methods.empty())
    return getClassMember(expr, args);
  auto bestMethod = getBestClassMethod(expr, methods, args);

  // Check if a method is a static or an instance method and transform accordingly
  if (expr->expr->isType() || args) {
    // Static access: `cls.method`
    ExprPtr e = N<IdExpr>(bestMethod->ast->name);
    unify(e->type, unify(expr->type, ctx->instantiate(expr, bestMethod, typ.get())));
    return transform(e); // Realize if needed
  } else {
    // Instance access: `obj.method`
    // Transform y.method to a partial call `type(obj).method(args, ...)`
    std::vector<ExprPtr> methodArgs{expr->expr};
    // If a method is marked with @property, just call it directly
    if (!bestMethod->ast->attributes.has(Attr::Property))
      methodArgs.push_back(N<EllipsisExpr>());
    return transform(N<CallExpr>(N<IdExpr>(bestMethod->ast->name), methodArgs));
  }

  return nullptr;
}

/// Select the requested class member.
/// @param args (optional) list of class method arguments used to select the best
///             overload if the member is optional. nullptr if not available.
/// @example
///   `obj.GENERIC`     -> `GENERIC` (IdExpr with generic/static value)
///   `optional.member` -> `unwrap(optional).member`
///   `pyobj.member`    -> `pyobj._getattr("member")`
ExprPtr TypecheckVisitor::getClassMember(DotExpr *expr,
                                         std::vector<CallExpr::Arg> *args) {
  auto typ = expr->expr->getType()->getClass();
  seqassert(typ, "not a class");

  // Case: object member access (`obj.member`)
  if (auto member = ctx->findMember(typ->name, expr->member)) {
    unify(expr->type, ctx->instantiate(expr, member, typ.get()));
    if (expr->expr->isDone() && realize(expr->type))
      expr->setDone();
    return nullptr;
  }

  // Case: object generic access (`obj.T`)
  TypePtr generic = nullptr;
  for (auto &g : typ->generics)
    if (ctx->cache->reverseIdentifierLookup[g.name] == expr->member) {
      generic = g.type;
      break;
    }
  if (generic) {
    unify(expr->type, generic);
    if (!generic->isStaticType()) {
      expr->markType();
    } else {
      expr->staticValue.type = StaticValue::Type(generic->isStaticType());
    }
    if (realize(expr->type)) {
      if (!generic->isStaticType()) {
        return transform(N<IdExpr>(generic->realizedName()));
      } else if (generic->getStatic()->expr->staticValue.type == StaticValue::STRING) {
        expr->type = nullptr; // to prevent unify(T, Static[T]) error
        return transform(
            N<StringExpr>(generic->getStatic()->expr->staticValue.getString()));
      } else {
        expr->type = nullptr; // to prevent unify(T, Static[T]) error
        return transform(N<IntExpr>(generic->getStatic()->expr->staticValue.getInt()));
      }
    }
    return nullptr;
  }

  // Case: transform `optional.member` to `unwrap(optional).member`
  if (typ->is(TYPE_OPTIONAL)) {
    auto dot = N<DotExpr>(transform(N<CallExpr>(N<IdExpr>(FN_UNWRAP), expr->expr)),
                          expr->member);
    if (auto d = transformDot(dot.get(), args))
      return d;
    return dot;
  }

  // Case: transform `pyobj.member` to `pyobj._getattr("member")`
  if (typ->is("pyobj")) {
    return transform(
        N<CallExpr>(N<DotExpr>(expr->expr, "_getattr"), N<StringExpr>(expr->member)));
  }

  // For debugging purposes: ctx->findMethod(typ->name, expr->member);
  error("cannot find '{}' in {}", expr->member, typ->toString());
  return nullptr;
}

/// Select the requested class method. If overloaded, select the best overload.
/// @param methods List of available methods.
/// @param args    (optional) list of class method arguments used to select the best
///                overload if the member is optional. nullptr if not available.
FuncTypePtr
TypecheckVisitor::getBestClassMethod(DotExpr *expr,
                                     const std::vector<FuncTypePtr> &methods,
                                     std::vector<CallExpr::Arg> *args) {
  // Prepare the list of method arguments if possible
  std::unique_ptr<std::vector<CallExpr::Arg>> methodArgs;

  if (args) {
    // Case: arguments explicitly provided (by CallExpr)
    if (!expr->expr->isType()) {
      // Add `self` as the first argument
      args->insert(args->begin(), {"", expr->expr});
    }
    methodArgs = std::make_unique<std::vector<CallExpr::Arg>>();
    for (auto &a : *args)
      methodArgs->push_back(a);
  } else if (methods.size() > 1) {
    // Partially deduced type thus far
    auto typeSoFar = expr->getType() ? expr->getType()->getClass() : nullptr;
    if (typeSoFar && typeSoFar->getFunc()) {
      // Case: arguments available from the previous type checking round
      methodArgs = std::make_unique<std::vector<CallExpr::Arg>>();
      if (!expr->expr->isType()) { // Add `self`
        auto n = N<NoneExpr>();
        n->setType(expr->expr->type);
        methodArgs->push_back({"", n});
      }
      for (auto &a : typeSoFar->getFunc()->getArgTypes()) {
        auto n = N<NoneExpr>();
        n->setType(a);
        methodArgs->push_back({"", n});
      }
    }
  }

  FuncTypePtr bestMethod = nullptr;
  if (methodArgs) {
    // Use the provided arguments to select the best method
    if (auto m = findBestMethod(expr->expr.get(), expr->member, *methodArgs))
      return m;
  } else if (methods.size() > 1) {
    // If overload is ambiguous, route through dispatch function
    return getDispatch(
        ctx->cache->getMethod(expr->expr->type->getClass(), expr->member));
  } else if (!methods.empty()) {
    // Only one overload available
    return methods.front();
  }

  // Print a nice error message
  std::string argsNice;
  if (methodArgs) {
    std::vector<std::string> a;
    for (auto &t : *methodArgs)
      a.emplace_back(fmt::format("'{}'", t.value->type->toString()));
    argsNice = fmt::format(" with arguments {}", fmt::join(a, ", "));
  }
  error("cannot find a method '{}' in {}{}", expr->member, expr->expr->type->toString(),
        argsNice);
  return nullptr;
}

} // namespace codon::ast