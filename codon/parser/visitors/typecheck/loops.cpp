#include <string>
#include <tuple>

#include "codon/parser/ast.h"
#include "codon/parser/common.h"
#include "codon/parser/visitors/simplify/simplify.h"
#include "codon/parser/visitors/typecheck/typecheck.h"

using fmt::format;

namespace codon::ast {

using namespace types;

/// Nothing to typecheck; just call setDone
void TypecheckVisitor::visit(BreakStmt *stmt) { stmt->setDone(); }

/// Nothing to typecheck; just call setDone
void TypecheckVisitor::visit(ContinueStmt *stmt) { stmt->setDone(); }

/// Typecheck while statements.
void TypecheckVisitor::visit(WhileStmt *stmt) {
  transform(stmt->cond);
  ctx->blockLevel++;
  transform(stmt->suite);
  ctx->blockLevel--;

  if (stmt->cond->isDone() && stmt->suite->isDone())
    stmt->setDone();
}

/// Typecheck for statements. Wrap the iterator expression with `__iter__` if needed.
/// See @c transformHeterogenousTupleFor for iterating heterogenous tuples.
void TypecheckVisitor::visit(ForStmt *stmt) {
  transform(stmt->decorator);
  transform(stmt->iter);

  // Extract the iterator type of the for
  auto iterType = stmt->iter->getType()->getClass();
  if (!iterType)
    return; // wait until the iterator is known

  if (isTuple(iterType->name) && !iterType->canRealize()) {
    return; // wait until the tuple is fully realizable
  } else if (iterType->getHeterogenousTuple()) {
    // Case: iterating a heterogenous tuple
    resultStmt = transformHeterogenousTupleFor(stmt);
    return;
  }

  // Case: iterating a non-generator. Wrap with `__iter__`
  if (iterType->name != "Generator" && !stmt->wrapped) {
    stmt->iter = transform(N<CallExpr>(N<DotExpr>(stmt->iter, "__iter__")));
    iterType = stmt->iter->getType()->getClass();
    stmt->wrapped = true;
  }

  auto var = stmt->var->getId();
  seqassert(var, "corrupt for variable: {}", stmt->var->toString());

  // Handle dominated for bindings
  auto changed = in(ctx->cache->replacements, var->value);
  while (auto s = in(ctx->cache->replacements, var->value))
    var->value = s->first, changed = s;
  if (changed && changed->second) {
    auto u =
        N<AssignStmt>(N<IdExpr>(format("{}.__used__", var->value)), N<BoolExpr>(true));
    u->setUpdate();
    stmt->suite = N<SuiteStmt>(u, stmt->suite);
  }
  if (changed)
    var->setAttr(ExprAttr::Dominated);

  // Unify iterator variable and the iterator type
  auto val = ctx->find(var->value);
  if (!changed)
    val = ctx->add(TypecheckItem::Var, var->value,
                   ctx->getUnbound(stmt->var->getSrcInfo()));
  if (iterType && iterType->name != "Generator")
    error("for loop expected a generator");
  unify(stmt->var->type,
        iterType ? unify(val->type, iterType->generics[0].type) : val->type);

  ctx->blockLevel++;
  transform(stmt->suite);
  ctx->blockLevel--;

  if (stmt->iter->isDone() && stmt->suite->isDone())
    stmt->setDone();
}

/// Handle heterogeneous tuple iteration.
/// @example
///   `for i in tuple_expr: <suite>` ->
///   ```tuple = tuple_expr
///      for cnt in range(<tuple length>):
///        if cnt == 0:
///          i = t[0]; <suite>
///        if cnt == 1:
///          i = t[1]; <suite> ...```
/// A separate suite is generated  for each tuple member.
StmtPtr TypecheckVisitor::transformHeterogenousTupleFor(ForStmt *stmt) {
  auto block = N<SuiteStmt>();
  // `tuple = <tuple expression>`
  auto tupleVar = ctx->cache->getTemporaryVar("tuple");
  block->stmts.push_back(N<AssignStmt>(N<IdExpr>(tupleVar), stmt->iter));

  auto tupleArgs = stmt->iter->getType()->getHeterogenousTuple()->args;
  auto cntVar = ctx->cache->getTemporaryVar("idx");
  std::vector<StmtPtr> forBlock;
  for (size_t ai = 0; ai < tupleArgs.size(); ai++) {
    // `if cnt == ai: (var = tuple[ai]; <suite>)`
    forBlock.push_back(N<IfStmt>(
        N<BinaryExpr>(N<IdExpr>(cntVar), "==", N<IntExpr>(ai)),
        N<SuiteStmt>(N<AssignStmt>(clone(stmt->var),
                                   N<IndexExpr>(N<IdExpr>(tupleVar), N<IntExpr>(ai))),
                     clone(stmt->suite))));
  }
  // `for cnt in range(tuple_size): ...`
  block->stmts.push_back(
      N<ForStmt>(N<IdExpr>(cntVar),
                 N<CallExpr>(N<IdExpr>("std.internal.types.range.range"),
                             N<IntExpr>(tupleArgs.size())),
                 N<SuiteStmt>(forBlock)));

  ctx->blockLevel++;
  transform(block);
  ctx->blockLevel--;

  return block;
}

} // namespace codon::ast