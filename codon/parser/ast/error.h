#pragma once

#include <stdexcept>
#include <string>
#include <vector>

#include "codon/util/fmt/format.h"

namespace codon {
struct SrcInfo {
  std::string file;
  int line;
  int col;
  int len;
  int id; /// used to differentiate different instances

  SrcInfo(std::string file, int line, int col, int len)
      : file(std::move(file)), line(line), col(col), len(len), id(0) {
    static int nextId = 0;
    id = nextId++;
  };

  SrcInfo() : SrcInfo("", 0, 0, 0) {}

  bool operator==(const SrcInfo &src) const { return id == src.id; }
};

/// Raise a parsing error.
void raise_error(const char *format);
/// Raise a parsing error at a source location p.
void raise_error(const SrcInfo &info, const char *format);
void raise_error(const SrcInfo &info, const std::string &format);

} // namespace codon

namespace codon::exc {

/**
 * Parser error exception.
 * Used for parsing, transformation and type-checking errors.
 */
class ParserException : public std::runtime_error {
public:
  /// These vectors (stacks) store an error stack-trace.
  std::vector<SrcInfo> locations;
  std::vector<std::string> messages;

public:
  ParserException(const std::string &msg, const SrcInfo &info) noexcept
      : std::runtime_error(msg) {
    messages.push_back(msg);
    locations.push_back(info);
  }
  ParserException() noexcept : std::runtime_error("") {}
  explicit ParserException(const std::string &msg) noexcept
      : ParserException(msg, {}) {}
  ParserException(const ParserException &e) noexcept
      : std::runtime_error(e), locations(e.locations), messages(e.messages){};

  /// Add an error message to the current stack trace
  void trackRealize(const std::string &msg, const SrcInfo &info) {
    locations.push_back(info);
    messages.push_back("while realizing " + msg);
  }

  /// Add an error message to the current stack trace
  void track(const std::string &msg, const SrcInfo &info) {
    locations.push_back(info);
    messages.push_back(msg);
  }
};

enum Error {
  CALL_NAME_ORDER,
  CALL_NAME_STAR,
  CALL_ELLIPSIS,
  IMPORT_IDENTIFIER,
  IMPORT_FN,
  FN_LLVM,
  FN_LAST_KWARG,
  FN_MULTIPLE_ARGS,
  FN_DEFAULT_STARARG,
  FN_ARG_TWICE,
  FN_DEFAULT,
  FN_C_DEFAULT,
  FN_C_TYPE,
  FN_SINGLE_DECORATOR,
  CLASS_EXTENSION,
  CLASS_MISSING_TYPE,
  CLASS_ARG_TWICE,
  CLASS_BAD_DECORATOR,
  CLASS_MULTIPLE_DECORATORS,
  CLASS_SINGLE_DECORATOR,
  CLASS_NONSTATIC_DECORATOR,
  CLASS_BAD_DECORATOR_ARG,
  ID_NOT_FOUND,
  ID_CANNOT_CAPTURE,
  UNION_TOO_BIG,
  COMPILER_NO_FILE,
  ID_NONLOCAL,
  IMPORT_NO_MODULE,
  IMPORT_NO_NAME,
  DEL_NOT_ALLOWED,
  DEL_INVALID,
  ASSIGN_INVALID,
  ASSIGN_LOCAL_REFERENCE,
  ASSIGN_MULTI_STAR,
  INT_RANGE,
  FLOAT_RANGE,
  STR_FSTRING_BALANCE_EXTRA,
  STR_FSTRING_BALANCE_MISSING,
  CALL_NO_TYPE,
  CALL_TUPLE_COMPREHENSION,
  CALL_NAMEDTUPLE,
  CALL_PARTIAL,
  EXPECTED_TOPLEVEL,
  CLASS_ID_NOT_FOUND,
  CLASS_INVALID_BIND,
  CLASS_NO_INHERIT,
  CLASS_TUPLE_INHERIT,
  CLASS_GENERIC_MISMATCH,
  CLASS_BAD_MRO,
  CLASS_BAD_ATTR,
  MATCH_MULTI_ELLIPSIS,
  FN_OUTSIDE_ERROR,
  FN_GLOBAL_ASSIGNED,
  FN_GLOBAL_NOT_FOUND,
  FN_NO_DECORATORS,
  FN_BAD_LLVM,
  FN_REALIZE_BUILTIN,
  EXPECTED_LOOP,
  LOOP_DECORATOR,
  BAD_STATIC,
  EXPECTED_TYPE,
  UNEXPECTED_TYPE,
  __END__
};

template <class... TA> std::string Emsg(Error e, TA... args) {
  switch (e) {
  /// Validations
  case Error::CALL_NAME_ORDER:
    return fmt::format("positional argument follows keyword argument");
  case Error::CALL_NAME_STAR:
    return fmt::format("cannot use starred expression here");
  case Error::CALL_ELLIPSIS:
    return fmt::format("multiple ellipsis expressions");
  case Error::IMPORT_IDENTIFIER:
    return fmt::format("expected identifier");
  case Error::IMPORT_FN:
    return fmt::format(
        "function signatures only allowed when importing C or Python functions");
  case Error::FN_LLVM:
    return fmt::format("return types required for LLVM and C functions");
  case Error::FN_LAST_KWARG:
    return fmt::format("kwargs must be the last argument");
  case Error::FN_MULTIPLE_ARGS:
    return fmt::format("multiple star arguments provided");
  case Error::FN_DEFAULT_STARARG:
    return fmt::format("star arguments cannot have default values");
  case Error::FN_ARG_TWICE:
    return fmt::format("duplicate argument '{}' in function definition", args...);
  case Error::FN_DEFAULT:
    return fmt::format("non-default argument '{}' follows default argument", args...);
  case Error::FN_C_DEFAULT:
    return fmt::format(
        "argument '{}' within C function definition cannot have default value",
        args...);
  case Error::FN_C_TYPE:
    return fmt::format(
        "argument '{}' within C function definition requires type annotation", args...);
  case Error::FN_SINGLE_DECORATOR:
    return fmt::format("cannot combine '@{}' with other attributes or decorators",
                       args...);
  case Error::CLASS_EXTENSION:
    return fmt::format(
        "class extensions cannot inherit or specify data attributes or generics");
  case Error::CLASS_MISSING_TYPE:
    return fmt::format("type required for data attribute '{}'", args...);
  case Error::CLASS_ARG_TWICE:
    return fmt::format("duplicate data attribute '{}' in class definition", args...);
  case Error::CLASS_BAD_DECORATOR:
    return fmt::format("unsupported class decorator");
  case Error::CLASS_MULTIPLE_DECORATORS:
    return fmt::format("duplicate decorator '@{}' in class definition", args...);
  case Error::CLASS_SINGLE_DECORATOR:
    return fmt::format("cannot combine '@{}' with other attributes or decorators",
                       args...);
  case Error::CLASS_NONSTATIC_DECORATOR:
    return fmt::format("class decorator arguments must be compile-time static values");
  case Error::CLASS_BAD_DECORATOR_ARG:
    return fmt::format("class decorator got unexpected argument");
    /// Simplification

  case Error::ID_NOT_FOUND:
    return fmt::format("name '{}' is not defined", args...);
  case Error::ID_CANNOT_CAPTURE:
    return fmt::format("name '{}' cannot be captured", args...);
  case Error::ID_NONLOCAL:
    return fmt::format("no binding for nonlocal '{}' found", args...);
  case Error::IMPORT_NO_MODULE:
    return fmt::format("no module named '{}'", args...);
  case Error::IMPORT_NO_NAME:
    return fmt::format("cannot import name '{}' from '{}'", args...);
  case Error::DEL_NOT_ALLOWED:
    return fmt::format("name '{}' cannot be deleted", args...);
  case Error::DEL_INVALID:
    return fmt::format("cannot delete the given expression", args...);
  case Error::ASSIGN_INVALID:
    return fmt::format("cannot assign to the provided expression");
  case Error::ASSIGN_LOCAL_REFERENCE:
    return fmt::format("local variable '{}' referenced before assignment", args...);
  case Error::ASSIGN_MULTI_STAR:
    return fmt::format("multiple starred expressions in assignment");

  case Error::INT_RANGE:
    return fmt::format("integer '{}' cannot fit into 64 bits (Int[64])", args...);
  case Error::FLOAT_RANGE:
    return fmt::format("float '{}' cannot fit into 64 bits (double)", args...);
  case Error::STR_FSTRING_BALANCE_EXTRA:
    return fmt::format("expecting '}' in f-string");
  case Error::STR_FSTRING_BALANCE_MISSING:
    return fmt::format("single '{' is not allowed in f-string");

  case Error::CALL_NO_TYPE:
    return fmt::format("cannot use type() in function and class signatures", args...);
  case Error::CALL_TUPLE_COMPREHENSION:
    return fmt::format(
        "tuple constructor does not accept nested or conditioned comprehensions",
        args...);
  case Error::CALL_NAMEDTUPLE:
    return fmt::format("namedtuple() takes 2 static arguments", args...);
  case Error::CALL_PARTIAL:
    return fmt::format("partial() takes 1 or more arguments", args...);
  case Error::EXPECTED_TOPLEVEL:
    return fmt::format("{} must be a top-level statement", args...);
  case Error::CLASS_ID_NOT_FOUND:
    // Note that type aliases are not valid class names
    return fmt::format("class name '{}' is not defined", args...);
  case Error::CLASS_INVALID_BIND:
    // Note that type aliases are not valid class names
    return fmt::format("cannot bind '{}' to class or function", args...);
  case Error::CLASS_NO_INHERIT:
    return fmt::format("{} classes cannot inherit other classes", args...);
  case Error::CLASS_TUPLE_INHERIT:
    return fmt::format("reference classes cannot inherit tuple classes");
  case Error::CLASS_GENERIC_MISMATCH:
    return fmt::format("expected {} generics", args...);
  case Error::CLASS_BAD_MRO:
    return fmt::format("inconsistent class hierarchy");
  case Error::CLASS_BAD_ATTR:
    return fmt::format("unexpected expression in class definition");

  case Error::MATCH_MULTI_ELLIPSIS:
    return fmt::format("no binding for nonlocal '{}' found", args...);
  case Error::FN_OUTSIDE_ERROR:
    return fmt::format("'{}' outside function", args...);
  case Error::FN_GLOBAL_ASSIGNED:
    return fmt::format("name '{}' is assigned to before global declaration", args...);
  case Error::FN_GLOBAL_NOT_FOUND:
    return fmt::format("no binding for {} '{}' found", args...);
  case Error::FN_NO_DECORATORS:
    return fmt::format("class methods cannot be decorated", args...);
  case Error::FN_BAD_LLVM:
    return fmt::format("invalid LLVM code");
  case Error::FN_REALIZE_BUILTIN:
    return fmt::format("builtin, exported and external functions cannot be generic");

  case Error::EXPECTED_LOOP:
    return fmt::format("'{}' outside loop", args...);
  case Error::LOOP_DECORATOR:
    return fmt::format("invalid loop decorator");
  case Error::BAD_STATIC:
    return fmt::format("expected int or str (only integers and strings can be static)");
  case Error::EXPECTED_TYPE:
    return fmt::format("expected '{}' expression", args...);
  case Error::UNEXPECTED_TYPE:
    return fmt::format("expected runtime expression, got type instead");

  /// Typechecking
  case Error::UNION_TOO_BIG:
    return fmt::format(
        "union exceeded its maximum capacity (contains more than {} types)");

  case Error::COMPILER_NO_FILE:
    return fmt::format("cannot open file '{}' for parsing");

  default:
    assert(false);
  }
}

template <class... TA> void E(Error e, const SrcInfo &o, TA... args) {
  auto msg = Emsg(e, args...);
  codon::raise_error(o, msg);
}

} // namespace codon::exc
