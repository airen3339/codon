#include "types.h"

#include <algorithm>
#include <memory>
#include <utility>

#include "codon/parser/cache.h"
#include "codon/sir/module.h"
#include "codon/sir/util/irtools.h"
#include "codon/sir/util/iterators.h"
#include "codon/sir/util/visitor.h"
#include "codon/sir/value.h"
#include "codon/util/fmt/format.h"

namespace codon {
namespace ir {
namespace types {
namespace {
std::vector<codon::ast::types::TypePtr>
extractTypes(const std::vector<codon::ast::types::ClassType::Generic> &gens) {
  std::vector<codon::ast::types::TypePtr> ret;
  for (auto &g : gens)
    ret.push_back(g.type);
  return ret;
}
} // namespace

const char Type::NodeId = 0;

std::vector<Generic> Type::doGetGenerics() const {
  if (!astType)
    return {};

  std::vector<Generic> ret;
  for (auto &g : astType->getClass()->generics) {
    if (auto cls = g.type->getClass())
      ret.emplace_back(
          getModule()->getCache()->realizeType(cls, extractTypes(cls->generics)));
    else {
      seqassertn(g.type->getStatic()->expr->staticValue.type == ast::StaticValue::INT,
                 "IR only supports int statics [{}]", g.type->getSrcInfo());
      ret.emplace_back(g.type->getStatic()->expr->staticValue.getInt());
    }
  }

  return ret;
}

Value *Type::doConstruct(std::vector<Value *> args) {
  auto *module = getModule();
  std::vector<Type *> argTypes;
  for (auto *a : args)
    argTypes.push_back(a->getType());

  auto *fn = module->getOrRealizeMethod(this, Module::NEW_MAGIC_NAME, argTypes);
  if (!fn)
    return nullptr;

  return module->Nr<CallInstr>(module->Nr<VarValue>(fn), args);
}

const char PrimitiveType::NodeId = 0;

const char IntType::NodeId = 0;

const char FloatType::NodeId = 0;

const char BoolType::NodeId = 0;

const char ByteType::NodeId = 0;

const char VoidType::NodeId = 0;

const char MemberedType::NodeId = 0;

const char RecordType::NodeId = 0;

RecordType::RecordType(std::string name, std::vector<Type *> fieldTypes,
                       std::vector<std::string> fieldNames)
    : AcceptorExtend(std::move(name)) {
  for (auto i = 0; i < fieldTypes.size(); ++i) {
    fields.emplace_back(fieldNames[i], fieldTypes[i]);
  }
}

RecordType::RecordType(std::string name, std::vector<Type *> mTypes)
    : AcceptorExtend(std::move(name)) {
  for (int i = 0; i < mTypes.size(); ++i) {
    fields.emplace_back(std::to_string(i + 1), mTypes[i]);
  }
}

std::vector<Type *> RecordType::doGetUsedTypes() const {
  std::vector<Type *> ret;
  for (auto &f : fields)
    ret.push_back(const_cast<Type *>(f.getType()));
  return ret;
}

Type *RecordType::getMemberType(const std::string &n) const {
  auto it = std::find_if(fields.begin(), fields.end(),
                         [n](auto &x) { return x.getName() == n; });
  return it->getType();
}

int RecordType::getMemberIndex(const std::string &n) const {
  auto it = std::find_if(fields.begin(), fields.end(),
                         [n](auto &x) { return x.getName() == n; });
  int index = std::distance(fields.begin(), it);
  return (index < fields.size()) ? index : -1;
}

void RecordType::realize(std::vector<Type *> mTypes, std::vector<std::string> mNames) {
  fields.clear();
  for (auto i = 0; i < mTypes.size(); ++i) {
    fields.emplace_back(mNames[i], mTypes[i]);
  }
}

const char RefType::NodeId = 0;

Value *RefType::doConstruct(std::vector<Value *> args) {
  auto *module = getModule();
  auto *argsTuple = util::makeTuple(args, module);
  auto *constructFn = module->getOrRealizeFunc("construct_ref", {argsTuple->getType()},
                                               {this}, "std.internal.gc");
  if (!constructFn)
    return nullptr;

  std::vector<Value *> callArgs = {argsTuple};
  return module->Nr<CallInstr>(module->Nr<VarValue>(constructFn), callArgs);
}

const char FuncType::NodeId = 0;

std::vector<Generic> FuncType::doGetGenerics() const {
  auto t = getAstType();
  if (!t)
    return {};
  auto astType = t->getFunc();
  if (!astType)
    return {};

  std::vector<Generic> ret;
  for (auto &g : astType->funcGenerics) {
    if (auto cls = g.type->getClass())
      ret.emplace_back(
          getModule()->getCache()->realizeType(cls, extractTypes(cls->generics)));
    else {
      seqassertn(g.type->getStatic()->expr->staticValue.type == ast::StaticValue::INT,
                 "IR only supports int statics [{}]", getSrcInfo());
      ret.emplace_back(g.type->getStatic()->expr->staticValue.getInt());
    }
  }

  return ret;
}

std::vector<Type *> FuncType::doGetUsedTypes() const {
  auto ret = argTypes;
  ret.push_back(rType);
  return ret;
}

const char DerivedType::NodeId = 0;

const char PointerType::NodeId = 0;

std::string PointerType::getInstanceName(Type *base) {
  return fmt::format(FMT_STRING("Pointer[{}]"), base->referenceString());
}

const char OptionalType::NodeId = 0;

std::string OptionalType::getInstanceName(Type *base) {
  return fmt::format(FMT_STRING("Optional[{}]"), base->referenceString());
}

const char GeneratorType::NodeId = 0;

std::string GeneratorType::getInstanceName(Type *base) {
  return fmt::format(FMT_STRING("Generator[{}]"), base->referenceString());
}

const char IntNType::NodeId = 0;

std::string IntNType::getInstanceName(unsigned int len, bool sign) {
  return fmt::format(FMT_STRING("{}Int{}"), sign ? "" : "U", len);
}

} // namespace types
} // namespace ir
} // namespace codon
