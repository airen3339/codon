// Copyright (C) 2022-2023 Exaloop Inc. <https://exaloop.io>

#include "cache.h"

#include <chrono>
#include <string>
#include <vector>

#include "codon/cir/pyextension.h"
#include "codon/cir/util/irtools.h"
#include "codon/parser/common.h"
#include "codon/parser/peg/peg.h"
#include "codon/parser/visitors/simplify/simplify.h"
#include "codon/parser/visitors/translate/translate.h"
#include "codon/parser/visitors/typecheck/ctx.h"
#include "codon/parser/visitors/typecheck/typecheck.h"

namespace codon::ast {

Cache::Cache(std::string argv0)
    : generatedSrcInfoCount(0), unboundCount(256), varCount(0), age(0),
      argv0(std::move(argv0)), typeCtx(nullptr), codegenCtx(nullptr), isJit(false),
      jitCell(0), pythonExt(false), pyModule(nullptr) {}

std::string Cache::getTemporaryVar(const std::string &prefix, char sigil) {
  return fmt::format("{}{}_{}", sigil ? fmt::format("{}_", sigil) : "", prefix,
                     ++varCount);
}

std::string Cache::rev(const std::string &s) {
  auto i = reverseIdentifierLookup.find(s);
  if (i != reverseIdentifierLookup.end())
    return i->second;
  seqassertn(false, "'{}' has no non-canonical name", s);
  return "";
}

void Cache::addGlobal(const std::string &name, ir::Var *var) {
  if (!in(globals, name)) {
    // LOG("[global] {}", name);
    globals[name] = var;
  }
}

SrcInfo Cache::generateSrcInfo() {
  return {FILE_GENERATED, generatedSrcInfoCount, generatedSrcInfoCount++, 0};
}

std::string Cache::getContent(const SrcInfo &info) {
  auto i = imports.find(info.file);
  if (i == imports.end())
    return "";
  int line = info.line - 1;
  if (line < 0 || line >= i->second.content.size())
    return "";
  auto s = i->second.content[line];
  int col = info.col - 1;
  if (col < 0 || col >= s.size())
    return "";
  int len = info.len;
  return s.substr(col, len);
}

types::ClassTypePtr Cache::findClass(const std::string &name) const {
  auto f = typeCtx->find(name);
  if (f && f->kind == TypecheckItem::Type)
    return f->type->getClass();
  return nullptr;
}

types::FuncTypePtr Cache::findFunction(const std::string &name) const {
  auto f = typeCtx->find(name);
  if (f && f->type && f->kind == TypecheckItem::Func)
    return f->type->getFunc();
  f = typeCtx->find(name + ":0");
  if (f && f->type && f->kind == TypecheckItem::Func)
    return f->type->getFunc();
  return nullptr;
}

types::FuncTypePtr Cache::findMethod(types::ClassType *typ, const std::string &member,
                                     const std::vector<types::TypePtr> &args) {
  auto e = std::make_shared<IdExpr>(typ->name);
  e->type = typ->getClass();
  seqassertn(e->type, "not a class");
  int oldAge = typeCtx->age;
  typeCtx->age = 99999;
  auto f = TypecheckVisitor(typeCtx).findBestMethod(e->type->getClass(), member, args);
  typeCtx->age = oldAge;
  return f;
}

ir::types::Type *Cache::realizeType(types::ClassTypePtr type,
                                    const std::vector<types::TypePtr> &generics) {
  auto e = std::make_shared<IdExpr>(type->name);
  e->type = type;
  type = typeCtx->instantiateGeneric(type, generics)->getClass();
  auto tv = TypecheckVisitor(typeCtx);
  if (auto rtv = tv.realize(type)) {
    return classes[rtv->getClass()->name]
        .realizations[rtv->getClass()->realizedTypeName()]
        ->ir;
  }
  return nullptr;
}

ir::Func *Cache::realizeFunction(types::FuncTypePtr type,
                                 const std::vector<types::TypePtr> &args,
                                 const std::vector<types::TypePtr> &generics,
                                 const types::ClassTypePtr &parentClass) {
  auto e = std::make_shared<IdExpr>(type->ast->name);
  e->type = type;
  type = typeCtx->instantiate(type, parentClass)->getFunc();
  if (args.size() != type->getArgTypes().size() + 1)
    return nullptr;
  types::Type::Unification undo;
  if (type->getRetType()->unify(args[0].get(), &undo) < 0) {
    undo.undo();
    return nullptr;
  }
  for (int gi = 1; gi < args.size(); gi++) {
    undo = types::Type::Unification();
    if (type->getArgTypes()[gi - 1]->unify(args[gi].get(), &undo) < 0) {
      undo.undo();
      return nullptr;
    }
  }
  if (!generics.empty()) {
    if (generics.size() != type->funcGenerics.size())
      return nullptr;
    for (int gi = 0; gi < generics.size(); gi++) {
      undo = types::Type::Unification();
      if (type->funcGenerics[gi].type->unify(generics[gi].get(), &undo) < 0) {
        undo.undo();
        return nullptr;
      }
    }
  }
  int oldAge = typeCtx->age;
  typeCtx->age = 99999;
  auto tv = TypecheckVisitor(typeCtx);
  ir::Func *f = nullptr;
  if (auto rtv = tv.realize(type)) {
    auto pr = pendingRealizations; // copy it as it might be modified
    for (auto &fn : pr)
      TranslateVisitor(codegenCtx).transform(functions[fn.first].ast->clone());
    f = functions[rtv->getFunc()->ast->name].realizations[rtv->realizedName()]->ir;
  }
  typeCtx->age = oldAge;
  return f;
}

ir::types::Type *Cache::makeTuple(const std::vector<types::TypePtr> &types) {
  auto tv = TypecheckVisitor(typeCtx);
  auto name = tv.generateTuple(types.size());
  auto t = typeCtx->find(name);
  seqassertn(t && t->type, "cannot find {}", name);
  return realizeType(t->type->getClass(), types);
}

ir::types::Type *Cache::makeFunction(const std::vector<types::TypePtr> &types) {
  auto tv = TypecheckVisitor(typeCtx);
  seqassertn(!types.empty(), "types must have at least one argument");

  auto tup = tv.generateTuple(types.size() - 1);
  const auto &ret = types[0];
  auto argType = typeCtx->instantiateGeneric(
      typeCtx->find(tup)->type,
      std::vector<types::TypePtr>(types.begin() + 1, types.end()));
  auto t = typeCtx->find("Function");
  seqassertn(t && t->type, "cannot find 'Function'");
  return realizeType(t->type->getClass(), {argType, ret});
}

ir::types::Type *Cache::makeUnion(const std::vector<types::TypePtr> &types) {
  auto tv = TypecheckVisitor(typeCtx);

  auto tup = tv.generateTuple(types.size());
  auto argType = typeCtx->instantiateGeneric(typeCtx->find(tup)->type, types);
  auto t = typeCtx->find("Union");
  seqassertn(t && t->type, "cannot find 'Union'");
  return realizeType(t->type->getClass(), {argType});
}

void Cache::parseCode(const std::string &code) {
  auto node = ast::parseCode(this, "<internal>", code, /*startLine=*/0);
  auto sctx = imports[MAIN_IMPORT].ctx;
  node = ast::SimplifyVisitor::apply(sctx, node, "<internal>", 99999);
  node = ast::TypecheckVisitor::apply(this, node);
  ast::TranslateVisitor(codegenCtx).transform(node);
}

std::vector<ExprPtr> Cache::mergeC3(std::vector<std::vector<ExprPtr>> &seqs) {
  // Reference: https://www.python.org/download/releases/2.3/mro/
  std::vector<ExprPtr> result;
  for (size_t i = 0;; i++) {
    bool found = false;
    ExprPtr cand = nullptr;
    for (auto &seq : seqs) {
      if (seq.empty())
        continue;
      found = true;
      bool nothead = false;
      for (auto &s : seqs)
        if (!s.empty()) {
          bool in = false;
          for (size_t j = 1; j < s.size(); j++) {
            if ((in |= (seq[0]->getTypeName() == s[j]->getTypeName())))
              break;
          }
          if (in) {
            nothead = true;
            break;
          }
        }
      if (!nothead) {
        cand = seq[0];
        break;
      }
    }
    if (!found)
      return result;
    if (!cand)
      return {};
    result.push_back(clone(cand));
    for (auto &s : seqs)
      if (!s.empty() && cand->getTypeName() == s[0]->getTypeName()) {
        s.erase(s.begin());
      }
  }
  return result;
}

void Cache::populatePythonModule() {
  LOG("[py] ====== module generation =======");

#define N std::make_shared
  auto getFn = [&](const std::string &canonicalName,
                   const std::string &className = "") -> ir::Func * {
    auto &fna = functions[canonicalName].ast;
    std::vector<Param> params;
    std::vector<ExprPtr> args;
    auto sctx = imports[MAIN_IMPORT].ctx;

    bool isMethod = className.empty() ? false : fna->hasAttr(Attr::Method);
    auto name =
        fmt::format("{}{}", className.empty() ? "" : className + ".", canonicalName);
    LOG("[py] {}: {} => {}", isMethod ? "method" : "classm", name, isMethod);
    params = {Param{sctx->generateCanonicalName("self"), N<IdExpr>("cobj")},
              Param{sctx->generateCanonicalName("args"), N<IdExpr>("cobj")}};
    if (fna->args.size() > 1 + isMethod) {
      params.back().type = N<InstantiateExpr>(N<IdExpr>("Ptr"), params.back().type);
      params.push_back(Param{sctx->generateCanonicalName("nargs"), N<IdExpr>("int")});
    }
    ExprPtr po = N<IdExpr>(params[0].name);
    if (!className.empty())
      po = N<CallExpr>(N<DotExpr>(N<IdExpr>(className), "__from_py__"), po);
    if (isMethod)
      args.push_back(po);
    if (fna->args.size() > 1 + isMethod) {
      for (size_t ai = isMethod; ai < fna->args.size(); ai++) {
        ExprPtr po = N<IndexExpr>(N<IdExpr>(params[1].name), N<IntExpr>(ai - isMethod));
        if (fna->args[ai].type) {
          po = N<CallExpr>(N<DotExpr>(fna->args[ai].type->clone(), "__from_py__"), po);
        } else {
          po = N<CallExpr>(N<IdExpr>("pyobj"), po);
        }
        args.push_back(po);
      }
    } else if (fna->args.size() == 1 + isMethod) {
      ExprPtr po = N<IdExpr>(params[1].name);
      if (fna->args[isMethod].type) {
        po = N<CallExpr>(N<DotExpr>(fna->args[isMethod].type->clone(), "__from_py__"),
                         po);
      } else {
        po = N<CallExpr>(N<IdExpr>("pyobj"), po);
      }
      args.push_back(po);
    }
    auto stubName = sctx->generateCanonicalName(fmt::format("_py.{}", name));
    auto node = N<FunctionStmt>(
        stubName, N<IdExpr>("cobj"), params,
        N<SuiteStmt>(N<ReturnStmt>(N<CallExpr>(
            N<DotExpr>(N<CallExpr>(N<IdExpr>(canonicalName), args), "__to_py__")))),
        Attr({Attr::ForceRealize}));
    functions[node->name].ast = node;
    auto tv = TypecheckVisitor(typeCtx);
    auto tnode = tv.transform(node);
    seqassertn(tnode, "blah");
    seqassertn(typeCtx->forceFind(stubName) && typeCtx->forceFind(stubName)->type,
               "bad type");
    auto rtv = tv.realize(typeCtx->forceFind(stubName)->type);
    seqassertn(rtv, "realization of {} failed", stubName);

    auto pr = pendingRealizations; // copy it as it might be modified
    for (auto &fn : pr)
      TranslateVisitor(codegenCtx).transform(functions[fn.first].ast->clone());
    auto f = functions[rtv->getFunc()->ast->name].realizations[rtv->realizedName()]->ir;
    return f;
  };

  // if (!pythonExt)
  //   return;
  if (!pyModule)
    pyModule = std::make_shared<ir::PyModule>();
  using namespace ast;

  int oldAge = typeCtx->age;
  typeCtx->age = 99999;

  // def wrapper(self: cobj, arg: cobj) -> cobj
  // def wrapper(self: cobj, args: Ptr[cobj], nargs: int) -> cobj
  for (const auto &[cn, c] : classes)
    if (c.module.empty() && startswith(cn, "Pyx")) {
      ir::PyType py{rev(cn), c.ast->getDocstr()};

      auto tc = typeCtx->forceFind(cn)->type;
      if (!tc->canRealize())
        compilationError(fmt::format("cannot realize '{}' for Python export", rev(cn)));
      tc = TypecheckVisitor(typeCtx).realize(tc);
      seqassertn(tc, "cannot realize '{}'", cn);

      // fix to_py / from_py
      if (auto ofnn = in(c.methods, "__to_py__")) {
        auto fnn = overloads[*ofnn].begin()->name; // default first overload!
        auto &fna = functions[fnn].ast;
        fna->getFunction()->suite = N<ReturnStmt>(N<CallExpr>(
            N<IdExpr>("__internal__.to_py:0"), N<IdExpr>(fna->args[0].name)));
      } else {
        compilationError(fmt::format("class '{}' has no __to_py__"), rev(cn));
      }
      if (auto ofnn = in(c.methods, "__from_py__")) {
        auto fnn = overloads[*ofnn].begin()->name; // default first overload!
        auto &fna = functions[fnn].ast;
        fna->getFunction()->suite =
            N<ReturnStmt>(N<CallExpr>(N<IdExpr>("__internal__.from_py:0"),
                                      N<IdExpr>(fna->args[0].name), N<IdExpr>(cn)));
      } else {
        compilationError(fmt::format("class '{}' has no __from_py__"), rev(cn));
      }
      for (auto &n : std::vector<std::string>{"__from_py__", "__to_py__"}) {
        auto fnn = overloads[*in(c.methods, n)].begin()->name;
        ir::Func *oldIR = nullptr;
        if (!functions[fnn].realizations.empty())
          oldIR = functions[fnn].realizations.begin()->second->ir;
        functions[fnn].realizations.clear();
        auto tf = TypecheckVisitor(typeCtx).realize(functions[fnn].type);
        seqassertn(tf, "cannot re-realize '{}'", fnn);
        if (oldIR) {
          std::vector<ir::Value *> args;
          for (auto it = oldIR->arg_begin(); it != oldIR->arg_end(); ++it) {
            args.push_back(module->Nr<ir::VarValue>(*it));
          }
          ir::cast<ir::BodiedFunc>(oldIR)->setBody(ir::util::series(
              ir::util::call(functions[fnn].realizations.begin()->second->ir, args)));
        }
      }

      for (auto &[rn, r] : functions["__internal__.py_type:0"].realizations) {
        if (r->type->funcGenerics[0].type->unify(tc.get(), nullptr) >= 0) {
          py.typePtrHook = r->ir;
          break;
        }
      }

      for (const auto &[n, ofnn] : c.methods) {
        auto fnn = overloads[ofnn].back().name; // last overload
        auto &fna = functions[fnn].ast;
        if (fna->hasAttr("autogenerated"))
          continue;
        auto f = getFn(fnn, cn);
        if (!f)
          continue;
        if (n == "__repr__") {
          py.repr = f;
        } else if (n == "__add__") {
          py.add = f;
        } else if (n == "__iadd__") {
          py.iadd = f;
        } else if (n == "__sub__") {
          py.sub = f;
        } else if (n == "__isub__") {
          py.isub = f;
        } else if (n == "__mul__") {
          py.mul = f;
        } else if (n == "__imul__") {
          py.imul = f;
        } else if (n == "__mod__") {
          py.mod = f;
        } else if (n == "__imod__") {
          py.imod = f;
        } else if (n == "__divmod__") {
          py.divmod = f;
        } else if (n == "__pow__") {
          py.pow = f;
        } else if (n == "__ipow__") {
          py.ipow = f;
        } else if (n == "__neg__") {
          py.neg = f;
        } else if (n == "__pos__") {
          py.pos = f;
        } else if (n == "__abs__") {
          py.abs = f;
        } else if (n == "__bool__") {
          py.bool_ = f;
        } else if (n == "__invert__") {
          py.invert = f;
        } else if (n == "__lshift__") {
          py.lshift = f;
        } else if (n == "__ilshift__") {
          py.ilshift = f;
        } else if (n == "__rshift__") {
          py.rshift = f;
        } else if (n == "__irshift__") {
          py.irshift = f;
        } else if (n == "__and__") {
          py.and_ = f;
        } else if (n == "__iand__") {
          py.iand = f;
        } else if (n == "__xor__") {
          py.xor_ = f;
        } else if (n == "__ixor__") {
          py.ixor = f;
        } else if (n == "__or__") {
          py.or_ = f;
        } else if (n == "__ior__") {
          py.ior = f;
        } else if (n == "__int__") {
          py.int_ = f;
        } else if (n == "__float__") {
          py.float_ = f;
        } else if (n == "__floordiv__") {
          py.floordiv = f;
        } else if (n == "__ifloordiv__") {
          py.ifloordiv = f;
        } else if (n == "__truediv__") {
          py.truediv = f;
        } else if (n == "__itruediv__") {
          py.itruediv = f;
        } else if (n == "__index__") {
          py.index = f;
        } else if (n == "__matmul__") {
          py.matmul = f;
        } else if (n == "__imatmul__") {
          py.imatmul = f;
        } else if (n == "__len__") {
          py.len = f;
        } else if (n == "__getitem__") {
          py.getitem = f;
        } else if (n == "__setitem__") {
          py.setitem = f;
        } else if (n == "__contains__") {
          py.contains = f;
        } else if (n == "__hash__") {
          py.hash = f;
        } else if (n == "__call__") {
          py.call = f;
        } else if (n == "__str__") {
          py.str = f;
        } else if (n == "__cmp__") {
          py.cmp = f;
        } else if (n == "__iter__") {
          py.iter = f;
        } else if (n == "__del__") {
          py.del = f;
        } else if (n == "__new__") {
          py.new_ = f;
        } else if (n == "__init__") {
          py.init = f;
        } else {
          py.methods.push_back(
              ir::PyFunction{n, fna->getDocstr(), f,
                             fna->hasAttr(Attr::Method) ? ir::PyFunction::Type::METHOD
                                                        : ir::PyFunction::Type::CLASS,
                             int(fna->args.size()) - fna->hasAttr(Attr::Method)});
        }
        // LOG(">| [{}] {}", functions[stubName].realizations.size(), *f);
      }

      if (c.realizations.size() != 1)
        compilationError(fmt::format("cannot pythonize generic class '{}'", cn));
      auto &r = c.realizations.begin()->second;
      py.type = realizeType(r->type);
      for (auto &[mn, mt] : r->fields) {
        py.members.push_back(ir::PyMember{mn, "",
                                          mt->is("int") ? ir::PyMember::Type::LONGLONG
                                          : mt->is("float")
                                              ? ir::PyMember::Type::DOUBLE
                                              : ir::PyMember::Type::OBJECT,
                                          true});
        LOG("[py] {}: {}.{} => {}", "member", cn, mn, py.members.back().type);
      }
      pyModule->types.push_back(py);
    }
#undef N

  for (const auto &[fn, f] : functions)
    if (f.isToplevel) {
      auto fnn = overloads[f.rootName].back().name; // last overload
      LOG("[py] functn {} => {}", rev(fn), fnn);
      auto ir = getFn(fnn);
      pyModule->functions.push_back(ir::PyFunction{rev(fn), f.ast->getDocstr(), ir,
                                                   ir::PyFunction::Type::TOPLEVEL,
                                                   int(f.ast->args.size())});
    }

  typeCtx->age = oldAge;
}

} // namespace codon::ast
