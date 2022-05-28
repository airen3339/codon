#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "codon/parser/ast.h"
#include "codon/parser/common.h"
#include "codon/parser/visitors/simplify/simplify.h"
#include "codon/parser/visitors/typecheck/typecheck.h"
#include "codon/sir/attribute.h"

using fmt::format;

namespace codon {
namespace ast {

using namespace types;

struct FindTypeVisitor : ReplaceASTVisitor {
  std::map<types::TypePtr, std::string> &unbounds;
  FindTypeVisitor(std::map<types::TypePtr, std::string> &unbounds)
      : unbounds(unbounds) {}
  void transform(std::shared_ptr<Expr> &expr) override {
    if (!expr)
      return;
    FindTypeVisitor v(unbounds);
    expr->accept(v);
    deactivateUnbounds(expr->type.get(), unbounds);
  }
  void transform(std::shared_ptr<Stmt> &stmt) override {
    if (!stmt)
      return;
    FindTypeVisitor v(unbounds);
    stmt->accept(v);
  }
  static void deactivateUnbounds(Type *t,
                                 std::map<types::TypePtr, std::string> &unbounds) {
    if (!t)
      return;
    auto ub = t->getUnbounds();
    for (auto &u : ub)
      unbounds.erase(u);
    if (auto f = t->getFunc())
      deactivateUnbounds(f->args[0].get(), unbounds);
  }
};
void TypecheckVisitor::deactivateUnbounds(Type *t) {
  FindTypeVisitor::deactivateUnbounds(t, ctx->activeUnbounds);
}
void TypecheckVisitor::deactivateUnbounds(const ExprPtr &expr) {
  FindTypeVisitor v(ctx->activeUnbounds);
  expr->accept(v);
}


ExprPtr TypecheckVisitor::transform(const ExprPtr &expr) {
  return transform(const_cast<ExprPtr &>(expr), false);
}

ExprPtr TypecheckVisitor::transform(ExprPtr &expr, bool allowTypes, bool allowVoid,
                                    bool disableActivation) {
  if (!expr)
    return nullptr;
  auto typ = expr->type;
  if (!expr->done) {
    TypecheckVisitor v(ctx, prependStmts);
    v.allowVoidExpr = allowVoid;
    auto oldActivation = ctx->allowActivation;
    if (disableActivation)
      ctx->allowActivation = false;
    v.setSrcInfo(expr->getSrcInfo());
    ctx->pushSrcInfo(expr->getSrcInfo());
    expr->accept(v);
    ctx->popSrcInfo();
    if (v.resultExpr) {
      v.resultExpr->attributes |= expr->attributes;
      expr = v.resultExpr;
    }
    seqassert(expr->type, "type not set for {}", expr->toString());
    unify(typ, expr->type);
    if (disableActivation)
      ctx->allowActivation = oldActivation;
  }
  if (auto rt = realize(typ))
    unify(typ, rt);
  if (!expr->isType() && !allowVoid && expr->type && expr->type->is("void"))
    error("expression with void type");
  return expr;
}

ExprPtr TypecheckVisitor::transformType(ExprPtr &expr, bool disableActivation) {
  expr = transform(const_cast<ExprPtr &>(expr), true, false, disableActivation);
  if (expr) {
    TypePtr t = nullptr;
    if (!expr->isType()) {
      if (expr->isStatic())
        t = std::make_shared<StaticType>(expr, ctx);
      else
        error("expected type expression");
    } else {
      t = ctx->instantiate(expr.get(), expr->getType());
    }
    expr->setType(t);
  }
  return expr;
}

void TypecheckVisitor::defaultVisit(Expr *e) {
  seqassert(false, "unexpected AST node {}", e->toString());
}

/**************************************************************************************/

void TypecheckVisitor::visit(BoolExpr *expr) {
  unify(expr->type, ctx->findInternal("bool"));
  expr->done = true;
}

void TypecheckVisitor::visit(IntExpr *expr) {
  unify(expr->type, ctx->findInternal("int"));
  expr->done = true;
}

void TypecheckVisitor::visit(FloatExpr *expr) {
  unify(expr->type, ctx->findInternal("float"));
  expr->done = true;
}

void TypecheckVisitor::visit(StringExpr *expr) {
  unify(expr->type, ctx->findInternal("str"));
  expr->done = true;
}

void TypecheckVisitor::visit(IdExpr *expr) {
  if (startswith(expr->value, TYPE_TUPLE))
    generateTupleStub(std::stoi(expr->value.substr(7)));
  else if (startswith(expr->value, TYPE_FUNCTION))
    generateFunctionStub(std::stoi(expr->value.substr(10)));
  else if (expr->value == "Callable") { // Empty Callable references
    auto typ = ctx->addUnbound(expr, ctx->typecheckLevel);
    typ->getLink()->trait = std::make_shared<CallableTrait>(std::vector<TypePtr>{});
    unify(expr->type, typ);
    expr->markType();
    return;
  }
  auto val = ctx->find(expr->value);
  if (!val) {
    auto j = ctx->cache->globals.find(expr->value);
    if (j != ctx->cache->globals.end()) {
      auto typ = ctx->addUnbound(expr, ctx->typecheckLevel);
      unify(expr->type, typ);
      return;
    }
    auto i = ctx->cache->overloads.find(expr->value);
    if (i != ctx->cache->overloads.end()) {
      if (i->second.size() == 1) {
        val = ctx->find(i->second[0].name);
      } else {
        auto d = findDispatch(expr->value);
        val = ctx->find(d->ast->name);
      }
    }
  }
  seqassert(val, "cannot find IdExpr '{}' ({})", expr->value, expr->getSrcInfo());

  auto t = ctx->instantiate(expr, val->type);
  expr->type = unify(expr->type, t);
  if (val->type->isStaticType()) {
    expr->staticValue.type = StaticValue::Type(val->type->isStaticType());
    auto s = val->type->getStatic();
    seqassert(!expr->staticValue.evaluated, "expected unevaluated expression: {}",
              expr->toString());
    if (s && s->expr->staticValue.evaluated) {
      if (s->expr->staticValue.type == StaticValue::STRING)
        resultExpr = transform(N<StringExpr>(s->expr->staticValue.getString()));
      else
        resultExpr = transform(N<IntExpr>(s->expr->staticValue.getInt()));
    }
    return;
  } else if (val->isType()) {
    expr->markType();
  }

  // Check if we can realize the type.
  if (auto rt = realize(expr->type)) {
    unify(expr->type, rt);
    if (val->kind == TypecheckItem::Type || val->kind == TypecheckItem::Func)
      expr->value = expr->type->realizedName();
    expr->done = true;
  } else {
    expr->done = false;
  }
}

void TypecheckVisitor::visit(TupleExpr *expr) {
  for (int ai = 0; ai < expr->items.size(); ai++)
    if (auto es = const_cast<StarExpr *>(expr->items[ai]->getStar())) {
      // Case 1: *arg unpacking
      es->what = transform(es->what);
      auto t = es->what->type->getClass();
      if (!t)
        return;
      if (!t->getRecord())
        error("can only unpack tuple types");
      auto &ff = ctx->cache->classes[t->name].fields;
      for (int i = 0; i < t->getRecord()->args.size(); i++, ai++)
        expr->items.insert(expr->items.begin() + ai,
                           transform(N<DotExpr>(clone(es->what), ff[i].name)));
      expr->items.erase(expr->items.begin() + ai);
      ai--;
    } else {
      expr->items[ai] = transform(expr->items[ai]);
    }
  auto name = generateTupleStub(expr->items.size());
  resultExpr = transform(N<CallExpr>(N<DotExpr>(name, "__new__"), clone(expr->items)));
}

void TypecheckVisitor::visit(GeneratorExpr *expr) {
  seqassert(expr->kind == GeneratorExpr::Generator && expr->loops.size() == 1 &&
                expr->loops[0].conds.empty(),
            "invalid tuple generator");

  auto gen = transform(expr->loops[0].gen);
  if (!gen->type->getRecord())
    return; // continue after the iterator is realizable

  auto tuple = gen->type->getRecord();
  if (!startswith(tuple->name, TYPE_TUPLE))
    error("can only iterate over a tuple");

  auto block = N<SuiteStmt>();
  auto tupleVar = ctx->cache->getTemporaryVar("tuple");
  block->stmts.push_back(N<AssignStmt>(N<IdExpr>(tupleVar), gen));

  std::vector<ExprPtr> items;
  for (int ai = 0; ai < tuple->args.size(); ai++)
    items.emplace_back(
        N<StmtExpr>(N<AssignStmt>(clone(expr->loops[0].vars),
                                  N<IndexExpr>(N<IdExpr>(tupleVar), N<IntExpr>(ai))),
                    clone(expr->expr)));
  resultExpr = transform(N<StmtExpr>(block, N<TupleExpr>(items)));
}

void TypecheckVisitor::visit(IfExpr *expr) {
  expr->cond = transform(expr->cond);
  if (expr->cond->isStatic()) {
    if (expr->cond->staticValue.evaluated) {
      bool isTrue = false;
      if (expr->cond->staticValue.type == StaticValue::STRING)
        isTrue = !expr->cond->staticValue.getString().empty();
      else
        isTrue = expr->cond->staticValue.getInt();
      resultExpr =
          transform(isTrue ? expr->ifexpr : expr->elsexpr, false, allowVoidExpr);
      unify(expr->type, resultExpr->getType());
    } else {
      auto i = clone(expr->ifexpr), e = clone(expr->elsexpr);
      i = transform(i, false, allowVoidExpr, /*disableActivation*/ true);
      e = transform(e, false, allowVoidExpr, /*disableActivation*/ true);
      unify(expr->type, ctx->addUnbound(expr, ctx->typecheckLevel));
      if (i->isStatic() && e->isStatic()) {
        expr->staticValue.type = i->staticValue.type;
        unify(expr->type,
              ctx->findInternal(expr->staticValue.type == StaticValue::INT ? "int"
                                                                           : "str"));
      }
      expr->done = false; // do not typecheck this suite yet
    }
    return;
  }

  expr->ifexpr = transform(expr->ifexpr, false, allowVoidExpr);
  expr->elsexpr = transform(expr->elsexpr, false, allowVoidExpr);
  if (expr->cond->type->getClass() && !expr->cond->type->is("bool"))
    expr->cond = transform(N<CallExpr>(N<DotExpr>(expr->cond, "__bool__")));
  wrapOptionalIfNeeded(expr->ifexpr->getType(), expr->elsexpr);
  wrapOptionalIfNeeded(expr->elsexpr->getType(), expr->ifexpr);
  unify(expr->type, expr->ifexpr->getType());
  unify(expr->type, expr->elsexpr->getType());
  expr->ifexpr = transform(expr->ifexpr);
  expr->elsexpr = transform(expr->elsexpr);
  expr->done = expr->cond->done && expr->ifexpr->done && expr->elsexpr->done;
}

void TypecheckVisitor::visit(UnaryExpr *expr) {
  expr->expr = transform(expr->expr);
  if (expr->expr->isStatic()) {
    if (expr->expr->staticValue.type == StaticValue::STRING) {
      if (expr->op == "!") {
        if (expr->expr->staticValue.evaluated) {
          resultExpr =
              transform(N<BoolExpr>(expr->expr->staticValue.getString().empty()));
        } else {
          unify(expr->type, ctx->findInternal("bool"));
          if (!expr->isStatic())
            expr->staticValue.type = StaticValue::INT;
        }
        return;
      }
    } else if (expr->op == "-" || expr->op == "+" || expr->op == "!") {
      if (expr->expr->staticValue.evaluated) {
        int value = expr->expr->staticValue.getInt();
        if (expr->op == "+")
          ;
        else if (expr->op == "-")
          value = -value;
        else
          value = !bool(value);
        if (expr->op == "!")
          resultExpr = transform(N<BoolExpr>(bool(value)));
        else
          resultExpr = transform(N<IntExpr>(value));
      } else {
        unify(expr->type, ctx->findInternal("int"));
        if (!expr->isStatic())
          expr->staticValue.type = StaticValue::INT;
      }
      return;
    }
  }
  if (expr->op == "!") {
    resultExpr = transform(N<CallExpr>(N<DotExpr>(
        N<CallExpr>(N<DotExpr>(clone(expr->expr), "__bool__")), "__invert__")));
  } else {
    std::string magic;
    if (expr->op == "~")
      magic = "invert";
    else if (expr->op == "+")
      magic = "pos";
    else if (expr->op == "-")
      magic = "neg";
    else
      error("invalid unary operator '{}'", expr->op);
    magic = format("__{}__", magic);
    resultExpr = transform(N<CallExpr>(N<DotExpr>(clone(expr->expr), magic)));
  }
}

void TypecheckVisitor::visit(BinaryExpr *expr) {
  static std::unordered_map<StaticValue::Type, std::unordered_set<std::string>>
      staticOps = {
          {StaticValue::INT,
           {"<", "<=", ">", ">=", "==", "!=", "&&", "||", "+", "-", "*", "//", "%"}},
          {StaticValue::STRING, {"==", "!=", "+"}}};
  if (!(startswith(expr->op, "is") && expr->lexpr->getNone()))
    expr->lexpr = transform(expr->lexpr);
  if (!(startswith(expr->op, "is") && expr->rexpr->getNone()))
    expr->rexpr = transform(expr->rexpr);
  if (expr->lexpr->isStatic() && expr->rexpr->isStatic() &&
      expr->lexpr->staticValue.type == expr->rexpr->staticValue.type &&
      in(staticOps[expr->rexpr->staticValue.type], expr->op)) {
    if (expr->rexpr->staticValue.type == StaticValue::STRING) {
      if (expr->op == "+") {
        if (expr->lexpr->staticValue.evaluated && expr->rexpr->staticValue.evaluated) {
          resultExpr = transform(N<StringExpr>(expr->lexpr->staticValue.getString() +
                                               expr->rexpr->staticValue.getString()));
        } else {
          if (!expr->isStatic())
            expr->staticValue.type = StaticValue::STRING;
          unify(expr->type, ctx->findInternal("str"));
        }
      } else {
        if (expr->lexpr->staticValue.evaluated && expr->rexpr->staticValue.evaluated) {
          bool eq = expr->lexpr->staticValue.getString() ==
                    expr->rexpr->staticValue.getString();
          resultExpr = transform(N<BoolExpr>(expr->op == "==" ? eq : !eq));
        } else {
          if (!expr->isStatic())
            expr->staticValue.type = StaticValue::INT;
          unify(expr->type, ctx->findInternal("bool"));
        }
      }
    } else {
      if (expr->lexpr->staticValue.evaluated && expr->rexpr->staticValue.evaluated) {
        int64_t lvalue = expr->lexpr->staticValue.getInt();
        int64_t rvalue = expr->rexpr->staticValue.getInt();
        if (expr->op == "<")
          lvalue = lvalue < rvalue;
        else if (expr->op == "<=")
          lvalue = lvalue <= rvalue;
        else if (expr->op == ">")
          lvalue = lvalue > rvalue;
        else if (expr->op == ">=")
          lvalue = lvalue >= rvalue;
        else if (expr->op == "==")
          lvalue = lvalue == rvalue;
        else if (expr->op == "!=")
          lvalue = lvalue != rvalue;
        else if (expr->op == "&&")
          lvalue = lvalue && rvalue;
        else if (expr->op == "||")
          lvalue = lvalue || rvalue;
        else if (expr->op == "+")
          lvalue = lvalue + rvalue;
        else if (expr->op == "-")
          lvalue = lvalue - rvalue;
        else if (expr->op == "*")
          lvalue = lvalue * rvalue;
        else if (expr->op == "//") {
          if (!rvalue)
            error("static division by zero");
          lvalue = lvalue / rvalue;
        } else if (expr->op == "%") {
          if (!rvalue)
            error("static division by zero");
          lvalue = lvalue % rvalue;
        } else {
          seqassert(false, "unknown static operator {}", expr->op); // TODO!
        }
        if (in(std::set<std::string>{"==", "!=", "<", "<=", ">", ">=", "&&", "||"},
               expr->op))
          resultExpr = transform(N<BoolExpr>(bool(lvalue)));
        else
          resultExpr = transform(N<IntExpr>(lvalue));
      } else {
        if (!expr->isStatic())
          expr->staticValue.type = StaticValue::INT;
        unify(expr->type, ctx->findInternal("int"));
      }
    }
  } else {
    resultExpr = transformBinary(expr);
  }
}

void TypecheckVisitor::visit(PipeExpr *expr) {
  bool hasGenerator = false;

  // Returns T if t is of type Generator[T].
  auto getIterableType = [&](TypePtr t) {
    if (t->is("Generator")) {
      hasGenerator = true;
      return t->getClass()->generics[0].type;
    }
    return t;
  };
  // List of output types (for a|>b|>c, this list is type(a), type(a|>b),
  // type(a|>b|>c). These types are raw types (i.e. generator types are preserved).
  expr->inTypes.clear();
  expr->items[0].expr = transform(expr->items[0].expr);

  // The input type to the next stage.
  TypePtr inType = expr->items[0].expr->getType();
  expr->inTypes.push_back(inType);
  inType = getIterableType(inType);
  expr->done = expr->items[0].expr->done;
  for (int i = 1; i < expr->items.size(); i++) {
    ExprPtr prepend = nullptr; // An optional preceding stage
                               // (e.g. prepend  (|> unwrap |>) if an optional argument
                               //  needs unpacking).
    int inTypePos = -1;

    // Get the stage expression (take heed of StmtExpr!):
    auto ec = &expr->items[i].expr; // This is a pointer to a CallExprPtr
    while ((*ec)->getStmtExpr())
      ec = &const_cast<StmtExpr *>((*ec)->getStmtExpr())->expr;
    if (auto ecc = const_cast<CallExpr *>((*ec)->getCall())) {
      // Find the input argument position (a position of ... in the argument list):
      for (int ia = 0; ia < ecc->args.size(); ia++)
        if (auto ee = ecc->args[ia].value->getEllipsis()) {
          if (inTypePos == -1) {
            const_cast<EllipsisExpr *>(ee)->isPipeArg = true;
            inTypePos = ia;
            break;
          }
        }
      // If there is no ... in the argument list, use the first argument as the input
      // argument and add an ellipsis there
      if (inTypePos == -1) {
        ecc->args.insert(ecc->args.begin(), {"", N<EllipsisExpr>(true)});
        inTypePos = 0;
      }
    } else {
      // If this is not a CallExpr, make it a call expression with a single input
      // argument:
      expr->items[i].expr = N<CallExpr>(expr->items[i].expr, N<EllipsisExpr>(true));
      ec = &expr->items[i].expr;
      inTypePos = 0;
    }

    if (auto nn = transformCall((CallExpr *)(ec->get()), inType, &prepend))
      *ec = nn;
    if (prepend) { // Prepend the stage and rewind the loop (yes, the current
                   // expression will get parsed twice).
      expr->items.insert(expr->items.begin() + i, {"|>", prepend});
      i--;
      continue;
    }
    if ((*ec)->type)
      unify(expr->items[i].expr->type, (*ec)->type);
    expr->items[i].expr = *ec;
    inType = expr->items[i].expr->getType();
    if (auto rt = realize(inType))
      unify(inType, rt);
    else
      expr->done = false;
    expr->inTypes.push_back(inType);
    // Do not extract the generator type in the last stage of a pipeline.
    if (i < expr->items.size() - 1)
      inType = getIterableType(inType);
  }
  unify(expr->type, (hasGenerator ? ctx->findInternal("void") : inType));
}

void TypecheckVisitor::visit(InstantiateExpr *expr) {
  if (expr->typeExpr->getId() &&
      startswith(expr->typeExpr->getId()->value, TYPE_CALLABLE)) {
    std::vector<TypePtr> types;
    for (int i = 0; i < expr->typeParams.size(); i++) {
      expr->typeParams[i] = transformType(expr->typeParams[i]);
      if (expr->typeParams[i]->type->isStaticType())
        error("unexpected static type");
      types.push_back(expr->typeParams[i]->type);
    }
    auto typ = ctx->addUnbound(expr, ctx->typecheckLevel);
    typ->getLink()->trait = std::make_shared<CallableTrait>(types);
    unify(expr->type, typ);
  } else {
    expr->typeExpr = transformType(expr->typeExpr);
    TypePtr typ = ctx->instantiate(expr->typeExpr.get(), expr->typeExpr->getType());
    seqassert(typ->getClass(), "unknown type");
    auto &generics = typ->getClass()->generics;
    if (expr->typeParams.size() != generics.size())
      error("expected {} generics and/or statics", generics.size());

    for (int i = 0; i < expr->typeParams.size(); i++) {
      expr->typeParams[i] = transform(expr->typeParams[i], true);
      TypePtr t = nullptr;
      if (expr->typeParams[i]->isStatic()) {
        t = std::make_shared<StaticType>(expr->typeParams[i], ctx);
      } else {
        if (!expr->typeParams[i]->isType())
          error("expected type or static parameters");
        t = ctx->instantiate(expr->typeParams[i].get(), expr->typeParams[i]->getType());
      }
      unify(generics[i].type, t);
    }
    unify(expr->type, typ);
  }
  expr->markType();

  // If this is realizable, use the realized name (e.g. use Id("Ptr[byte]") instead of
  // Instantiate(Ptr, {byte})).
  if (auto rt = realize(expr->type)) {
    unify(expr->type, rt);
    resultExpr = N<IdExpr>(expr->type->realizedName());
    unify(resultExpr->type, rt);
    resultExpr->done = true;
    if (expr->typeExpr->isType())
      resultExpr->markType();
  }
}

void TypecheckVisitor::visit(SliceExpr *expr) {
  ExprPtr none = N<CallExpr>(N<DotExpr>(TYPE_OPTIONAL, "__new__"));
  resultExpr = transform(N<CallExpr>(
      N<IdExpr>(TYPE_SLICE), expr->start ? expr->start : clone(none),
      expr->stop ? expr->stop : clone(none), expr->step ? expr->step : clone(none)));
}

void TypecheckVisitor::visit(IndexExpr *expr) {
  if (expr->expr->isId("Static")) {
    TypePtr typ =
        ctx->addUnbound(expr, ctx->getLevel(), true, expr->index->isId("str") ? 1 : 2);
    unify(expr->type, typ);
    expr->done = true;
    return;
  }

  expr->expr = transform(expr->expr, true);
  auto typ = expr->expr->getType();
  seqassert(!expr->expr->isType(), "index not converted to instantiate");
  if (auto c = typ->getClass()) {
    // Case 1: check if this is a static tuple access...
    resultExpr = transformStaticTupleIndex(c.get(), expr->expr, expr->index);
    if (!resultExpr) {
      // Case 2: ... and if not, just call __getitem__.
      ExprPtr e = N<CallExpr>(N<DotExpr>(expr->expr, "__getitem__"), expr->index);
      resultExpr = transform(e, false, allowVoidExpr);
    }
  } else {
    // Case 3: type is still unknown.
    // expr->index = transform(expr->index);
    unify(expr->type, ctx->addUnbound(expr, ctx->typecheckLevel));
  }
}

void TypecheckVisitor::visit(DotExpr *expr) { resultExpr = transformDot(expr); }

void TypecheckVisitor::visit(CallExpr *expr) { resultExpr = transformCall(expr); }

void TypecheckVisitor::visit(EllipsisExpr *expr) {
  unify(expr->type, ctx->addUnbound(expr, ctx->typecheckLevel));
}

void TypecheckVisitor::visit(YieldExpr *expr) {
  seqassert(!ctx->bases.empty(), "yield outside of a function");
  auto typ = ctx->instantiateGeneric(expr, ctx->findInternal("Generator"),
                                     {ctx->addUnbound(expr, ctx->typecheckLevel)});
  unify(ctx->bases.back().returnType, typ);
  unify(expr->type, typ->getClass()->generics[0].type);
  expr->done = realize(expr->type) != nullptr;
}

void TypecheckVisitor::visit(StmtExpr *expr) {
  expr->done = true;
  for (auto &s : expr->stmts) {
    s = transform(s);
    expr->done &= s->done;
  }
  expr->expr = transform(expr->expr, false, allowVoidExpr);
  unify(expr->type, expr->expr->type);
  expr->done &= expr->expr->done;
}

/**************************************************************************************/

void TypecheckVisitor::wrapOptionalIfNeeded(const TypePtr &targetType, ExprPtr &e) {
  if (!targetType)
    return;
  auto t1 = targetType->getClass();
  auto t2 = e->getType()->getClass();
  if (t1 && t2 && t1->name == TYPE_OPTIONAL && t1->name != t2->name)
    e = transform(N<CallExpr>(N<IdExpr>(TYPE_OPTIONAL), e));
}

ExprPtr TypecheckVisitor::transformBinary(BinaryExpr *expr, bool isAtomic,
                                          bool *noReturn) {
  // Table of supported binary operations and the corresponding magic methods.
  auto magics = std::unordered_map<std::string, std::string>{
      {"+", "add"},     {"-", "sub"},       {"*", "mul"},     {"**", "pow"},
      {"/", "truediv"}, {"//", "floordiv"}, {"@", "matmul"},  {"%", "mod"},
      {"<", "lt"},      {"<=", "le"},       {">", "gt"},      {">=", "ge"},
      {"==", "eq"},     {"!=", "ne"},       {"<<", "lshift"}, {">>", "rshift"},
      {"&", "and"},     {"|", "or"},        {"^", "xor"},
  };
  if (noReturn)
    *noReturn = false;

  // Case 0: simple transformations
  if (expr->op == "&&") {
    return transform(N<IfExpr>(expr->lexpr,
                               N<CallExpr>(N<DotExpr>(expr->rexpr, "__bool__")),
                               N<BoolExpr>(false)));
  } else if (expr->op == "||") {
    return transform(N<IfExpr>(expr->lexpr, N<BoolExpr>(true),
                               N<CallExpr>(N<DotExpr>(expr->rexpr, "__bool__"))));
  } else if (expr->op == "not in") {
    return transform(N<CallExpr>(
        N<DotExpr>(N<CallExpr>(N<DotExpr>(expr->rexpr, "__contains__"), expr->lexpr),
                   "__invert__")));
  } else if (expr->op == "in") {
    return transform(N<CallExpr>(N<DotExpr>(expr->rexpr, "__contains__"), expr->lexpr));
  } else if (expr->op == "is") {
    if (expr->lexpr->getNone() && expr->rexpr->getNone())
      return transform(N<BoolExpr>(true));
    else if (expr->lexpr->getNone())
      return transform(N<BinaryExpr>(expr->rexpr, "is", expr->lexpr));
  } else if (expr->op == "is not") {
    return transform(N<UnaryExpr>("!", N<BinaryExpr>(expr->lexpr, "is", expr->rexpr)));
  }

  if (expr->lexpr->getType()->getUnbound() ||
      (expr->op != "is" && expr->rexpr->getType()->getUnbound())) {
    // Case 1: If operand types are unknown, continue later (no transformation).
    // Mark the type as unbound.
    unify(expr->type, ctx->addUnbound(expr, ctx->typecheckLevel));
    return nullptr;
  }

  // Check if this is a "a is None" expression. If so, ...
  if (expr->op == "is" && expr->rexpr->getNone()) {
    if (expr->lexpr->getType()->getClass()->name == "NoneType")
      return transform(N<BoolExpr>(true));
    if (expr->lexpr->getType()->getClass()->name != TYPE_OPTIONAL)
      // ... return False if lhs is not an Optional...
      return transform(N<BoolExpr>(false));
    else
      // ... or return lhs.__bool__.__invert__()
      return transform(N<CallExpr>(
          N<DotExpr>(N<CallExpr>(N<DotExpr>(expr->lexpr, "__bool__")), "__invert__")));
  }

  // Check the type equality (operand types and __raw__ pointers must match).
  // TODO: more special cases needed (Optional[T] == Optional[U] if both are None)
  if (expr->op == "is") {
    auto lc = realize(expr->lexpr->getType());
    auto rc = realize(expr->rexpr->getType());
    if (!lc || !rc) {
      // We still do not know the exact types...
      unify(expr->type, ctx->findInternal("bool"));
      return nullptr;
    } else if (!lc->getRecord() && !rc->getRecord()) {
      return transform(
          N<BinaryExpr>(N<CallExpr>(N<DotExpr>(expr->lexpr, "__raw__")),
                        "==", N<CallExpr>(N<DotExpr>(expr->rexpr, "__raw__"))));
    } else if (lc->getClass()->name == TYPE_OPTIONAL) {
      return transform(
          N<CallExpr>(N<DotExpr>(expr->lexpr, "__is_optional__"), expr->rexpr));
    } else if (rc->getClass()->name == TYPE_OPTIONAL) {
      return transform(
          N<CallExpr>(N<DotExpr>(expr->rexpr, "__is_optional__"), expr->lexpr));
    } else if (lc->realizedName() != rc->realizedName()) {
      return transform(N<BoolExpr>(false));
    } else {
      return transform(N<BinaryExpr>(expr->lexpr, "==", expr->rexpr));
    }
  }

  // Transform a binary expression to a magic method call.
  auto mi = magics.find(expr->op);
  if (mi == magics.end())
    error("invalid binary operator '{}'", expr->op);
  auto magic = mi->second;

  auto lt = expr->lexpr->getType()->getClass();
  auto rt = expr->rexpr->getType()->getClass();
  seqassert(lt && rt, "lhs and rhs types not known");

  FuncTypePtr method = nullptr;
  // Check if lt.__atomic_op__(Ptr[lt], rt) exists.
  if (isAtomic) {
    auto ptrlt =
        ctx->instantiateGeneric(expr->lexpr.get(), ctx->findInternal("Ptr"), {lt});
    method =
        findBestMethod(expr->lexpr.get(), format("__atomic_{}__", magic), {ptrlt, rt});
    if (method) {
      expr->lexpr = N<CallExpr>(N<IdExpr>("__ptr__"), expr->lexpr);
      if (noReturn)
        *noReturn = true;
    }
  }
  // Check if lt.__iop__(lt, rt) exists.
  if (!method && expr->inPlace) {
    method = findBestMethod(expr->lexpr.get(), format("__i{}__", magic), {lt, rt});
    if (method && noReturn)
      *noReturn = true;
  }
  // Check if lt.__op__(lt, rt) exists.
  if (!method)
    method = findBestMethod(expr->lexpr.get(), format("__{}__", magic), {lt, rt});
  // Check if rt.__rop__(rt, lt) exists.
  if (!method) {
    method = findBestMethod(expr->rexpr.get(), format("__r{}__", magic), {rt, lt});
    if (method)
      swap(expr->lexpr, expr->rexpr);
  }
  if (method) {
    return transform(
        N<CallExpr>(N<IdExpr>(method->ast->name), expr->lexpr, expr->rexpr));
  } else if (lt->is("pyobj")) {
    return transform(N<CallExpr>(N<CallExpr>(N<DotExpr>(expr->lexpr, "_getattr"),
                                             N<StringExpr>(format("__{}__", magic))),
                                 expr->rexpr));
  } else if (rt->is("pyobj")) {
    return transform(N<CallExpr>(N<CallExpr>(N<DotExpr>(expr->rexpr, "_getattr"),
                                             N<StringExpr>(format("__r{}__", magic))),
                                 expr->lexpr));
  }
  error("cannot find magic '{}' in {}", magic, lt->toString());
  return nullptr;
}

ExprPtr TypecheckVisitor::transformStaticTupleIndex(ClassType *tuple, ExprPtr &expr,
                                                    ExprPtr &index) {
  if (!tuple->getRecord())
    return nullptr;
  if (!startswith(tuple->name, TYPE_TUPLE) && !startswith(tuple->name, TYPE_PARTIAL))
    return nullptr;

  // Extract a static integer value from a compatible expression.
  auto getInt = [&](int64_t *o, const ExprPtr &e) {
    if (!e)
      return true;
    auto f = transform(clone(e));
    if (f->staticValue.type == StaticValue::INT) {
      seqassert(f->staticValue.evaluated, "{} not evaluated", e->toString());
      *o = f->staticValue.getInt();
      return true;
    }
    if (auto ei = f->getInt()) {
      *o = ei->intValue;
      return true;
    }
    return false;
  };

  auto classItem = ctx->cache->classes.find(tuple->name);
  seqassert(classItem != ctx->cache->classes.end(), "cannot find class '{}'",
            tuple->name);
  auto sz = classItem->second.fields.size();
  int64_t start = 0, stop = sz, step = 1;
  if (getInt(&start, index)) {
    int i = translateIndex(start, stop);
    if (i < 0 || i >= stop)
      error("tuple index out of range (expected 0..{}, got {})", stop, i);
    return transform(N<DotExpr>(expr, classItem->second.fields[i].name));
  } else if (auto es = CAST(index, SliceExpr)) {
    if (!getInt(&start, es->start) || !getInt(&stop, es->stop) ||
        !getInt(&step, es->step))
      return nullptr;
    // Correct slice indices.
    if (es->step && !es->start)
      start = step > 0 ? 0 : sz;
    if (es->step && !es->stop)
      stop = step > 0 ? sz : 0;
    sliceAdjustIndices(sz, &start, &stop, step);
    // Generate new tuple.
    std::vector<ExprPtr> te;
    for (auto i = start; (step >= 0) ? (i < stop) : (i >= stop); i += step) {
      if (i < 0 || i >= sz)
        error("tuple index out of range (expected 0..{}, got {})", sz, i);
      te.push_back(N<DotExpr>(clone(expr), classItem->second.fields[i].name));
    }
    return transform(
        N<CallExpr>(N<DotExpr>(format(TYPE_TUPLE "{}", te.size()), "__new__"), te));
  }
  return nullptr;
}

ExprPtr TypecheckVisitor::transformDot(DotExpr *expr,
                                       std::vector<CallExpr::Arg> *args) {
  if (expr->member == "__class__") {
    expr->expr = transform(expr->expr, true, true, true);
    unify(expr->type, ctx->findInternal("str"));
    if (auto f = expr->expr->type->getFunc())
      return transform(N<StringExpr>(f->toString()));
    if (auto t = realize(expr->expr->type))
      return transform(N<StringExpr>(t->toString()));
    expr->done = false;
    return nullptr;
  }
  expr->expr = transform(expr->expr, true);

  // Case 1: type not yet known, so just assign an unbound type and wait until the
  // next iteration.
  if (expr->expr->getType()->getUnbound()) {
    unify(expr->type, ctx->addUnbound(expr, ctx->typecheckLevel));
    return nullptr;
  }

  auto typ = expr->expr->getType()->getClass();
  seqassert(typ, "expected formed type: {}", typ->toString());

  if (startswith(typ->name, TYPE_FUNCTION) && expr->member == "__call__")
    generateFnCall(typ->generics.size() - 1);
  auto methods = ctx->findMethod(typ->name, expr->member);
  if (methods.empty()) {
    auto findGeneric = [this](ClassType *c, const std::string &m) -> TypePtr {
      for (auto &g : c->generics) {
        if (ctx->cache->reverseIdentifierLookup[g.name] == m)
          return g.type;
      }
      return nullptr;
    };
    if (auto member = ctx->findMember(typ->name, expr->member)) {
      // Case 2(a): Object member access.
      auto t = ctx->instantiate(expr, member, typ.get());
      unify(expr->type, t);
      expr->done = expr->expr->done && realize(expr->type) != nullptr;
      return nullptr;
    } else if (auto t = findGeneric(typ.get(), expr->member)) {
      // Case 2(b): Object generic access.
      unify(expr->type, t);
      if (!t->isStaticType())
        expr->markType();
      else
        expr->staticValue.type = StaticValue::Type(t->isStaticType());
      if (auto rt = realize(expr->type)) {
        unify(expr->type, rt);
        ExprPtr e;
        if (!t->isStaticType())
          e = N<IdExpr>(t->realizedName());
        else if (t->getStatic()->expr->staticValue.type == StaticValue::STRING)
          e = transform(N<StringExpr>(t->getStatic()->expr->staticValue.getString()));
        else
          e = transform(N<IntExpr>(t->getStatic()->expr->staticValue.getInt()));
        return transform(e);
      }
      return nullptr;
    } else if (typ->name == TYPE_OPTIONAL) {
      // Case 3: Transform optional.member to unwrap(optional).member.
      auto d = N<DotExpr>(transform(N<CallExpr>(N<IdExpr>(FN_UNWRAP), expr->expr)),
                          expr->member);
      if (auto dd = transformDot(d.get(), args))
        return dd;
      return d;
    } else if (typ->name == "pyobj") {
      // Case 4: Transform pyobj.member to pyobj._getattr("member").
      return transform(
          N<CallExpr>(N<DotExpr>(expr->expr, "_getattr"), N<StringExpr>(expr->member)));
    } else {
      // For debugging purposes: ctx->findMethod(typ->name, expr->member);
      error("cannot find '{}' in {}", expr->member, typ->toString());
    }
  }

  // Case 5: look for a method that best matches the given arguments.
  //         If it exists, return a simple IdExpr with that method's name.
  //         Append a "self" variable to the front if needed.
  if (args) {
    std::vector<CallExpr::Arg> argTypes;
    bool isType = expr->expr->isType();
    if (!isType) {
      ExprPtr expr = N<IdExpr>("self");
      expr->setType(typ);
      argTypes.emplace_back(CallExpr::Arg{"", expr});
    }
    for (const auto &a : *args)
      argTypes.emplace_back(a);
    if (auto bestMethod = findBestMethod(expr->expr.get(), expr->member, argTypes)) {
      ExprPtr e = N<IdExpr>(bestMethod->ast->name);
      auto t = ctx->instantiate(expr, bestMethod, typ.get());
      unify(e->type, t);
      unify(expr->type, e->type);
      if (!isType)
        args->insert(args->begin(), {"", expr->expr}); // self variable
      e = transform(e); // Visit IdExpr and realize it if necessary.
      // Remove lingering unbound variables from expr->expr (typ) instantiation
      // if we accessed a method that does not reference any generic in typ.
      if (isType && !bestMethod->ast->attributes.has(Attr::Method))
        deactivateUnbounds(typ.get());
      return e;
    }
    // No method was found, print a nice error message.
    std::vector<std::string> nice;
    for (auto &t : argTypes)
      nice.emplace_back(format("{} = {}", t.name, t.value->type->toString()));
    findBestMethod(expr->expr.get(), expr->member, argTypes);
    error("cannot find a method '{}' in {} with arguments {}", expr->member,
          typ->toString(), join(nice, ", "));
  }

  // Case 6: multiple overloaded methods available.
  FuncTypePtr bestMethod = nullptr;
  auto oldType = expr->getType() ? expr->getType()->getClass() : nullptr;
  if (methods.size() > 1 && oldType && oldType->getFunc()) {
    // If old type is already a function, use its arguments to pick the best call.
    std::vector<TypePtr> methodArgs;
    if (!expr->expr->isType()) // self argument
      methodArgs.emplace_back(typ);
    for (auto i = 1; i < oldType->generics.size(); i++)
      methodArgs.emplace_back(oldType->generics[i].type);
    bestMethod = findBestMethod(expr->expr.get(), expr->member, methodArgs);
    if (!bestMethod) {
      // Print a nice error message.
      std::vector<std::string> nice;
      for (auto &t : methodArgs)
        nice.emplace_back(format("{}", t->toString()));
      error("cannot find a method '{}' in {} with arguments {}", expr->member,
            typ->toString(), join(nice, ", "));
    }
  } else if (methods.size() > 1) {
    auto m = ctx->cache->classes.find(typ->name);
    auto t = m->second.methods.find(expr->member);
    bestMethod = findDispatch(t->second);
  } else {
    bestMethod = methods[0];
  }

  // Case 7: only one valid method remaining. Check if this is a class method or an
  // object method access and transform accordingly.
  if (expr->expr->isType()) {
    // Class method access: Type.method.
    auto name = bestMethod->ast->name;
    auto val = ctx->find(name);
    seqassert(val, "cannot find method '{}'", name);
    ExprPtr e = N<IdExpr>(name);
    auto t = ctx->instantiate(expr, bestMethod, typ.get());
    unify(e->type, t);
    unify(expr->type, e->type);
    e = transform(e); // Visit IdExpr and realize it if necessary.
    // Remove lingering unbound variables from expr->expr (typ) instantiation
    // if we accessed a method that does not reference any generic in typ.
    if (!bestMethod->ast->attributes.has(Attr::Method))
      deactivateUnbounds(typ.get());
    return e;
  } else {
    // Object access: y.method. Transform y.method to a partial call
    // typeof(t).foo(y, ...).
    std::vector<ExprPtr> methodArgs{expr->expr};
    methodArgs.push_back(N<EllipsisExpr>());
    // Handle @property methods.
    if (bestMethod->ast->attributes.has(Attr::Property))
      methodArgs.pop_back();
    ExprPtr e = N<CallExpr>(N<IdExpr>(bestMethod->ast->name), methodArgs);
    auto ex = transform(e, false, allowVoidExpr);
    return ex;
  }
}

ExprPtr TypecheckVisitor::transformCall(CallExpr *expr, const types::TypePtr &inType,
                                        ExprPtr *extraStage) {
  // keep the old expression if we end up with an extra stage
  ExprPtr oldExpr = extraStage ? expr->clone() : nullptr;

  if (!callTransformCallArgs(expr->args, inType))
    return nullptr;

  PartialCallData part{!expr->args.empty() && expr->args.back().value->getEllipsis() &&
                       !expr->args.back().value->getEllipsis()->isPipeArg &&
                       expr->args.back().name.empty()};
  expr->expr = callTransformCallee(expr->expr, expr->args, part);

  auto callee = expr->expr->getType()->getClass();
  FuncTypePtr calleeFn = callee ? callee->getFunc() : nullptr;
  if (!callee) {
    // Case 1: Unbound callee, will be resolved later.
    unify(expr->type, ctx->addUnbound(expr, ctx->typecheckLevel));
    return nullptr;
  } else if (expr->expr->isType()) {
    if (callee->getRecord()) {
      // Case 2a: Tuple constructor. Transform to: t.__new__(args)
      return transform(N<CallExpr>(N<DotExpr>(expr->expr, "__new__"), expr->args));
    } else {
      // Case 2b: Type constructor. Transform to a StmtExpr:
      //   c = t.__new__(); c.__init__(args); c
      ExprPtr var = N<IdExpr>(ctx->cache->getTemporaryVar("v"));
      return transform(N<StmtExpr>(
          N<SuiteStmt>(
              N<AssignStmt>(clone(var), N<CallExpr>(N<DotExpr>(expr->expr, "__new__"))),
              N<ExprStmt>(N<CallExpr>(N<IdExpr>("std.internal.gc.register_finalizer"),
                                      clone(var))),
              N<ExprStmt>(N<CallExpr>(N<DotExpr>(clone(var), "__init__"), expr->args))),
          clone(var)));
    }
  } else if (auto pc = callee->getPartial()) {
    ExprPtr var = N<IdExpr>(part.var = ctx->cache->getTemporaryVar("pt"));
    expr->expr = transform(N<StmtExpr>(N<AssignStmt>(clone(var), expr->expr),
                                       N<IdExpr>(pc->func->ast->name)));
    calleeFn = expr->expr->type->getFunc();
    // Fill in generics
    for (int i = 0, j = 0; i < pc->known.size(); i++)
      if (pc->func->ast->args[i].generic) {
        if (pc->known[i])
          unify(calleeFn->funcGenerics[j].type,
                ctx->instantiate(expr, pc->func->funcGenerics[j].type));
        j++;
      }
    part.known = pc->known;
    seqassert(calleeFn, "not a function: {}", expr->expr->type->toString());
  } else if (!callee->getFunc()) {
    // Case 3: callee is not a named function. Route it through a __call__ method.
    ExprPtr newCall = N<CallExpr>(N<DotExpr>(expr->expr, "__call__"), expr->args);
    return transform(newCall, false, allowVoidExpr);
  }

  // Handle named and default arguments
  int ellipsisStage = -1;
  if (auto e = callReorderArguments(callee, expr, ellipsisStage, part))
    return e;

  auto special = transformSpecialCall(expr);
  if (special.first) {
    deactivateUnbounds(expr->expr->type.get());
    deactivateUnbounds(expr->type.get());
    return special.second;
  }

  // Check if ellipsis is in the *args/*kwArgs
  if (extraStage && ellipsisStage != -1) {
    *extraStage = expr->args[ellipsisStage].value;
    expr->args[ellipsisStage].value = N<EllipsisExpr>();
    const_cast<CallExpr *>(oldExpr->getCall())->args = expr->args;
    const_cast<CallExpr *>(oldExpr->getCall())->ordered = true;
    deactivateUnbounds(calleeFn.get());
    return oldExpr;
  }

  bool unificationsDone = true;
  std::vector<TypePtr> replacements(calleeFn->args.size() - 1, nullptr);
  for (int si = 0; si < calleeFn->args.size() - 1; si++) {
    bool isPipeArg = extraStage && expr->args[si].value->getEllipsis();
    auto orig = expr->args[si].value.get();
    if (!wrapExpr(expr->args[si].value, calleeFn->args[si + 1], calleeFn))
      unificationsDone = false;

    replacements[si] = !calleeFn->args[si + 1]->getClass() ? expr->args[si].value->type
                                                           : calleeFn->args[si + 1];
    if (isPipeArg && orig != expr->args[si].value.get()) {
      *extraStage = expr->args[si].value;
      return oldExpr;
    }
  }

  // Realize arguments.
  expr->done = true;
  for (auto &a : expr->args) {
    if (auto rt = realize(a.value->type)) {
      unify(rt, a.value->type);
      a.value = transform(a.value);
    }
    expr->done &= a.value->done;
  }

  // Handle default generics (calleeFn.g. foo[S, T=int]) only if all arguments were
  // unified.
  // TODO: remove once the proper partial handling of overloaded functions land
  if (unificationsDone) {
    for (int i = 0, j = 0; i < calleeFn->ast->args.size(); i++)
      if (calleeFn->ast->args[i].generic) {
        if (calleeFn->ast->args[i].deflt &&
            calleeFn->funcGenerics[j].type->getUnbound()) {
          auto de = transform(calleeFn->ast->args[i].deflt, true);
          TypePtr t = nullptr;
          if (de->isStatic())
            t = std::make_shared<StaticType>(de, ctx);
          else
            t = de->getType();
          unify(calleeFn->funcGenerics[j].type, t);
        }
        j++;
      }
  }
  for (int si = 0; si < replacements.size(); si++)
    if (replacements[si]) {
      if (replacements[si]->getFunc())
        deactivateUnbounds(replacements[si].get());
      calleeFn->generics[si + 1].type = calleeFn->args[si + 1] = replacements[si];
    }
  if (!part.isPartial) {
    if (auto rt = realize(calleeFn)) {
      unify(rt, std::static_pointer_cast<Type>(calleeFn));
      expr->expr = transform(expr->expr);
    }
  }
  expr->done &= expr->expr->done;

  // Emit the final call.
  if (part.isPartial) {
    // Case 1: partial call.
    // Transform calleeFn(args...) to Partial.N<known>.<calleeFn>(args...).
    auto partialTypeName = generatePartialStub(part.known, calleeFn->getFunc().get());
    deactivateUnbounds(calleeFn.get());
    std::vector<ExprPtr> newArgs;
    for (auto &r : expr->args)
      if (!r.value->getEllipsis()) {
        newArgs.push_back(r.value);
        newArgs.back()->setAttr(ExprAttr::SequenceItem);
      }
    newArgs.push_back(part.args);
    newArgs.push_back(part.kwArgs);

    std::string var = ctx->cache->getTemporaryVar("partial");
    ExprPtr call = nullptr;
    if (!part.var.empty()) {
      auto stmts = const_cast<StmtExpr *>(expr->expr->getStmtExpr())->stmts;
      stmts.push_back(N<AssignStmt>(N<IdExpr>(var),
                                    N<CallExpr>(N<IdExpr>(partialTypeName), newArgs)));
      call = N<StmtExpr>(stmts, N<IdExpr>(var));
    } else {
      call =
          N<StmtExpr>(N<AssignStmt>(N<IdExpr>(var),
                                    N<CallExpr>(N<IdExpr>(partialTypeName), newArgs)),
                      N<IdExpr>(var));
    }
    call->setAttr(ExprAttr::Partial);
    call = transform(call, false, allowVoidExpr);
    seqassert(call->type->getPartial(), "expected partial type");
    return call;
  } else {
    // Case 2. Normal function call.
    unify(expr->type, calleeFn->args[0]); // function return type
    return nullptr;
  }
}

std::pair<bool, ExprPtr> TypecheckVisitor::transformSpecialCall(CallExpr *expr) {
  if (!expr->expr->getId())
    return {false, nullptr};

  auto val = expr->expr->getId()->value;
  if (val == "superf") { // TODO
    if (ctx->bases.back().supers.empty())
      error("no matching superf methods are available");
    auto parentCls = ctx->bases.back().type->getFunc()->funcParent;
    auto m =
        findMatchingMethods(parentCls ? CAST(parentCls, types::ClassType) : nullptr,
                            ctx->bases.back().supers, expr->args);
    if (m.empty())
      error("no matching superf methods are available");
    ExprPtr e = N<CallExpr>(N<IdExpr>(m[0]->ast->name), expr->args);
    return {true, transform(e, false, true)};
  } else if (val == "super") {
    return {true, transformSuper(expr)};
  } else if (val == "__ptr__") {
    auto id = expr->args[0].value->getId();
    auto v = id ? ctx->find(id->value) : nullptr;
    if (v && v->kind == TypecheckItem::Var) {
      expr->args[0].value = transform(expr->args[0].value);
      auto t = ctx->instantiateGeneric(expr, ctx->findInternal("Ptr"),
                                       {expr->args[0].value->type});
      unify(expr->type, t);
      expr->done = expr->args[0].value->done;
      return {true, nullptr};
    } else {
      error("__ptr__ only accepts a variable identifier");
    }
  } else if (val == "__array__.__new__:0") {
    seqassert(false, "not yet implemented");
  } else if (val == "isinstance") {
    // Make sure not to activate new unbound here, as we just need to check type
    // equality.
    expr->staticValue.type = StaticValue::INT;
    expr->type = unify(expr->type, ctx->findInternal("bool"));
    expr->args[0].value = transform(expr->args[0].value, true, true);
    auto typ = expr->args[0].value->type->getClass();
    if (!typ || !typ->canRealize()) {
      return {true, nullptr};
    } else {
      expr->args[0].value = transform(expr->args[0].value, true, true); // to realize it
      deactivateUnbounds(expr->args[0].value);
      if (expr->args[1].value->isId("Tuple") || expr->args[1].value->isId("tuple")) {
        return {true, transform(N<BoolExpr>(startswith(typ->name, TYPE_TUPLE)))};
      } else if (expr->args[1].value->isId("ByVal")) {
        return {true, transform(N<BoolExpr>(typ->getRecord() != nullptr))};
      } else if (expr->args[1].value->isId("ByRef")) {
        return {true, transform(N<BoolExpr>(typ->getRecord() == nullptr))};
      } else {
        expr->args[1].value = transformType(expr->args[1].value);
        auto t = expr->args[1].value->type;
        deactivateUnbounds(t.get());
        auto hierarchy = getSuperTypes(typ->getClass());

        for (auto &tx : hierarchy) {
          auto unifyOK = tx->unify(t.get(), nullptr) >= 0;
          if (unifyOK) {
            return {true, transform(N<BoolExpr>(true))};
          }
        }
        return {true, transform(N<BoolExpr>(false))};
      }
    }
  } else if (val == "staticlen") {
    expr->staticValue.type = StaticValue::INT;
    expr->args[0].value = transform(expr->args[0].value);
    auto typ = expr->args[0].value->getType();
    if (auto s = typ->getStatic()) {
      if (s->expr->staticValue.type != StaticValue::STRING)
        error("expected a static string");
      if (!s->expr->staticValue.evaluated)
        return {true, nullptr};
      return {true, transform(N<IntExpr>(s->expr->staticValue.getString().size()))};
    }
    if (!typ->getClass())
      return {true, nullptr};
    else if (!typ->getRecord())
      error("{} is not a tuple type", typ->toString());
    else
      return {true, transform(N<IntExpr>(typ->getRecord()->args.size()))};
  } else if (val == "hasattr") {
    expr->staticValue.type = StaticValue::INT;
    auto typ = expr->args[0].value->getType()->getClass();
    if (!typ)
      return {true, nullptr};
    auto member = expr->expr->type->getFunc()
                      ->funcGenerics[0]
                      .type->getStatic()
                      ->evaluate()
                      .getString();
    std::vector<TypePtr> args{typ};
    seqassert(expr->args[1].value->type->getClass()->name == "Tuple.N0",
              "not yet implm: {}", expr->toString());
    // for (int i = 1; i < expr->args.size(); i++) {
    //   expr->args[i].value = transformType(expr->args[i].value);
    //   if (!expr->args[i].value->getType()->getClass())
    //     return {true, nullptr};
    //   args.push_back(expr->args[i].value->getType());
    // }
    bool exists = !ctx->findMethod(typ->getClass()->name, member).empty() ||
                  ctx->findMember(typ->getClass()->name, member);
    if (exists && args.size() > 1)
      exists &= findBestMethod(expr->args[0].value.get(), member, args) != nullptr;
    deactivateUnbounds(expr->args[0].value);
    return {true, transform(N<BoolExpr>(exists))};
  } else if (val == "compile_error") {
    expr->args[0].value = transform(expr->args[0].value);
    if (expr->args[0].value->staticValue.type != StaticValue::STRING)
      error("expected static string");
    if (!expr->args[0].value->staticValue.evaluated)
      return {true, nullptr};
    error("custom error: {}", expr->args[0].value->staticValue.getString());
  } else if (val == "type.__new__:0") {
    expr->args[0].value = transform(expr->args[0].value);
    unify(expr->type, expr->args[0].value->getType());
    if (auto rt = realize(expr->type)) {
      unify(expr->type, rt);
      auto resultExpr = N<IdExpr>(expr->type->realizedName());
      unify(resultExpr->type, expr->type);
      resultExpr->done = true;
      resultExpr->markType();
      return {true, resultExpr};
    }
    return {true, nullptr};
  } else if (val == "getattr") {
    expr->args[1].value = transform(expr->args[1].value);
    if (expr->args[1].value->staticValue.type != StaticValue::STRING)
      error("expected static string");
    if (!expr->args[1].value->staticValue.evaluated)
      return {true, nullptr};
    return {true, transform(N<DotExpr>(expr->args[0].value,
                                       expr->args[1].value->staticValue.getString()))};
  }
  return {false, nullptr};
}

void TypecheckVisitor::addFunctionGenerics(const FuncType *t) {
  for (auto p = t->funcParent; p;) {
    if (auto f = p->getFunc()) {
      for (auto &g : f->funcGenerics)
        ctx->add(TypecheckItem::Type, g.name, g.type);
      p = f->funcParent;
    } else {
      auto c = p->getClass();
      seqassert(c, "not a class: {}", p->toString());
      for (auto &g : c->generics)
        ctx->add(TypecheckItem::Type, g.name, g.type);
      break;
    }
  }
  for (auto &g : t->funcGenerics)
    ctx->add(TypecheckItem::Type, g.name, g.type);
}

std::string TypecheckVisitor::generateTupleStub(int len, const std::string &name,
                                                std::vector<std::string> names,
                                                bool hasSuffix) {
  static std::map<std::string, int> usedNames;
  auto key = join(names, ";");
  std::string suffix;
  if (!names.empty()) {
    if (!in(usedNames, key))
      usedNames[key] = usedNames.size();
    suffix = format("_{}", usedNames[key]);
  } else {
    for (int i = 1; i <= len; i++)
      names.push_back(format("item{}", i));
  }
  auto typeName = format("{}{}", name, hasSuffix ? format(".N{}{}", len, suffix) : "");
  if (!ctx->find(typeName)) {
    std::vector<Param> args;
    for (int i = 1; i <= len; i++)
      args.emplace_back(Param(names[i - 1], N<IdExpr>(format("T{}", i)), nullptr));
    for (int i = 1; i <= len; i++)
      args.emplace_back(Param(format("T{}", i), N<IdExpr>("type"), nullptr, true));
    StmtPtr stmt =
        std::make_shared<ClassStmt>(typeName, args, nullptr, Attr({Attr::Tuple}));
    stmt->setSrcInfo(ctx->cache->generateSrcInfo());

    stmt = SimplifyVisitor::apply(ctx->cache->imports[STDLIB_IMPORT].ctx, stmt,
                                  FILE_GENERATED, 0);
    stmt = TypecheckVisitor(ctx).transform(stmt);
    prependStmts->push_back(stmt);
  }
  return typeName;
}

std::string TypecheckVisitor::generateFunctionStub(int n) {
  seqassert(n >= 0, "invalid n");
  auto typeName = format(TYPE_FUNCTION "{}", n);
  if (!ctx->find(typeName)) {
    std::vector<ExprPtr> genericNames{N<IdExpr>("TR")};
    // TODO: remove this args hack
    std::vector<Param> args;
    args.emplace_back(Param{"ret", N<IdExpr>("TR"), nullptr});
    for (int i = 1; i <= n; i++) {
      genericNames.emplace_back(N<IdExpr>(format("T{}", i)));
      args.emplace_back(Param{format("a{}", i), N<IdExpr>(format("T{}", i)), nullptr});
    }
    for (auto &g : genericNames)
      args.emplace_back(Param(g->getId()->value, N<IdExpr>("type"), nullptr, true));
    ExprPtr type = N<IndexExpr>(N<IdExpr>(typeName), N<TupleExpr>(genericNames));

    std::vector<StmtPtr> fns;
    std::vector<Param> params;
    std::vector<StmtPtr> stmts;
    // def __new__(what: Ptr[byte]) -> Function.N[TR, T1, ..., TN]:
    //   return __internal__.fn_new[Function.N[TR, ...]](what)
    params.emplace_back(
        Param{"what", N<IndexExpr>(N<IdExpr>("Ptr"), N<IdExpr>("byte"))});
    stmts.push_back(N<ReturnStmt>(N<CallExpr>(N<DotExpr>("__internal__", "fn_new"),
                                              N<IdExpr>("what"), clone(type))));
    fns.emplace_back(std::make_shared<FunctionStmt>("__new__", clone(type), params,
                                                    N<SuiteStmt>(stmts)));
    params.clear();
    stmts.clear();
    // def __new__(what: Function.N[TR, T1, ..., TN]) -> Function.N[TR, T1, ..., TN]:
    //   return what
    params.emplace_back(Param{"what", clone(type)});
    fns.emplace_back(
        std::make_shared<FunctionStmt>("__new__", clone(type), params,
                                       N<SuiteStmt>(N<ReturnStmt>(N<IdExpr>("what")))));
    params.clear();
    // def __raw__(self: Function.N[TR, T1, ..., TN]) -> Ptr[byte]:
    //   return __internal__.fn_raw(self)
    params.emplace_back(Param{"self", clone(type)});
    stmts.push_back(N<ReturnStmt>(
        N<CallExpr>(N<DotExpr>("__internal__", "fn_raw"), N<IdExpr>("self"))));
    fns.emplace_back(std::make_shared<FunctionStmt>(
        "__raw__", N<IndexExpr>(N<IdExpr>("Ptr"), N<IdExpr>("byte")), params,
        N<SuiteStmt>(stmts)));
    params.clear();
    stmts.clear();
    // def __repr__(self: Function.N[TR, T1, ..., TN]) -> str:
    //   return __internal__.raw_type_str(self.__raw__(), "function")
    params.emplace_back(Param{"self", clone(type)});
    stmts.push_back(N<ReturnStmt>(N<CallExpr>(
        N<DotExpr>("__internal__", "raw_type_str"),
        N<CallExpr>(N<DotExpr>("self", "__raw__")), N<StringExpr>("function"))));
    fns.emplace_back(
        N<FunctionStmt>("__repr__", N<IdExpr>("str"), params, N<SuiteStmt>(stmts)));
    params.clear();
    stmts.clear();
    // class Function.N[TR, T1, ..., TN]
    StmtPtr stmt = std::make_shared<ClassStmt>(typeName, args, N<SuiteStmt>(fns),
                                               Attr({Attr::Internal, Attr::Tuple}));
    stmt->setSrcInfo(ctx->cache->generateSrcInfo());

    // Parse this function in a clean context.
    stmt = SimplifyVisitor::apply(ctx->cache->imports[STDLIB_IMPORT].ctx, stmt,
                                  FILE_GENERATED, 0);
    stmt = TypecheckVisitor(ctx).transform(stmt);
    prependStmts->push_back(stmt);
  }
  return typeName;
}

std::string TypecheckVisitor::generatePartialStub(const std::vector<char> &mask,
                                                  types::FuncType *fn) {
  std::string strMask(mask.size(), '1');
  int tupleSize = 0, genericSize = 0;
  for (int i = 0; i < mask.size(); i++)
    if (!mask[i])
      strMask[i] = '0';
    else if (!fn->ast->args[i].generic)
      tupleSize++;
    else
      genericSize++;
  auto typeName = format(TYPE_PARTIAL "{}.{}", strMask, fn->toString());
  if (!ctx->find(typeName)) {
    ctx->cache->partials[typeName] = {fn->generalize(0)->getFunc(), mask};
    generateTupleStub(tupleSize + 2, typeName, {}, false);
  }
  return typeName;
}

void TypecheckVisitor::generateFnCall(int n) {
  // @llvm
  // def __call__(self: Function.N[TR, T1, ..., TN], a1: T1, ..., aN: TN) -> TR:
  //   %0 = call {=TR} %self({=T1} %a1, ..., {=TN} %aN)
  //   ret {=TR} %0
  //  std::string name = ctx->cache->generateSrcInfo()

  auto typeName = format(TYPE_FUNCTION "{}", n);
  if (!in(ctx->cache->classes[typeName].methods, "__call__")) {
    std::vector<Param> params;
    std::vector<StmtPtr> fns;
    params.emplace_back(Param{"self", nullptr});
    std::vector<std::string> llvmArgs;
    std::vector<CallExpr::Arg> callArgs;
    for (int i = 1; i <= n; i++) {
      llvmArgs.emplace_back(format("{{=T{}}} %a{}", i, i));
      params.emplace_back(Param{format("a{}", i), N<IdExpr>(format("T{}", i))});
      callArgs.emplace_back(CallExpr::Arg{"", N<IdExpr>(format("a{}", i))});
    }
    std::string llvmNonVoid =
        format("%0 = call {{=TR}} %self({})\nret {{=TR}} %0", join(llvmArgs, ", "));
    std::string llvmVoid =
        format("call {{=TR}} %self({})\nret void", join(llvmArgs, ", "));
    fns.emplace_back(std::make_shared<FunctionStmt>(
        "__call__.void", N<IdExpr>("TR"), clone_nop(params),
        N<SuiteStmt>(N<ExprStmt>(N<StringExpr>(llvmVoid))), Attr({Attr::LLVM})));
    fns.emplace_back(std::make_shared<FunctionStmt>(
        "__call__.ret", N<IdExpr>("TR"), clone_nop(params),
        N<SuiteStmt>(N<ExprStmt>(N<StringExpr>(llvmNonVoid))), Attr({Attr::LLVM})));
    fns.emplace_back(std::make_shared<FunctionStmt>(
        "__call__", N<IdExpr>("TR"), clone_nop(params),
        N<SuiteStmt>(N<IfStmt>(
            N<CallExpr>(N<IdExpr>("isinstance"), N<IdExpr>("TR"), N<IdExpr>("void")),
            N<ExprStmt>(
                N<CallExpr>(N<DotExpr>("self", "__call__.void"), clone_nop(callArgs))),
            N<ReturnStmt>(N<CallExpr>(N<DotExpr>("self", "__call__.ret"),
                                      clone_nop(callArgs)))))));
    // Parse this function in a clean context.

    StmtPtr stmt = N<ClassStmt>(typeName, std::vector<Param>{}, N<SuiteStmt>(fns),
                                Attr({Attr::Extend}));
    stmt = SimplifyVisitor::apply(ctx->cache->imports[STDLIB_IMPORT].ctx, stmt,
                                  FILE_GENERATED, 0);
    stmt = TypecheckVisitor(ctx).transform(stmt);
    prependStmts->push_back(stmt);
  }
}

ExprPtr TypecheckVisitor::partializeFunction(ExprPtr expr) {
  auto fn = expr->getType()->getFunc();
  seqassert(fn, "not a function: {}", expr->getType()->toString());
  std::vector<char> mask(fn->ast->args.size(), 0);
  for (int i = 0, j = 0; i < fn->ast->args.size(); i++)
    if (fn->ast->args[i].generic) {
      // TODO: better detection of user-provided args...?
      if (!fn->funcGenerics[j].type->getUnbound())
        mask[i] = 1;
      j++;
    }
  auto partialTypeName = generatePartialStub(mask, fn.get());
  deactivateUnbounds(fn.get());
  std::string var = ctx->cache->getTemporaryVar("partial");
  auto kwName = generateTupleStub(0, "KwTuple", {});
  ExprPtr call =
      N<StmtExpr>(N<AssignStmt>(N<IdExpr>(var),
                                N<CallExpr>(N<IdExpr>(partialTypeName), N<TupleExpr>(),
                                            N<CallExpr>(N<IdExpr>(kwName)))),
                  N<IdExpr>(var));
  call->setAttr(ExprAttr::Partial);
  call = transform(call, false, allowVoidExpr);
  seqassert(call->type->getPartial(), "expected partial type");
  return call;
}

types::FuncTypePtr
TypecheckVisitor::findBestMethod(const Expr *expr, const std::string &member,
                                 const std::vector<types::TypePtr> &args) {
  std::vector<CallExpr::Arg> callArgs;
  for (auto &a : args) {
    callArgs.push_back({"", std::make_shared<NoneExpr>()}); // dummy expression
    callArgs.back().value->setType(a);
  }
  return findBestMethod(expr, member, callArgs);
}

types::FuncTypePtr
TypecheckVisitor::findBestMethod(const Expr *expr, const std::string &member,
                                 const std::vector<CallExpr::Arg> &args) {
  auto typ = expr->getType()->getClass();
  seqassert(typ, "not a class");
  auto methods = ctx->findMethod(typ->name, member, false);
  auto m = findMatchingMethods(typ.get(), methods, args);
  return m.empty() ? nullptr : m[0];
}

types::FuncTypePtr
TypecheckVisitor::findBestMethod(const std::string &fn,
                                 const std::vector<CallExpr::Arg> &args) {
  std::vector<types::FuncTypePtr> methods;
  for (auto &m : ctx->cache->overloads[fn])
    if (!endswith(m.name, ":dispatch"))
      methods.push_back(ctx->cache->functions[m.name].type);
  std::reverse(methods.begin(), methods.end());
  auto m = findMatchingMethods(nullptr, methods, args);
  return m.empty() ? nullptr : m[0];
}

std::vector<types::FuncTypePtr>
TypecheckVisitor::findSuperMethods(const types::FuncTypePtr &func) {
  if (func->ast->attributes.parentClass.empty() ||
      endswith(func->ast->name, ":dispatch"))
    return {};
  auto p = ctx->find(func->ast->attributes.parentClass)->type;
  if (!p || !p->getClass())
    return {};

  auto methodName = ctx->cache->reverseIdentifierLookup[func->ast->name];
  auto m = ctx->cache->classes.find(p->getClass()->name);
  std::vector<types::FuncTypePtr> result;
  if (m != ctx->cache->classes.end()) {
    auto t = m->second.methods.find(methodName);
    if (t != m->second.methods.end()) {
      for (auto &m : ctx->cache->overloads[t->second]) {
        if (endswith(m.name, ":dispatch"))
          continue;
        if (m.name == func->ast->name)
          break;
        result.emplace_back(ctx->cache->functions[m.name].type);
      }
    }
  }
  std::reverse(result.begin(), result.end());
  return result;
}

std::vector<types::FuncTypePtr>
TypecheckVisitor::findMatchingMethods(types::ClassType *typ,
                                      const std::vector<types::FuncTypePtr> &methods,
                                      const std::vector<CallExpr::Arg> &args) {
  // Pick the last method that accepts the given arguments.
  std::vector<types::FuncTypePtr> results;
  for (int mi = 0; mi < methods.size(); mi++) {
    auto m = ctx->instantiate(nullptr, methods[mi], typ, false)->getFunc();
    std::vector<types::TypePtr> reordered;
    auto score = ctx->reorderNamedArgs(
        m.get(), args,
        [&](int s, int k, const std::vector<std::vector<int>> &slots, bool _) {
          for (int si = 0; si < slots.size(); si++) {
            if (m->ast->args[si].generic) {
              // Ignore type arguments
            } else if (si == s || si == k || slots[si].size() != 1) {
              // Ignore *args, *kwargs and default arguments
              reordered.emplace_back(nullptr);
            } else {
              reordered.emplace_back(args[slots[si][0]].value->type);
            }
          }
          return 0;
        },
        [](const std::string &) { return -1; });
    for (int ai = 0, mi = 1, gi = 0; score != -1 && ai < reordered.size(); ai++) {
      auto expectTyp =
          m->ast->args[ai].generic ? m->generics[gi++].type : m->args[mi++];
      auto argType = reordered[ai];
      if (!argType)
        continue;
      try {
        ExprPtr dummy = std::make_shared<IdExpr>("");
        dummy->type = argType;
        dummy->done = true;
        wrapExpr(dummy, expectTyp, m, /*undoOnSuccess*/ true);
      } catch (const exc::ParserException &) {
        score = -1;
      }
    }
    if (score != -1) {
      results.push_back(methods[mi]);
    }
  }
  return results;
}

bool TypecheckVisitor::wrapExpr(ExprPtr &expr, TypePtr expectedType,
                                const FuncTypePtr &callee, bool undoOnSuccess) {
  auto expectedClass = expectedType->getClass();
  auto exprClass = expr->getType()->getClass();
  if (callee && expr->isType())
    expr = transform(N<CallExpr>(expr, N<EllipsisExpr>()));

  std::unordered_set<std::string> hints = {"Generator", "float", TYPE_OPTIONAL};
  if (!exprClass && expectedClass && in(hints, expectedClass->name)) {
    return false; // argument type not yet known.
  } else if (expectedClass && expectedClass->name == "Generator" &&
             exprClass->name != expectedClass->name && !expr->getEllipsis()) {
    // Note: do not do this in pipelines (TODO: why?).
    expr = transform(N<CallExpr>(N<DotExpr>(expr, "__iter__")));
  } else if (expectedClass && expectedClass->name == "float" &&
             exprClass->name == "int") {
    expr = transform(N<CallExpr>(N<IdExpr>("float"), expr));
  } else if (expectedClass && expectedClass->name == TYPE_OPTIONAL &&
             exprClass->name != expectedClass->name) {
    expr = transform(N<CallExpr>(N<IdExpr>(TYPE_OPTIONAL), expr));
  } else if (expectedClass && exprClass && exprClass->name == TYPE_OPTIONAL &&
             exprClass->name != expectedClass->name) { // unwrap optional
    expr = transform(N<CallExpr>(N<IdExpr>(FN_UNWRAP), expr));
  } else if (callee && exprClass && expr->type->getFunc() &&
             !(expectedClass && startswith(expectedClass->name, TYPE_FUNCTION))) {
    // Case 7: wrap raw Seq functions into Partial(...) call for easy realization.
    expr = partializeFunction(expr);
  }

  // Special case:
  unify(expr->type, expectedType, undoOnSuccess);
  return true;
}

int64_t TypecheckVisitor::translateIndex(int64_t idx, int64_t len, bool clamp) {
  if (idx < 0)
    idx += len;
  if (clamp) {
    if (idx < 0)
      idx = 0;
    if (idx > len)
      idx = len;
  } else if (idx < 0 || idx >= len) {
    error("tuple index {} out of bounds (len: {})", idx, len);
  }
  return idx;
}

int64_t TypecheckVisitor::sliceAdjustIndices(int64_t length, int64_t *start,
                                             int64_t *stop, int64_t step) {
  if (step == 0)
    error("slice step cannot be 0");

  if (*start < 0) {
    *start += length;
    if (*start < 0) {
      *start = (step < 0) ? -1 : 0;
    }
  } else if (*start >= length) {
    *start = (step < 0) ? length - 1 : length;
  }

  if (*stop < 0) {
    *stop += length;
    if (*stop < 0) {
      *stop = (step < 0) ? -1 : 0;
    }
  } else if (*stop >= length) {
    *stop = (step < 0) ? length - 1 : length;
  }

  if (step < 0) {
    if (*stop < *start) {
      return (*start - *stop - 1) / (-step) + 1;
    }
  } else {
    if (*start < *stop) {
      return (*stop - *start - 1) / step + 1;
    }
  }
  return 0;
}

types::FuncTypePtr TypecheckVisitor::findDispatch(const std::string &fn) {
  for (auto &m : ctx->cache->overloads[fn])
    if (endswith(ctx->cache->functions[m.name].ast->name, ":dispatch"))
      return ctx->cache->functions[m.name].type;

  // Generate dispatch and return it!
  auto name = fn + ":dispatch";

  ExprPtr root;
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
      N<SuiteStmt>(N<IfStmt>(
          N<CallExpr>(N<IdExpr>("isinstance"), root->clone(), N<IdExpr>("void")),
          N<ExprStmt>(root->clone()), N<ReturnStmt>(root))),
      Attr({"autogenerated"}));
  ctx->cache->reverseIdentifierLookup[name] = ctx->cache->reverseIdentifierLookup[fn];

  auto baseType =
      ctx->instantiate(N<IdExpr>(name).get(), ctx->find(generateFunctionStub(2))->type,
                       nullptr, false)
          ->getRecord();
  auto typ = std::make_shared<FuncType>(baseType, ast.get());
  typ = std::static_pointer_cast<FuncType>(typ->generalize(ctx->typecheckLevel));
  ctx->add(TypecheckItem::Func, name, typ);

  ctx->cache->overloads[fn].insert(ctx->cache->overloads[fn].begin(), {name, 0});
  ctx->cache->functions[name].ast = ast;
  ctx->cache->functions[name].type = typ;
  prependStmts->push_back(ast);
  return typ;
}

ExprPtr TypecheckVisitor::transformSuper(const CallExpr *expr) {
  // For now, we just support casting to the _FIRST_ overload (i.e. empty super())
  if (ctx->bases.empty() || !ctx->bases.back().type)
    error("no parent classes available");
  auto fptyp = ctx->bases.back().type->getFunc();
  if (!fptyp || !fptyp->ast->hasAttr(Attr::Method))
    error("no parent classes available");
  if (fptyp->args.size() < 2)
    error("no parent classes available");
  ClassTypePtr typ = fptyp->args[1]->getClass();
  auto &cands = ctx->cache->classes[typ->name].parentClasses;
  if (cands.empty())
    error("no parent classes available");
  //  if (typ->getRecord())
  //    error("cannot use super on tuple types");

  // find parent typ
  // unify top N args with parent typ args
  // realize & do bitcast
  // call bitcast() . method

  auto name = cands[0].first;
  int fields = cands[0].second;
  auto val = ctx->find(name);
  seqassert(val, "cannot find '{}'", name);
  auto ftyp = ctx->instantiate(expr, val->type)->getClass();

  if (typ->getRecord()) {
    std::vector<ExprPtr> members;
    for (int i = 0; i < fields; i++)
      members.push_back(N<DotExpr>(N<IdExpr>(fptyp->ast->args[0].name),
                                   ctx->cache->classes[typ->name].fields[i].name));
    ExprPtr e = transform(
        N<CallExpr>(N<IdExpr>(format(TYPE_TUPLE "{}", members.size())), members));
    unify(e->type, ftyp);
    e->type = ftyp;
    return e;
  } else {
    for (int i = 0; i < fields; i++) {
      auto t = ctx->cache->classes[typ->name].fields[i].type;
      t = ctx->instantiate(expr, t, typ.get());

      auto ft = ctx->cache->classes[name].fields[i].type;
      ft = ctx->instantiate(expr, ft, ftyp.get());
      unify(t, ft);
    }

    ExprPtr typExpr = N<IdExpr>(name);
    typExpr->setType(ftyp);
    auto self = fptyp->ast->args[0].name;
    ExprPtr e = transform(
        N<CallExpr>(N<DotExpr>(N<IdExpr>("__internal__"), "to_class_ptr"),
                    N<CallExpr>(N<DotExpr>(N<IdExpr>(self), "__raw__")), typExpr));
    return e;
  }
}

std::vector<ClassTypePtr> TypecheckVisitor::getSuperTypes(const ClassTypePtr &cls) {
  std::vector<ClassTypePtr> result;
  if (!cls)
    return result;
  result.push_back(cls);
  int start = 0;
  for (auto &cand : ctx->cache->classes[cls->name].parentClasses) {
    auto name = cand.first;
    int fields = cand.second;
    auto val = ctx->find(name);
    seqassert(val, "cannot find '{}'", name);
    auto ftyp = ctx->instantiate(nullptr, val->type)->getClass();
    for (int i = start; i < fields; i++) {
      auto t = ctx->cache->classes[cls->name].fields[i].type;
      t = ctx->instantiate(nullptr, t, cls.get());
      auto ft = ctx->cache->classes[name].fields[i].type;
      ft = ctx->instantiate(nullptr, ft, ftyp.get());
      unify(t, ft);
    }
    start += fields;
    for (auto &t : getSuperTypes(ftyp))
      result.push_back(t);
  }
  return result;
}

//////////////

bool TypecheckVisitor::callTransformCallArgs(std::vector<CallExpr::Arg> &args,
                                             const types::TypePtr &inType) {
  for (int ai = 0; ai < args.size(); ai++) {
    if (auto es = args[ai].value->getStar()) {
      // Case 1: *arg unpacking
      es->what = transform(es->what);
      auto t = es->what->type->getClass();
      if (!t)
        return false;
      if (!t->getRecord())
        error("can only unpack tuple types");
      auto &ff = ctx->cache->classes[t->name].fields;
      for (int i = 0; i < t->getRecord()->args.size(); i++, ai++)
        args.insert(
            args.begin() + ai,
            CallExpr::Arg{"", transform(N<DotExpr>(clone(es->what), ff[i].name))});
      args.erase(args.begin() + ai);
      ai--;
    } else if (auto ek = CAST(args[ai].value, KeywordStarExpr)) {
      // Case 2: **kwarg unpacking
      ek->what = transform(ek->what);
      auto t = ek->what->type->getClass();
      if (!t)
        return false;
      if (!t->getRecord() || startswith(t->name, TYPE_TUPLE))
        error("can only unpack named tuple types: {}", t->toString());
      auto &ff = ctx->cache->classes[t->name].fields;
      for (int i = 0; i < t->getRecord()->args.size(); i++, ai++)
        args.insert(args.begin() + ai,
                    CallExpr::Arg{ff[i].name,
                                  transform(N<DotExpr>(clone(ek->what), ff[i].name))});
      args.erase(args.begin() + ai);
      ai--;
    } else {
      // Case 3: Normal argument
      args[ai].value = transform(args[ai].value, true);
      // Unbound inType might become a generator that will need to be extracted, so
      // don't unify it yet.
      if (inType && !inType->getUnbound() && args[ai].value->getEllipsis() &&
          args[ai].value->getEllipsis()->isPipeArg)
        unify(args[ai].value->type, inType);
    }
  }
  std::set<std::string> seenNames;
  for (auto &i : args)
    if (!i.name.empty()) {
      if (in(seenNames, i.name))
        error("repeated named argument '{}'", i.name);
      seenNames.insert(i.name);
    }
  return true;
}

ExprPtr TypecheckVisitor::callTransformCallee(ExprPtr &callee,
                                              std::vector<CallExpr::Arg> &args,
                                              PartialCallData &part) {
  if (!part.isPartial) {
    // Intercept dot-callees (e.g. expr.foo). Needed in order to select a proper
    // overload for magic methods and to avoid dealing with partial calls
    // (a non-intercepted object DotExpr (e.g. expr.foo) will get transformed into a
    // partial call).
    ExprPtr *lhs = &callee;
    // Make sure to check for instantiation DotExpr (e.g. a.b[T]) as well.
    if (auto ei = callee->getIndex()) {
      // A potential function instantiation
      lhs = &ei->expr;
    } else if (auto eii = CAST(callee, InstantiateExpr)) {
      // Real instantiation
      lhs = &eii->typeExpr;
    }
    if (auto ed = const_cast<DotExpr *>((*lhs)->getDot())) {
      if (auto edt = transformDot(ed, &args))
        *lhs = edt;
    } else if (auto ei = const_cast<IdExpr *>((*lhs)->getId())) {
      // check if this is an overloaded function?
      auto i = ctx->cache->overloads.find(ei->value);
      if (i != ctx->cache->overloads.end() && i->second.size() != 1) {
        if (auto bestMethod = findBestMethod(ei->value, args)) {
          ExprPtr e = N<IdExpr>(bestMethod->ast->name);
          auto t = ctx->instantiate(callee.get(), bestMethod);
          unify(e->type, t);
          unify(ei->type, e->type);
          *lhs = e;
        } else {
          std::vector<std::string> nice;
          for (auto &t : args)
            nice.emplace_back(format("{} = {}", t.name, t.value->type->toString()));
          error("cannot find an overload '{}' with arguments {}", ei->value,
                join(nice, ", "));
        }
      }
    }
  }
  return transform(callee, true);
}

ExprPtr TypecheckVisitor::callReorderArguments(ClassTypePtr callee, CallExpr *expr,
                                               int &ellipsisStage,
                                               PartialCallData &part) {
  auto calleeFn = callee->getFunc();

  std::vector<CallExpr::Arg> args;
  std::vector<ExprPtr> typeArgs;
  int typeArgCount = 0;
  auto newMask = std::vector<char>(calleeFn->ast->args.size(), 1);
  auto getPartialArg = [&](int pi) {
    auto id = transform(N<IdExpr>(part.var));
    ExprPtr it = N<IntExpr>(pi);
    // Manual call to transformStaticTupleIndex needed because otherwise
    // IndexExpr routes this to InstantiateExpr.
    auto ex = transformStaticTupleIndex(callee.get(), id, it);
    seqassert(ex, "partial indexing failed");
    return ex;
  };

  part.args = part.kwArgs = nullptr;
  if (expr->ordered)
    args = expr->args;
  else
    ctx->reorderNamedArgs(
        calleeFn.get(), expr->args,
        [&](int starArgIndex, int kwstarArgIndex,
            const std::vector<std::vector<int>> &slots, bool partial) {
          ctx->addBlock(); // add generics for default arguments.
          addFunctionGenerics(calleeFn->getFunc().get());
          for (int si = 0, pi = 0; si < slots.size(); si++) {
            if (calleeFn->ast->args[si].generic) {
              typeArgs.push_back(slots[si].empty() ? nullptr
                                                   : expr->args[slots[si][0]].value);
              typeArgCount += typeArgs.back() != nullptr;
              newMask[si] = slots[si].empty() ? 0 : 1;
            } else if (si == starArgIndex) {
              std::vector<ExprPtr> extra;
              if (!part.known.empty())
                extra.push_back(N<StarExpr>(getPartialArg(-2)));
              for (auto &e : slots[si]) {
                extra.push_back(expr->args[e].value);
                if (extra.back()->getEllipsis())
                  ellipsisStage = args.size();
              }
              auto e = transform(N<TupleExpr>(extra));
              if (partial) {
                part.args = e;
                args.push_back({"", transform(N<EllipsisExpr>())});
                newMask[si] = 0;
              } else {
                args.push_back({"", e});
              }
            } else if (si == kwstarArgIndex) {
              std::vector<std::string> names;
              std::vector<CallExpr::Arg> values;
              if (!part.known.empty()) {
                auto e = getPartialArg(-1);
                auto t = e->getType()->getRecord();
                seqassert(t && startswith(t->name, "KwTuple"), "{} not a kwtuple",
                          e->toString());
                auto &ff = ctx->cache->classes[t->name].fields;
                for (int i = 0; i < t->getRecord()->args.size(); i++) {
                  names.emplace_back(ff[i].name);
                  values.emplace_back(
                      CallExpr::Arg{"", transform(N<DotExpr>(clone(e), ff[i].name))});
                }
              }
              for (auto &e : slots[si]) {
                names.emplace_back(expr->args[e].name);
                values.emplace_back(CallExpr::Arg{"", expr->args[e].value});
                if (values.back().value->getEllipsis())
                  ellipsisStage = args.size();
              }
              auto kwName = generateTupleStub(names.size(), "KwTuple", names);
              auto e = transform(N<CallExpr>(N<IdExpr>(kwName), values));
              if (partial) {
                part.kwArgs = e;
                args.push_back({"", transform(N<EllipsisExpr>())});
                newMask[si] = 0;
              } else {
                args.push_back({"", e});
              }
            } else if (slots[si].empty()) {
              if (!part.known.empty() && part.known[si]) {
                args.push_back({"", getPartialArg(pi++)});
              } else if (partial) {
                args.push_back({"", transform(N<EllipsisExpr>())});
                newMask[si] = 0;
              } else {
                auto es = calleeFn->ast->args[si].deflt->toString();
                if (in(ctx->defaultCallDepth, es))
                  error("recursive default arguments");
                ctx->defaultCallDepth.insert(es);
                args.push_back({"", transform(clone(calleeFn->ast->args[si].deflt))});
                ctx->defaultCallDepth.erase(es);
              }
            } else {
              seqassert(slots[si].size() == 1, "call transformation failed");
              args.push_back({"", expr->args[slots[si][0]].value});
            }
          }
          ctx->popBlock();
          return 0;
        },
        [&](const std::string &errorMsg) {
          error("{}", errorMsg);
          return -1;
        },
        part.known);
  if (part.args != nullptr)
    part.args->setAttr(ExprAttr::SequenceItem);
  if (part.kwArgs != nullptr)
    part.kwArgs->setAttr(ExprAttr::SequenceItem);
  if (part.isPartial) {
    deactivateUnbounds(expr->args.back().value->getType().get());
    expr->args.pop_back();
    if (!part.args)
      part.args = transform(N<TupleExpr>());
    if (!part.kwArgs) {
      auto kwName = generateTupleStub(0, "KwTuple", {});
      part.kwArgs = transform(N<CallExpr>(N<IdExpr>(kwName)));
    }
  }

  // Typecheck given arguments with the expected (signature) types.
  seqassert((expr->ordered && typeArgs.empty()) ||
                (!expr->ordered && typeArgs.size() == calleeFn->funcGenerics.size()),
            "bad vector sizes");
  for (int si = 0; !expr->ordered && si < calleeFn->funcGenerics.size(); si++)
    if (typeArgs[si]) {
      auto t = typeArgs[si]->type;
      if (calleeFn->funcGenerics[si].type->isStaticType()) {
        if (!typeArgs[si]->isStatic())
          error("expected static expression");
        t = std::make_shared<StaticType>(typeArgs[si], ctx);
      }
      unify(t, calleeFn->funcGenerics[si].type);
    }

  // Special case: function instantiation
  if (part.isPartial && typeArgCount && typeArgCount == expr->args.size()) {
    for (auto &a : args) {
      if (a.value->getEllipsis())
        deactivateUnbounds(a.value->getType().get());
    }
    auto e = transform(expr->expr);
    unify(expr->type, e->getType());
    return e;
  }

  expr->args = args;
  expr->ordered = true;
  part.known = newMask;
  return nullptr;
}

} // namespace ast
} // namespace codon
