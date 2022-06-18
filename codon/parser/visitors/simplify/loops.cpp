#include <string>
#include <tuple>
#include <vector>

#include "codon/parser/ast.h"
#include "codon/parser/common.h"
#include "codon/parser/peg/peg.h"
#include "codon/parser/visitors/simplify/simplify.h"

using fmt::format;

namespace codon::ast {

/// Ensure that `continue` is in a loop
void SimplifyVisitor::visit(ContinueStmt *stmt) {
  if (!ctx->getBase()->getLoop())
    error("continue outside of a loop");
}

/// Ensure that `break` is in a loop.
/// Transform if a loop break variable is available
/// (e.g., a break within loop-else block).
/// @example
///   `break` -> `no_break = False; break`
void SimplifyVisitor::visit(BreakStmt *stmt) {
  if (!ctx->getBase()->getLoop())
    error("break outside of a loop");
  if (!ctx->getBase()->getLoop()->breakVar.empty()) {
    resultStmt = N<SuiteStmt>(
        transform(N<AssignStmt>(N<IdExpr>(ctx->getBase()->getLoop()->breakVar),
                                N<BoolExpr>(false))),
        N<BreakStmt>());
  }
}

/// Transform a while loop.
/// @example
///   `while cond: ...`           ->  `while cond.__bool__(): ...`
///   `while cond: ... else: ...` -> ```no_break = True
///                                     while cond.__bool__():
///                                       ...
///                                     if no_break: ...```
void SimplifyVisitor::visit(WhileStmt *stmt) {
  // Check for while-else clause
  std::string breakVar;
  StmtPtr assign = nullptr;
  if (stmt->elseSuite && stmt->elseSuite->firstInBlock()) {
    // no_break = True
    breakVar = ctx->cache->getTemporaryVar("no_break");
    prependStmts->push_back(
        transform(N<AssignStmt>(N<IdExpr>(breakVar), N<BoolExpr>(true))));
  }

  ctx->addScope();
  ctx->getBase()->loops.push_back({breakVar, ctx->scope, {}});
  stmt->cond = transform(N<CallExpr>(N<DotExpr>(stmt->cond, "__bool__")));
  transformConditionalScope(stmt->suite);
  ctx->popScope();
  // Dominate loop variables
  for (auto &var : ctx->getBase()->getLoop()->seenVars)
    ctx->findDominatingBinding(var);
  ctx->getBase()->loops.pop_back();

  // Complete while-else clause
  if (stmt->elseSuite && stmt->elseSuite->firstInBlock()) {
    resultStmt = N<SuiteStmt>(N<WhileStmt>(*stmt),
                              N<IfStmt>(transform(N<IdExpr>(breakVar)),
                                        transformConditionalScope(stmt->elseSuite)));
  }
}

/// Transform for loop.
/// @example
///   `for i, j in it: ...`        -> ```for tmp in it:
///                                        i, j = tmp
///                                        ...```
///   `for i in it: ... else: ...` -> ```no_break = True
///                                      for i in it: ...
///                                      if no_break: ...```
void SimplifyVisitor::visit(ForStmt *stmt) {
  stmt->decorator = transformForDecorator(stmt->decorator);

  std::string breakVar;
  // Needs in-advance transformation to prevent name clashes with the iterator variable
  stmt->iter = transform(stmt->iter);

  // Check for for-else clause
  StmtPtr assign = nullptr;
  if (stmt->elseSuite && stmt->elseSuite->firstInBlock()) {
    breakVar = ctx->cache->getTemporaryVar("no_break");
    assign = transform(N<AssignStmt>(N<IdExpr>(breakVar), N<BoolExpr>(true)));
  }

  ctx->addScope();
  ctx->getBase()->loops.push_back({breakVar, ctx->scope, {}});
  std::string varName;
  if (auto i = stmt->var->getId()) {
    ctx->addVar(i->value, varName = ctx->generateCanonicalName(i->value),
                stmt->var->getSrcInfo());
    transform(stmt->var);
    transform(stmt->suite);
  } else {
    varName = ctx->cache->getTemporaryVar("for");
    ctx->addVar(varName, varName, stmt->var->getSrcInfo());
    auto var = N<IdExpr>(varName);
    std::vector<StmtPtr> stmts;
    // Add for_var = [for variables]
    stmts.push_back(N<AssignStmt>(stmt->var, clone(var)));
    stmt->var = var;
    stmts.push_back(stmt->suite);
    stmt->suite = transform(N<SuiteStmt>(stmts));
  }
  ctx->popScope();
  // Dominate loop variables
  for (auto &var : ctx->getBase()->getLoop()->seenVars)
    ctx->findDominatingBinding(var);
  ctx->getBase()->loops.pop_back();

  // Complete while-else clause
  if (stmt->elseSuite && stmt->elseSuite->firstInBlock()) {
    resultStmt = N<SuiteStmt>(assign, N<ForStmt>(*stmt),
                              N<IfStmt>(transform(N<IdExpr>(breakVar)),
                                        transformConditionalScope(stmt->elseSuite)));
  }
}

/// Transform and check for OpenMP decorator.
/// @example
///   `@par(num_threads=2, openmp="schedule(static)")` ->
///   `for_par(num_threads=2, schedule="static")`
ExprPtr SimplifyVisitor::transformForDecorator(ExprPtr decorator) {
  if (!decorator)
    return nullptr;
  ExprPtr callee = decorator;
  if (auto c = callee->getCall())
    callee = c->expr;
  if (!callee || !callee->isId("par"))
    error("for loop can only take parallel decorator");
  std::vector<CallExpr::Arg> args;
  std::string openmp;
  std::vector<CallExpr::Arg> omp;
  if (auto c = decorator->getCall())
    for (auto &a : c->args) {
      if (a.name == "openmp" ||
          (a.name.empty() && openmp.empty() && a.value->getString())) {
        omp = parseOpenMP(ctx->cache, a.value->getString()->getValue(),
                          a.value->getSrcInfo());
      } else {
        args.push_back({a.name, transform(a.value)});
      }
    }
  for (auto &a : omp)
    args.push_back({a.name, transform(a.value)});
  return N<CallExpr>(transform(N<IdExpr>("for_par")), args);
}

} // namespace codon::ast