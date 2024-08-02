// Copyright (C) 2022-2024 Exaloop Inc. <https://exaloop.io>

#include <string>
#include <vector>

#include "codon/parser/visitors/format/format.h"

using fmt::format;

namespace codon {
namespace ast {

std::string FormatVisitor::anchor_root(const std::string &s) const {
  return fmt::format("<a class=\".root\" name=\"{}\">{}</a>", s, s);
}

std::string FormatVisitor::anchor(const std::string &s) const {
  return fmt::format("<a class=\".anchor\" href=\"#{}\">{}</a>", s, s);
}

FormatVisitor::FormatVisitor(bool html, Cache *cache)
    : renderType(false), renderHTML(html), indent(0), cache(cache) {
  if (renderHTML) {
    header = "<html><head><link rel=stylesheet href=\"../code.css\"/></head>\n<body>";
    header += "<div class=code>\n";
    footer = "\n</div></body></html>";
    nl = "<br/>";
    typeStart = "<ast-type>";
    typeEnd = "</ast-type>";
    nodeStart = "";
    nodeEnd = "";
    stmtStart = "<ast-stmt>";
    stmtEnd = "</ast-stmt>";
    exprStart = "<ast-expr>";
    exprEnd = "</ast-expr>";
    commentStart = "<ast-comment>";
    commentEnd = "</ast-comment>";
    literalStart = "<ast-expr class=lit>";
    literalEnd = "</ast-expr>";
    keywordStart = "<ast-keyword>";
    keywordEnd = "</ast-keyword>";
    space = "&nbsp;";
    renderType = true;
  } else {
    space = " ";
  }
}

std::string FormatVisitor::transform(Expr *expr) {
  FormatVisitor v(renderHTML, cache);
  if (expr)
    expr->accept(v);
  return v.result;
}

std::string FormatVisitor::transform(Stmt *stmt) { return transform(stmt, 0); }

std::string FormatVisitor::transform(Stmt *stmt, int indent) {
  FormatVisitor v(renderHTML, cache);
  v.indent = this->indent + indent;
  if (stmt)
    stmt->accept(v);
  if (v.result.empty())
    return "";
  return fmt::format("{}{}{}{}{}", stmtStart,
                     stmt && stmt->getSuite() ? "" : pad(indent), v.result, stmtEnd,
                     newline());
}

std::string FormatVisitor::pad(int indent) const {
  std::string s;
  for (int i = 0; i < (this->indent + indent) * 2; i++)
    s += space;
  return s;
}

std::string FormatVisitor::newline() const { return nl + "\n"; }

std::string FormatVisitor::keyword(const std::string &s) const {
  return fmt::format("{}{}{}", keywordStart, s, keywordEnd);
}

std::string FormatVisitor::literal(const std::string &s) const {
  return fmt::format("{}{}{}", literalStart, s, literalEnd);
}

/*************************************************************************************/

void FormatVisitor::visit(NoneExpr *expr) { result = renderExpr(expr, "None"); }

void FormatVisitor::visit(BoolExpr *expr) {
  result = renderExpr(expr, "{}", literal(expr->getValue() ? "True" : "False"));
}

void FormatVisitor::visit(IntExpr *expr) {
  auto [value, suffix] = expr->getRawData();
  result = renderExpr(expr, "{}{}", literal(value), suffix);
}

void FormatVisitor::visit(FloatExpr *expr) {
  auto [value, suffix] = expr->getRawData();
  result = renderExpr(expr, "{}{}", literal(value), suffix);
}

void FormatVisitor::visit(StringExpr *expr) {
  result =
      renderExpr(expr, "{}", literal(fmt::format("\"{}\"", escape(expr->getValue()))));
}

void FormatVisitor::visit(IdExpr *expr) {
  result = renderExpr(expr, "{}",
                      expr->type && expr->type->getFunc() ? anchor(expr->getValue())
                                                          : expr->getValue());
}

void FormatVisitor::visit(StarExpr *expr) {
  result = renderExpr(expr, "*{}", transform(expr->getExpr()));
}

void FormatVisitor::visit(KeywordStarExpr *expr) {
  result = renderExpr(expr, "**{}", transform(expr->getExpr()));
}

void FormatVisitor::visit(TupleExpr *expr) {
  result = renderExpr(expr, "({})", transformItems(*expr));
}

void FormatVisitor::visit(ListExpr *expr) {
  result = renderExpr(expr, "[{}]", transformItems(*expr));
}

void FormatVisitor::visit(InstantiateExpr *expr) {
  result =
      renderExpr(expr, "{}⟦{}⟧", transform(expr->getExpr()), transformItems(*expr));
}

void FormatVisitor::visit(SetExpr *expr) {
  result = renderExpr(expr, "{{{}}}", transformItems(*expr));
}

void FormatVisitor::visit(DictExpr *expr) {
  std::vector<std::string> s;
  for (auto *i : *expr) {
    auto t = cast<TupleExpr>(i);
    s.push_back(fmt::format("{}: {}", transform((*t)[0]), transform((*t)[1])));
  }
  result = renderExpr(expr, "{{{}}}", join(s, ", "));
}

void FormatVisitor::visit(GeneratorExpr *expr) {
  // seqassert(false, "not implemented");
  result = "GENERATOR_IMPL";
}

void FormatVisitor::visit(IfExpr *expr) {
  result = renderExpr(expr, "({} {} {} {} {})", transform(expr->getIf()), keyword("if"),
                      transform(expr->getCond()), keyword("else"),
                      transform(expr->getElse()));
}

void FormatVisitor::visit(UnaryExpr *expr) {
  result = renderExpr(expr, "{}{}", expr->getOp(), transform(expr->getExpr()));
}

void FormatVisitor::visit(BinaryExpr *expr) {
  result = renderExpr(expr, "({} {} {})", transform(expr->getLhs()), expr->getOp(),
                      transform(expr->getRhs()));
}

void FormatVisitor::visit(PipeExpr *expr) {
  std::vector<std::string> items;
  for (const auto &l : *expr) {
    if (!items.size())
      items.push_back(transform(l.expr));
    else
      items.push_back(l.op + " " + transform(l.expr));
  }
  result = renderExpr(expr, "({})", join(items, " "));
}

void FormatVisitor::visit(IndexExpr *expr) {
  result = renderExpr(expr, "{}[{}]", transform(expr->getExpr()),
                      transform(expr->getIndex()));
}

void FormatVisitor::visit(CallExpr *expr) {
  std::vector<std::string> args;
  for (auto &i : *expr) {
    if (i.name == "")
      args.push_back(transform(i.value));
    else
      args.push_back(fmt::format("{}={}", i.name, transform(i.value)));
  }
  result = renderExpr(expr, "{}({})", transform(expr->getExpr()), join(args, ", "));
}

void FormatVisitor::visit(DotExpr *expr) {
  result = renderExpr(expr, "{}○{}", transform(expr->getExpr()), expr->getMember());
}

void FormatVisitor::visit(SliceExpr *expr) {
  std::string s;
  if (expr->getStart())
    s += transform(expr->getStart());
  s += ":";
  if (expr->getStop())
    s += transform(expr->getStop());
  s += ":";
  if (expr->getStep())
    s += transform(expr->getStep());
  result = renderExpr(expr, "{}", s);
}

void FormatVisitor::visit(EllipsisExpr *expr) { result = renderExpr(expr, "..."); }

void FormatVisitor::visit(LambdaExpr *expr) {
  std::vector<std::string> s;
  for (const auto &v : *expr)
    s.emplace_back(v);
  result = renderExpr(expr, "{} {}: {}", keyword("lambda"), join(s, ", "),
                      transform(expr->getExpr()));
}

void FormatVisitor::visit(YieldExpr *expr) {
  result = renderExpr(expr, "(" + keyword("yield") + ")");
}

void FormatVisitor::visit(StmtExpr *expr) {
  std::string s;
  for (auto *i : *expr)
    s += format("{}{}", pad(2), transform(i, 2));
  result = renderExpr(expr, "《{}{}{}{}{}》", newline(), s, newline(), pad(2),
                      transform(expr->getExpr()));
}

void FormatVisitor::visit(AssignExpr *expr) {
  result = renderExpr(expr, "({} := {})", transform(expr->getVar()),
                      transform(expr->getExpr()));
}

void FormatVisitor::visit(SuiteStmt *stmt) {
  for (int i = 0; i < stmt->stmts.size(); i++)
    result += transform(stmt->stmts[i]);
}

void FormatVisitor::visit(BreakStmt *stmt) { result = keyword("break"); }

void FormatVisitor::visit(ContinueStmt *stmt) { result = keyword("continue"); }

void FormatVisitor::visit(ExprStmt *stmt) { result = transform(stmt->expr); }

void FormatVisitor::visit(AssignStmt *stmt) {
  if (stmt->type) {
    result = fmt::format("{}: {} = {}", transform(stmt->lhs), transform(stmt->type),
                         transform(stmt->rhs));
  } else {
    result = fmt::format("{} = {}", transform(stmt->lhs), transform(stmt->rhs));
  }
}

void FormatVisitor::visit(AssignMemberStmt *stmt) {
  result = fmt::format("{}○{} = {}", transform(stmt->lhs), stmt->member,
                       transform(stmt->rhs));
}

void FormatVisitor::visit(DelStmt *stmt) {
  result = fmt::format("{} {}", keyword("del"), transform(stmt->expr));
}

void FormatVisitor::visit(PrintStmt *stmt) {
  result = fmt::format("{} {}", keyword("print"), transformItems(stmt->items));
}

void FormatVisitor::visit(ReturnStmt *stmt) {
  result = fmt::format("{}{}", keyword("return"),
                       stmt->expr ? " " + transform(stmt->expr) : "");
}

void FormatVisitor::visit(YieldStmt *stmt) {
  result = fmt::format("{}{}", keyword("yield"),
                       stmt->expr ? " " + transform(stmt->expr) : "");
}

void FormatVisitor::visit(AssertStmt *stmt) {
  result = fmt::format("{} {}", keyword("assert"), transform(stmt->expr));
}

void FormatVisitor::visit(WhileStmt *stmt) {
  result = fmt::format("{} {}:{}{}", keyword("while"), transform(stmt->cond), newline(),
                       transform(stmt->suite, 1));
}

void FormatVisitor::visit(ForStmt *stmt) {
  result = fmt::format("{} {} {} {}:{}{}", keyword("for"), transform(stmt->var),
                       keyword("in"), transform(stmt->iter), newline(),
                       transform(stmt->suite, 1));
}

void FormatVisitor::visit(IfStmt *stmt) {
  result = fmt::format("{} {}:{}{}{}", keyword("if"), transform(stmt->cond), newline(),
                       transform(stmt->ifSuite, 1),
                       stmt->elseSuite ? format("{}:{}{}", keyword("else"), newline(),
                                                transform(stmt->elseSuite, 1))
                                       : "");
}

void FormatVisitor::visit(MatchStmt *stmt) {
  std::string s;
  for (auto &c : stmt->cases)
    s += fmt::format("{}{}{}{}:{}{}", pad(1), keyword("case"), transform(c.pattern),
                     c.guard ? " " + (keyword("case") + " " + transform(c.guard)) : "",
                     newline(), transform(c.suite, 2));
  result =
      fmt::format("{} {}:{}{}", keyword("match"), transform(stmt->what), newline(), s);
}

void FormatVisitor::visit(ImportStmt *stmt) {
  auto as = stmt->as.empty() ? "" : fmt::format(" {} {} ", keyword("as"), stmt->as);
  if (!stmt->what)
    result += fmt::format("{} {}{}", keyword("import"), transform(stmt->from), as);
  else
    result += fmt::format("{} {} {} {}{}", keyword("from"), transform(stmt->from),
                          keyword("import"), transform(stmt->what), as);
}

void FormatVisitor::visit(TryStmt *stmt) {
  std::vector<std::string> catches;
  for (auto &c : stmt->catches) {
    catches.push_back(
        fmt::format("{} {}{}:{}{}", keyword("catch"), transform(c->exc),
                    c->var == "" ? "" : fmt::format("{} {}", keyword("as"), c->var),
                    newline(), transform(c->suite, 1)));
  }
  result =
      fmt::format("{}:{}{}{}{}", keyword("try"), newline(), transform(stmt->suite, 1),
                  fmt::join(catches, ""),
                  stmt->finally ? fmt::format("{}:{}{}", keyword("finally"), newline(),
                                              transform(stmt->finally, 1))
                                : "");
}

void FormatVisitor::visit(GlobalStmt *stmt) {
  result = fmt::format("{} {}", keyword("global"), stmt->var);
}

void FormatVisitor::visit(ThrowStmt *stmt) {
  result = fmt::format("{} {}", keyword("raise"), transform(stmt->expr));
}

void FormatVisitor::visit(FunctionStmt *fstmt) {
  if (cache) {
    if (in(cache->functions, fstmt->name)) {
      if (!cache->functions[fstmt->name].realizations.empty()) {
        result += fmt::format("<details><summary># {}</summary>",
                              fmt::format("{} {}", keyword("def"), fstmt->name));
        for (auto &real : cache->functions[fstmt->name].realizations) {
          auto fa = real.second->ast;
          auto ft = real.second->type;
          std::vector<std::string> attrs;
          for (auto &a : fa->decorators)
            attrs.push_back(fmt::format("@{}", transform(a)));
          if (auto a = fa->getAttribute<ir::StringValueAttribute>(Attr::Module))
            if (!a->value.empty())
              attrs.push_back(fmt::format("@module:{}", a->value));
          if (auto a = fa->getAttribute<ir::StringValueAttribute>(Attr::ParentClass))
            if (!a->value.empty())
              attrs.push_back(fmt::format("@parent:{}", a->value));
          std::vector<std::string> args;
          for (size_t i = 0, j = 0; i < fa->args.size(); i++)
            if (fa->args[i].status == Param::Normal) {
              args.push_back(fmt::format(
                  "{}: {}{}", fa->args[i].name,
                  anchor(ft->getArgTypes()[j++]->realizedName()),
                  fa->args[i].defaultValue
                      ? fmt::format("={}", transform(fa->args[i].defaultValue))
                      : ""));
            }
          auto body = transform(fa->suite, 1);
          auto name = fmt::format("{}", anchor_root(fa->name));
          result += fmt::format(
              "{}{}{}{} {}({}){}:{}{}", newline(), pad(),
              attrs.size() ? join(attrs, newline() + pad()) + newline() + pad() : "",
              keyword("def"), anchor_root(name), fmt::join(args, ", "),
              fmt::format(" -> {}", anchor(ft->getRetType()->realizedName())),
              newline(), body.empty() ? fmt::format("{}", keyword("pass")) : body);
        }
        result += "</details>";
      }
      return;
    }
  }
}

void FormatVisitor::visit(ClassStmt *stmt) {
  if (cache) {
    if (auto cls = in(cache->classes, stmt->name)) {
      if (!cls->realizations.empty()) {
        result = fmt::format(
            "<details><summary># {}</summary>",
            fmt::format("{} {} {}", keyword("class"), stmt->name,
                        stmt->hasAttribute(Attr::Extend) ? " +@extend" : ""));
        for (auto &real : cls->realizations) {
          std::vector<std::string> args;
          auto l = real.second->type->is(TYPE_TUPLE)
                       ? real.second->type->generics.size()
                       : real.second->fields.size();
          for (size_t i = 0; i < l; i++) {
            const auto &[n, t] = real.second->fields[i];
            auto name = fmt::format("{}{}: {}{}", exprStart, n,
                                    anchor(t->realizedName()), exprEnd);
            args.push_back(name);
          }
          result += fmt::format("{}{}{}{} {}", newline(), pad(),
                                (stmt->hasAttribute(Attr::Tuple)
                                     ? format("@tuple{}{}", newline(), pad())
                                     : ""),
                                keyword("class"), anchor_root(real.first));
          if (!args.empty())
            result += fmt::format(":{}{}{}", newline(), pad(indent + 1),
                                  fmt::join(args, newline() + pad(indent + 1)));
        }
        result += "</details>";
      }
    }
  }
  // if (stmt->suite)
  //   result += transform(stmt->suite);
}

void FormatVisitor::visit(YieldFromStmt *stmt) {
  result = fmt::format("{} {}", keyword("yield from"), transform(stmt->expr));
}

void FormatVisitor::visit(WithStmt *stmt) {}

void FormatVisitor::visit(CommentStmt *stmt) {
  result = fmt::format("{}# {}{}", commentStart, stmt->comment, commentEnd);
}

} // namespace ast
} // namespace codon
