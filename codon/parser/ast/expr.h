// Copyright (C) 2022-2024 Exaloop Inc. <https://exaloop.io>

#pragma once

#include <memory>
#include <ostream>
#include <string>
#include <variant>
#include <vector>

#include "codon/parser/ast/types.h"
#include "codon/parser/common.h"

namespace codon::ast {

const int INDENT_SIZE = 2;

struct ASTVisitor;
struct Node : public codon::SrcObject {
  Cache *cache;

  /// Convert a node to an S-expression.
  virtual std::string toString(int) const = 0;
  virtual std::string toString() const { return toString(-1); }

  /// Deep copy a node.
  virtual Node *clone(bool clean) const = 0;
  Node *clone() const { return clone(false); }

  /// Accept an AST visitor.
  virtual void accept(ASTVisitor &visitor) = 0;

  /// Allow pretty-printing to C++ streams.
  friend std::ostream &operator<<(std::ostream &out, const Node &expr) {
    return out << expr.toString();
  }
};
} // namespace codon::ast

template <typename T>
struct fmt::formatter<
    T, std::enable_if_t<std::is_base_of<codon::ast::Node, T>::value, char>>
    : fmt::ostream_formatter {};

// template <typename T>
// struct fmt::formatter<
//     T, std::enable_if_t<std::is_convertible<T, codon::ast::Node *>::value, char>>
//     : fmt::formatter<std::string_view> {
//   template <typename FormatContext>
//   auto format(const T &p, FormatContext &ctx) const -> decltype(ctx.out()) {
//     return fmt::format_to(ctx.out(), "{}", p ? p->toString(0) : "<nullptr>");
//   }
// };

namespace codon::ast {

#define ACCEPT(X)                                                                      \
  Node *clone(bool) const override;                                                    \
  void accept(X &visitor) override

// Forward declarations
struct BinaryExpr;
struct CallExpr;
struct DotExpr;
struct EllipsisExpr;
struct IdExpr;
struct IfExpr;
struct IndexExpr;
struct IntExpr;
struct InstantiateExpr;
struct ListExpr;
struct NoneExpr;
struct StarExpr;
struct KeywordStarExpr;
struct StmtExpr;
struct StringExpr;
struct TupleExpr;
struct UnaryExpr;
struct Stmt;
struct SuiteStmt;

/**
 * A Seq AST expression.
 * Each AST expression is intended to be instantiated as a shared_ptr.
 */
struct Expr : public Node {
  using base_type = Expr;

  // private:
  /// Type of the expression. nullptr by default.
  types::TypePtr type;
  /// Flag that indicates if all types in an expression are inferred (i.e. if a
  /// type-checking procedure was successful).
  bool done;

  /// Set of attributes.
  int attributes;

  /// Original (pre-transformation) expression
  Expr *origExpr;

public:
  Expr();
  Expr(const Expr &) = default;
  Expr(const Expr &, bool);

  /// Validate a node. Throw ParseASTException if a node is not valid.
  void validate() const;
  /// Get a node type.
  /// @return Type pointer or a nullptr if a type is not set.
  types::TypePtr getType() const;
  /// Set a node type.
  void setType(types::TypePtr type);

  /// Allow pretty-printing to C++ streams.
  friend std::ostream &operator<<(std::ostream &out, const Expr &expr) {
    return out << expr.toString();
  }

  /// Convenience virtual functions to avoid unnecessary dynamic_cast calls.
  virtual bool isId(const std::string &val) const { return false; }
  virtual BinaryExpr *getBinary() { return nullptr; }
  virtual CallExpr *getCall() { return nullptr; }
  virtual DotExpr *getDot() { return nullptr; }
  virtual EllipsisExpr *getEllipsis() { return nullptr; }
  virtual IdExpr *getId() { return nullptr; }
  virtual IfExpr *getIf() { return nullptr; }
  virtual IndexExpr *getIndex() { return nullptr; }
  virtual InstantiateExpr *getInstantiate() { return nullptr; }
  virtual IntExpr *getInt() { return nullptr; }
  virtual ListExpr *getList() { return nullptr; }
  virtual NoneExpr *getNone() { return nullptr; }
  virtual StarExpr *getStar() { return nullptr; }
  virtual KeywordStarExpr *getKwStar() { return nullptr; }
  virtual StmtExpr *getStmtExpr() { return nullptr; }
  virtual StringExpr *getString() { return nullptr; }
  virtual TupleExpr *getTuple() { return nullptr; }
  virtual UnaryExpr *getUnary() { return nullptr; }

  /// Attribute helpers
  bool hasAttr(int attr) const;
  void setAttr(int attr);

  bool isDone() const { return done; }
  void setDone() { done = true; }

  /// @return Type name for IdExprs or instantiations.
  std::string getTypeName();

protected:
  /// Add a type to S-expression string.
  std::string wrapType(const std::string &sexpr) const;
};

/// Function signature parameter helper node (name: type = defaultValue).
struct Param : public codon::SrcObject {
  std::string name;
  Expr *type;
  Expr *defaultValue;
  enum {
    Normal,
    Generic,
    HiddenGeneric
  } status; // 1 for normal generic, 2 for hidden generic

  explicit Param(std::string name = "", Expr *type = nullptr,
                 Expr *defaultValue = nullptr, int generic = 0);
  explicit Param(const SrcInfo &info, std::string name = "", Expr *type = nullptr,
                 Expr *defaultValue = nullptr, int generic = 0);

  std::string toString(int) const;
  Param clone(bool) const;
};

/// None expression.
/// @li None
struct NoneExpr : public Expr {
  NoneExpr();
  NoneExpr(const NoneExpr &, bool);

  std::string toString(int) const override;
  ACCEPT(ASTVisitor);

  NoneExpr *getNone() override { return this; }
};

/// Bool expression (value).
/// @li True
struct BoolExpr : public Expr {
  bool value;

  explicit BoolExpr(bool value);
  BoolExpr(const BoolExpr &, bool);

  std::string toString(int) const override;
  ACCEPT(ASTVisitor);
};

/// Int expression (value.suffix).
/// @li 12
/// @li 13u
/// @li 000_010b
struct IntExpr : public Expr {
  /// Expression value is stored as a string that is parsed during typechecking.
  std::string value;
  /// Number suffix (e.g. "u" for "123u").
  std::string suffix;

  /// Parsed value and sign for "normal" 64-bit integers.
  std::unique_ptr<int64_t> intValue;

  explicit IntExpr(int64_t intValue);
  explicit IntExpr(const std::string &value, std::string suffix = "");
  IntExpr(const IntExpr &, bool);

  std::string toString(int) const override;
  ACCEPT(ASTVisitor);

  IntExpr *getInt() override { return this; }
};

/// Float expression (value.suffix).
/// @li 12.1
/// @li 13.15z
/// @li e-12
struct FloatExpr : public Expr {
  /// Expression value is stored as a string that is parsed during typechecking.
  std::string value;
  /// Number suffix (e.g. "u" for "123u").
  std::string suffix;

  /// Parsed value for 64-bit floats.
  std::unique_ptr<double> floatValue;

  explicit FloatExpr(double floatValue);
  explicit FloatExpr(const std::string &value, std::string suffix = "");
  FloatExpr(const FloatExpr &, bool);

  std::string toString(int) const override;
  ACCEPT(ASTVisitor);
};

/// String expression (prefix"value").
/// @li s'ACGT'
/// @li "fff"
struct StringExpr : public Expr {
  // Vector of {value, prefix} strings.
  std::vector<std::pair<std::string, std::string>> strings;

  explicit StringExpr(std::string value, std::string prefix = "");
  explicit StringExpr(std::vector<std::pair<std::string, std::string>> strings);
  StringExpr(const StringExpr &, bool);

  std::string toString(int) const override;
  ACCEPT(ASTVisitor);

  StringExpr *getString() override { return this; }
  std::string getValue() const;

  Expr *unpack() const;
  Expr *unpackFString(const std::string &) const;
};

/// Identifier expression (value).
struct IdExpr : public Expr {
  std::string value;

  explicit IdExpr(std::string value);
  IdExpr(const IdExpr &, bool);

  std::string toString(int) const override;
  ACCEPT(ASTVisitor);

  bool isId(const std::string &val) const override { return this->value == val; }
  IdExpr *getId() override { return this; }
};

/// Star (unpacking) expression (*what).
/// @li *args
struct StarExpr : public Expr {
  Expr *what;

  explicit StarExpr(Expr *what);
  StarExpr(const StarExpr &, bool);

  std::string toString(int) const override;
  ACCEPT(ASTVisitor);

  StarExpr *getStar() override { return this; }
};

/// KeywordStar (unpacking) expression (**what).
/// @li **kwargs
struct KeywordStarExpr : public Expr {
  Expr *what;

  explicit KeywordStarExpr(Expr *what);
  KeywordStarExpr(const KeywordStarExpr &, bool);

  std::string toString(int) const override;
  ACCEPT(ASTVisitor);

  KeywordStarExpr *getKwStar() override { return this; }
};

/// Tuple expression ((items...)).
/// @li (1, a)
struct TupleExpr : public Expr {
  std::vector<Expr *> items;

  explicit TupleExpr(std::vector<Expr *> items = {});
  TupleExpr(const TupleExpr &, bool);

  std::string toString(int) const override;
  ACCEPT(ASTVisitor);

  TupleExpr *getTuple() override { return this; }
};

/// List expression ([items...]).
/// @li [1, 2]
struct ListExpr : public Expr {
  std::vector<Expr *> items;

  explicit ListExpr(std::vector<Expr *> items = {});
  ListExpr(const ListExpr &, bool);

  std::string toString(int) const override;
  ACCEPT(ASTVisitor);

  ListExpr *getList() override { return this; }
};

/// Set expression ({items...}).
/// @li {1, 2}
struct SetExpr : public Expr {
  std::vector<Expr *> items;

  explicit SetExpr(std::vector<Expr *> items = {});
  SetExpr(const SetExpr &, bool);

  std::string toString(int) const override;
  ACCEPT(ASTVisitor);
};

/// Dictionary expression ({(key: value)...}).
/// Each (key, value) pair is stored as a TupleExpr.
/// @li {'s': 1, 't': 2}
struct DictExpr : public Expr {
  std::vector<Expr *> items;

  explicit DictExpr(std::vector<Expr *> items = {});
  DictExpr(const DictExpr &, bool);

  std::string toString(int) const override;
  ACCEPT(ASTVisitor);
};

/// Generator or comprehension expression [(expr (loops...))].
/// @li [i for i in j]
/// @li (f + 1 for j in k if j for f in j)
struct GeneratorExpr : public Expr {
  /// Generator kind: normal generator, list comprehension, set comprehension.
  enum GeneratorKind {
    Generator,
    ListGenerator,
    SetGenerator,
    TupleGenerator,
    DictGenerator
  };

  GeneratorKind kind;
  Stmt *loops;

  GeneratorExpr(Cache *cache, GeneratorKind kind, Expr *expr,
                std::vector<Stmt *> loops);
  GeneratorExpr(Cache *cache, Expr *key, Expr *expr, std::vector<Stmt *> loops);
  GeneratorExpr(const GeneratorExpr &, bool);

  std::string toString(int) const override;

  int loopCount() const;
  Stmt *getFinalSuite() const;
  Expr *getFinalExpr();
  void setFinalExpr(Expr *);
  void setFinalStmt(Stmt *);
  ACCEPT(ASTVisitor);

private:
  Stmt **getFinalStmt();
  void formCompleteStmt(const std::vector<Stmt *> &);
};

/// Conditional expression [cond if ifexpr else elsexpr].
/// @li 1 if a else 2
struct IfExpr : public Expr {
  Expr *cond, *ifexpr, *elsexpr;

  IfExpr(Expr *cond, Expr *ifexpr, Expr *elsexpr);
  IfExpr(const IfExpr &, bool);

  std::string toString(int) const override;
  ACCEPT(ASTVisitor);

  IfExpr *getIf() override { return this; }
};

/// Unary expression [op expr].
/// @li -56
struct UnaryExpr : public Expr {
  std::string op;
  Expr *expr;

  UnaryExpr(std::string op, Expr *expr);
  UnaryExpr(const UnaryExpr &, bool);

  std::string toString(int) const override;
  ACCEPT(ASTVisitor);

  UnaryExpr *getUnary() override { return this; }
};

/// Binary expression [lexpr op rexpr].
/// @li 1 + 2
/// @li 3 or 4
struct BinaryExpr : public Expr {
  std::string op;
  Expr *lexpr, *rexpr;

  /// True if an expression modifies lhs in-place (e.g. a += b).
  bool inPlace;

  BinaryExpr(Expr *lexpr, std::string op, Expr *rexpr, bool inPlace = false);
  BinaryExpr(const BinaryExpr &, bool);

  std::string toString(int) const override;
  ACCEPT(ASTVisitor);

  BinaryExpr *getBinary() override { return this; }
};

/// Chained binary expression.
/// @li 1 <= x <= 2
struct ChainBinaryExpr : public Expr {
  std::vector<std::pair<std::string, Expr *>> exprs;

  ChainBinaryExpr(std::vector<std::pair<std::string, Expr *>> exprs);
  ChainBinaryExpr(const ChainBinaryExpr &, bool);

  std::string toString(int) const override;
  ACCEPT(ASTVisitor);
};

/// Pipe expression [(op expr)...].
/// op is either "" (only the first item), "|>" or "||>".
/// @li a |> b ||> c
struct PipeExpr : public Expr {
  struct Pipe {
    std::string op;
    Expr *expr;

    Pipe clone(bool) const;
  };

  std::vector<Pipe> items;
  /// Output type of a "prefix" pipe ending at the index position.
  /// Example: for a |> b |> c, inTypes[1] is typeof(a |> b).
  std::vector<types::TypePtr> inTypes;

  explicit PipeExpr(std::vector<Pipe> items);
  PipeExpr(const PipeExpr &, bool);

  std::string toString(int) const override;
  void validate() const;
  ACCEPT(ASTVisitor);
};

/// Index expression (expr[index]).
/// @li a[5]
struct IndexExpr : public Expr {
  Expr *expr, *index;

  IndexExpr(Expr *expr, Expr *index);
  IndexExpr(const IndexExpr &, bool);

  std::string toString(int) const override;
  ACCEPT(ASTVisitor);

  IndexExpr *getIndex() override { return this; }
};

/// Call expression (expr((name=value)...)).
/// @li a(1, b=2)
struct CallExpr : public Expr {
  /// Each argument can have a name (e.g. foo(1, b=5))
  struct Arg : public codon::SrcObject {
    std::string name;
    Expr *value;

    Arg clone(bool) const;

    Arg(const SrcInfo &info, const std::string &name, Expr *value);
    Arg(const std::string &name, Expr *value);
    Arg(Expr *value);
  };

  Expr *expr;
  std::vector<Arg> args;
  /// True if type-checker has processed and re-ordered args.
  bool ordered;
  /// True if the call is partial
  bool partial = false;

  CallExpr(Expr *expr, std::vector<Arg> args = {});
  /// Convenience constructors
  CallExpr(Expr *expr, std::vector<Expr *> args);
  template <typename... Ts>
  CallExpr(Expr *expr, Expr *arg, Ts... args)
      : CallExpr(expr, std::vector<Expr *>{arg, args...}) {}
  CallExpr(const CallExpr &, bool);

  void validate() const;
  std::string toString(int) const override;
  ACCEPT(ASTVisitor);

  CallExpr *getCall() override { return this; }
};

/// Dot (access) expression (expr.member).
/// @li a.b
struct DotExpr : public Expr {
  Expr *expr;
  std::string member;

  DotExpr(Expr *expr, std::string member);
  DotExpr(const DotExpr &, bool);

  std::string toString(int) const override;
  ACCEPT(ASTVisitor);

  DotExpr *getDot() override { return this; }
};

/// Slice expression (st:stop:step).
/// @li 1:10:3
/// @li s::-1
/// @li :::
struct SliceExpr : public Expr {
  /// Any of these can be nullptr to account for partial slices.
  Expr *start, *stop, *step;

  SliceExpr(Expr *start, Expr *stop, Expr *step);
  SliceExpr(const SliceExpr &, bool);

  std::string toString(int) const override;
  ACCEPT(ASTVisitor);
};

/// Ellipsis expression.
/// @li ...
struct EllipsisExpr : public Expr {
  /// True if this is a target partial argument within a PipeExpr.
  /// If true, this node will be handled differently during the type-checking stage.
  enum EllipsisType { PIPE, PARTIAL, STANDALONE } mode;

  explicit EllipsisExpr(EllipsisType mode = STANDALONE);
  EllipsisExpr(const EllipsisExpr &, bool);

  std::string toString(int) const override;
  ACCEPT(ASTVisitor);

  EllipsisExpr *getEllipsis() override { return this; }
};

/// Lambda expression (lambda (vars)...: expr).
/// @li lambda a, b: a + b
struct LambdaExpr : public Expr {
  std::vector<std::string> vars;
  Expr *expr;

  LambdaExpr(std::vector<std::string> vars, Expr *expr);
  LambdaExpr(const LambdaExpr &, bool);

  std::string toString(int) const override;
  ACCEPT(ASTVisitor);
};

/// Yield (send to generator) expression.
/// @li (yield)
struct YieldExpr : public Expr {
  YieldExpr();
  YieldExpr(const YieldExpr &, bool);

  std::string toString(int) const override;
  ACCEPT(ASTVisitor);
};

/// Assignment (walrus) expression (var := expr).
/// @li a := 5 + 3
struct AssignExpr : public Expr {
  Expr *var, *expr;

  AssignExpr(Expr *var, Expr *expr);
  AssignExpr(const AssignExpr &, bool);

  std::string toString(int) const override;
  ACCEPT(ASTVisitor);
};

/// Range expression (start ... end).
/// Used only in match-case statements.
/// @li 1 ... 2
struct RangeExpr : public Expr {
  Expr *start, *stop;

  RangeExpr(Expr *start, Expr *stop);
  RangeExpr(const RangeExpr &, bool);

  std::string toString(int) const override;
  ACCEPT(ASTVisitor);
};

/// The following nodes are created during typechecking.

/// Statement expression (stmts...; expr).
/// Statements are evaluated only if the expression is evaluated
/// (to support short-circuiting).
/// @li (a = 1; b = 2; a + b)
struct StmtExpr : public Expr {
  std::vector<Stmt *> stmts;
  Expr *expr;

  StmtExpr(std::vector<Stmt *> stmts, Expr *expr);
  StmtExpr(Stmt *stmt, Expr *expr);
  StmtExpr(Stmt *stmt, Stmt *stmt2, Expr *expr);
  StmtExpr(const StmtExpr &, bool);

  std::string toString(int) const override;
  ACCEPT(ASTVisitor);

  StmtExpr *getStmtExpr() override { return this; }
};

/// Static tuple indexing expression (expr[index]).
/// @li (1, 2, 3)[2]
struct InstantiateExpr : Expr {
  Expr *typeExpr;
  std::vector<Expr *> typeParams;

  InstantiateExpr(Expr *typeExpr, std::vector<Expr *> typeParams);
  /// Convenience constructor for a single type parameter.
  InstantiateExpr(Expr *typeExpr, Expr *typeParam);
  InstantiateExpr(const InstantiateExpr &, bool);

  std::string toString(int) const override;
  ACCEPT(ASTVisitor);

  InstantiateExpr *getInstantiate() override { return this; }
};

#undef ACCEPT

enum ExprAttr {
  SequenceItem,
  StarSequenceItem,
  List,
  Set,
  Dict,
  Partial,
  Dominated,
  StarArgument,
  KwStarArgument,
  OrderedCall,
  ExternVar,
  __LAST__
};

char getStaticGeneric(Expr *e);

} // namespace codon::ast

template <>
struct fmt::formatter<codon::ast::CallExpr::Arg> : fmt::formatter<std::string_view> {
  template <typename FormatContext>
  auto format(const codon::ast::CallExpr::Arg &p,
              FormatContext &ctx) const -> decltype(ctx.out()) {
    return fmt::format_to(ctx.out(), "({}{})",
                          p.name.empty() ? "" : fmt::format("{} = ", p.name),
                          p.value ? p.value->toString(0) : "-");
  }
};

template <>
struct fmt::formatter<codon::ast::Param> : fmt::formatter<std::string_view> {
  template <typename FormatContext>
  auto format(const codon::ast::Param &p,
              FormatContext &ctx) const -> decltype(ctx.out()) {
    return fmt::format_to(ctx.out(), "{}", p.toString(0));
  }
};
