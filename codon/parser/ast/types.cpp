#include "types.h"

#include <memory>
#include <string>
#include <vector>

#include "codon/parser/ast.h"
#include "codon/parser/visitors/format/format.h"
#include "codon/parser/visitors/typecheck/typecheck.h"

namespace codon::ast::types {

void Type::Unification::undo() {
  for (int i = int(linked.size()) - 1; i >= 0; i--) {
    linked[i]->kind = LinkType::Unbound;
    linked[i]->type = nullptr;
  }
  for (int i = int(leveled.size()) - 1; i >= 0; i--) {
    seqassertn(leveled[i].first->kind == LinkType::Unbound, "not unbound [{}]",
               leveled[i].first->getSrcInfo());
    leveled[i].first->level = leveled[i].second;
  }
  for (auto &t : traits)
    t->trait = nullptr;
}
TypePtr Type::follow() { return shared_from_this(); }
std::vector<std::shared_ptr<Type>> Type::getUnbounds() const { return {}; }
std::string Type::prettyString() const { return debugString(0); }
std::string Type::toString() const { return debugString(1); }
bool Type::is(const std::string &s) { return getClass() && getClass()->name == s; }
char Type::isStaticType() {
  auto t = follow();
  if (auto s = t->getStatic())
    return char(s->expr->staticValue.type);
  if (auto l = t->getLink())
    return l->isStatic;
  return false;
}

TypePtr Type::makeType(const std::string &name, const std::string &niceName,
                       bool isRecord) {
  if (name == "Union") {
    return std::make_shared<UnionType>();
  }
  if (isRecord) {
    return std::make_shared<RecordType>(name, niceName);
  }
  return std::make_shared<ClassType>(name, niceName);
}
std::shared_ptr<StaticType> Type::makeStatic(const ExprPtr &expr,
                                             std::shared_ptr<TypeContext> ctx) {
  return std::make_shared<StaticType>(expr, ctx);
}

bool Trait::canRealize() const { return false; }
bool Trait::isInstantiated() const { return false; }
std::string Trait::debugString(char mode) const { return ""; }
std::string Trait::realizedName() const { return ""; }

LinkType::LinkType(Kind kind, int id, int level, TypePtr type, char isStatic,
                   std::shared_ptr<Trait> trait, TypePtr defaultType,
                   std::string genericName)
    : kind(kind), id(id), level(level), type(move(type)), isStatic(isStatic),
      trait(move(trait)), genericName(move(genericName)),
      defaultType(move(defaultType)) {
  seqassert((this->type && kind == Link) || (!this->type && kind == Generic) ||
                (!this->type && kind == Unbound),
            "inconsistent link state");
}
LinkType::LinkType(TypePtr type)
    : kind(Link), id(0), level(0), type(move(type)), isStatic(0), trait(nullptr),
      defaultType(nullptr) {
  seqassert(this->type, "link to nullptr");
}
int LinkType::unify(Type *typ, Unification *undo) {
  if (kind == Link) {
    // Case 1: Just follow the link
    return type->unify(typ, undo);
  } else if (kind == Generic) {
    // Case 2: Generic types cannot be unified.
    return -1;
  } else {
    // Case 3: Unbound unification
    if (isStaticType() != typ->isStaticType())
      return -1;
    if (auto ts = typ->getStatic()) {
      if (ts->expr->getId())
        return unify(ts->generics[0].type.get(), undo);
    }
    if (auto t = typ->getLink()) {
      if (t->kind == Link)
        return t->type->unify(this, undo);
      else if (t->kind == Generic)
        return -1;
      else {
        if (id == t->id) {
          // Identical unbound types get a score of 1
          return 1;
        } else if (id < t->id) {
          // Always merge a newer type into the older type (e.g. keep the types with
          // lower IDs around).
          return t->unify(this, undo);
        }
      }
    }
    // Ensure that we do not have recursive unification! (e.g. unify ?1 with list[?1])
    if (occurs(typ, undo))
      return -1;

    if (trait && trait->unify(typ, undo) == -1)
      return -1;

    // ⚠️ Unification: destructive part.
    seqassert(!type, "type has been already unified or is in inconsistent state");
    if (undo) {
      LOG_TYPECHECK("[unify] {} := {}", id, typ->debugString(true));
      // Link current type to typ and ensure that this modification is recorded in undo.
      undo->linked.push_back(this);
      kind = Link;
      seqassert(!typ->getLink() || typ->getLink()->kind != Unbound ||
                    typ->getLink()->id <= id,
                "type unification is not consistent");
      type = typ->follow();
      if (auto t = type->getLink())
        if (trait && t->kind == Unbound && !t->trait) {
          undo->traits.push_back(t.get());
          t->trait = trait;
        }
    }
    return 0;
  }
}
TypePtr LinkType::generalize(int atLevel) {
  if (kind == Generic) {
    return shared_from_this();
  } else if (kind == Unbound) {
    if (level >= atLevel)
      return std::make_shared<LinkType>(
          Generic, id, 0, nullptr, isStatic,
          trait ? std::static_pointer_cast<Trait>(trait->generalize(atLevel)) : nullptr,
          defaultType ? defaultType->generalize(atLevel) : nullptr, genericName);
    else
      return shared_from_this();
  } else {
    seqassert(type, "link is null");
    return type->generalize(atLevel);
  }
}
TypePtr LinkType::instantiate(int atLevel, int *unboundCount,
                              std::unordered_map<int, TypePtr> *cache) {
  if (kind == Generic) {
    if (cache && cache->find(id) != cache->end())
      return (*cache)[id];
    auto t = std::make_shared<LinkType>(
        Unbound, unboundCount ? (*unboundCount)++ : id, atLevel, nullptr, isStatic,
        trait ? std::static_pointer_cast<Trait>(
                    trait->instantiate(atLevel, unboundCount, cache))
              : nullptr,
        defaultType ? defaultType->instantiate(atLevel, unboundCount, cache) : nullptr,
        genericName);
    if (cache)
      (*cache)[id] = t;
    return t;
  } else if (kind == Unbound) {
    return shared_from_this();
  } else {
    seqassert(type, "link is null");
    return type->instantiate(atLevel, unboundCount, cache);
  }
}
TypePtr LinkType::follow() {
  if (kind == Link)
    return type->follow();
  else
    return shared_from_this();
}
std::vector<TypePtr> LinkType::getUnbounds() const {
  if (kind == Unbound)
    return {std::const_pointer_cast<Type>(shared_from_this())};
  else if (kind == Link)
    return type->getUnbounds();
  return {};
}
bool LinkType::canRealize() const {
  if (kind != Link)
    return false;
  else
    return type->canRealize();
}
bool LinkType::isInstantiated() const { return kind == Link && type->isInstantiated(); }
std::string LinkType::debugString(char mode) const {
  if (kind == Unbound || kind == Generic)
    return (mode == 2) ? fmt::format("{}{}{}", kind == Unbound ? '?' : '#', id,
                                     trait ? ":" + trait->debugString(mode) : "")
                       : (genericName.empty() ? "?" : genericName);
  else
    return type->debugString(mode);
}
std::string LinkType::realizedName() const {
  if (kind == Unbound || kind == Generic)
    return "?";
  seqassert(kind == Link, "unexpected generic link");
  return type->realizedName();
}
std::shared_ptr<LinkType> LinkType::getUnbound() {
  if (kind == Unbound)
    return std::static_pointer_cast<LinkType>(shared_from_this());
  if (kind == Link)
    return type->getUnbound();
  return nullptr;
}
bool LinkType::occurs(Type *typ, Type::Unification *undo) {
  if (auto tl = typ->getLink()) {
    if (tl->kind == Unbound) {
      if (tl->id == id)
        return true;
      if (tl->trait && occurs(tl->trait.get(), undo))
        return true;
      if (undo && tl->level > level) {
        undo->leveled.emplace_back(make_pair(tl.get(), tl->level));
        tl->level = level;
      }
      return false;
    } else if (tl->kind == Link) {
      return occurs(tl->type.get(), undo);
    } else {
      return false;
    }
  } else if (auto ts = typ->getStatic()) {
    for (auto &g : ts->generics)
      if (g.type && occurs(g.type.get(), undo))
        return true;
    return false;
  }
  if (auto tc = typ->getClass()) {
    for (auto &g : tc->generics)
      if (g.type && occurs(g.type.get(), undo))
        return true;
    if (auto tr = typ->getRecord())
      for (auto &t : tr->args)
        if (occurs(t.get(), undo))
          return true;
    return false;
  } else {
    return false;
  }
}

ClassType::ClassType(std::string name, std::string niceName,
                     std::vector<Generic> generics, std::vector<Generic> hiddenGenerics)
    : name(move(name)), niceName(move(niceName)), generics(move(generics)),
      hiddenGenerics(move(hiddenGenerics)) {}
ClassType::ClassType(const ClassTypePtr &base)
    : name(base->name), niceName(base->niceName), generics(base->generics),
      hiddenGenerics(base->hiddenGenerics) {}
int ClassType::unify(Type *typ, Unification *us) {
  if (auto tc = typ->getClass()) {
    // Check names.
    if (name != tc->name)
      return -1;
    // Check generics.
    int s1 = 3, s = 0;
    if (generics.size() != tc->generics.size())
      return -1;
    for (int i = 0; i < generics.size(); i++) {
      if ((s = generics[i].type->unify(tc->generics[i].type.get(), us)) == -1)
        return -1;
      s1 += s;
    }
    return s1;
  } else if (auto tl = typ->getLink()) {
    return tl->unify(this, us);
  } else {
    return -1;
  }
}
TypePtr ClassType::generalize(int atLevel) {
  auto g = generics, hg = hiddenGenerics;
  for (auto &t : g)
    t.type = t.type ? t.type->generalize(atLevel) : nullptr;
  for (auto &t : hg)
    t.type = t.type ? t.type->generalize(atLevel) : nullptr;
  auto c = std::make_shared<ClassType>(name, niceName, g, hg);
  c->setSrcInfo(getSrcInfo());
  return c;
}
TypePtr ClassType::instantiate(int atLevel, int *unboundCount,
                               std::unordered_map<int, TypePtr> *cache) {
  auto g = generics, hg = hiddenGenerics;
  for (auto &t : g)
    t.type = t.type ? t.type->instantiate(atLevel, unboundCount, cache) : nullptr;
  for (auto &t : hg)
    t.type = t.type ? t.type->instantiate(atLevel, unboundCount, cache) : nullptr;
  auto c = std::make_shared<ClassType>(name, niceName, g, hg);
  c->setSrcInfo(getSrcInfo());
  return c;
}
std::vector<TypePtr> ClassType::getUnbounds() const {
  std::vector<TypePtr> u;
  for (auto &t : generics)
    if (t.type) {
      auto tu = t.type->getUnbounds();
      u.insert(u.begin(), tu.begin(), tu.end());
    }
  for (auto &t : hiddenGenerics)
    if (t.type) {
      auto tu = t.type->getUnbounds();
      u.insert(u.begin(), tu.begin(), tu.end());
    }
  return u;
}
bool ClassType::canRealize() const {
  return std::all_of(generics.begin(), generics.end(),
                     [](auto &t) { return !t.type || t.type->canRealize(); }) &&
         std::all_of(hiddenGenerics.begin(), hiddenGenerics.end(),
                     [](auto &t) { return !t.type || t.type->canRealize(); });
}
bool ClassType::isInstantiated() const {
  return std::all_of(generics.begin(), generics.end(),
                     [](auto &t) { return !t.type || t.type->isInstantiated(); }) &&
         std::all_of(hiddenGenerics.begin(), hiddenGenerics.end(),
                     [](auto &t) { return !t.type || t.type->isInstantiated(); });
}
std::string ClassType::debugString(char mode) const {
  std::vector<std::string> gs;
  for (auto &a : generics)
    if (!a.name.empty())
      gs.push_back(a.type->debugString(mode));
  if ((mode == 2) && !hiddenGenerics.empty()) {
    for (auto &a : hiddenGenerics)
      if (!a.name.empty())
        gs.push_back("-" + a.type->debugString(mode));
  }
  // Special formatting for Functions and Tuples
  auto n = mode == 0 ? niceName : name;
  if (startswith(n, TYPE_TUPLE))
    n = "Tuple";
  return fmt::format("{}{}", n, gs.empty() ? "" : fmt::format("[{}]", join(gs, ",")));
}
std::string ClassType::realizedName() const {
  if (!_rn.empty())
    return _rn;

  std::vector<std::string> gs;
  for (auto &a : generics)
    if (!a.name.empty())
      gs.push_back(a.type->realizedName());
  std::string s = join(gs, ",");
  if (canRealize())
    const_cast<ClassType *>(this)->_rn =
        fmt::format("{}{}", name, s.empty() ? "" : fmt::format("[{}]", s));
  return _rn;
}
std::string ClassType::realizedTypeName() const {
  return this->ClassType::realizedName();
}

RecordType::RecordType(std::string name, std::string niceName,
                       std::vector<Generic> generics, std::vector<TypePtr> args)
    : ClassType(move(name), move(niceName), move(generics)), args(move(args)) {}
RecordType::RecordType(const ClassTypePtr &base, std::vector<TypePtr> args)
    : ClassType(base), args(move(args)) {}
int RecordType::unify(Type *typ, Unification *us) {
  if (auto tr = typ->getRecord()) {
    // Handle int <-> Int[64]
    if (name == "int" && tr->name == "Int")
      return tr->unify(this, us);
    if (tr->name == "int" && name == "Int") {
      auto t64 = std::make_shared<StaticType>(64);
      return generics[0].type->unify(t64.get(), us);
    }

    int s1 = 2, s = 0;
    if (args.size() != tr->args.size())
      return -1;
    for (int i = 0; i < args.size(); i++) {
      if ((s = args[i]->unify(tr->args[i].get(), us)) != -1)
        s1 += s;
      else
        return -1;
    }
    // Handle Tuple<->@tuple: when unifying tuples, only record members matter.
    if (startswith(name, TYPE_TUPLE) || startswith(tr->name, TYPE_TUPLE)) {
      return s1 + int(name == tr->name);
    }
    return this->ClassType::unify(tr.get(), us);
  } else if (auto t = typ->getLink()) {
    return t->unify(this, us);
  } else {
    return -1;
  }
}
TypePtr RecordType::generalize(int atLevel) {
  auto c = std::static_pointer_cast<ClassType>(this->ClassType::generalize(atLevel));
  auto a = args;
  for (auto &t : a)
    t = t->generalize(atLevel);
  return std::make_shared<RecordType>(c, a);
}
TypePtr RecordType::instantiate(int atLevel, int *unboundCount,
                                std::unordered_map<int, TypePtr> *cache) {
  auto c = std::static_pointer_cast<ClassType>(
      this->ClassType::instantiate(atLevel, unboundCount, cache));
  auto a = args;
  for (auto &t : a)
    t = t->instantiate(atLevel, unboundCount, cache);
  return std::make_shared<RecordType>(c, a);
}
std::vector<TypePtr> RecordType::getUnbounds() const {
  std::vector<TypePtr> u;
  for (auto &a : args) {
    auto tu = a->getUnbounds();
    u.insert(u.begin(), tu.begin(), tu.end());
  }
  auto tu = this->ClassType::getUnbounds();
  u.insert(u.begin(), tu.begin(), tu.end());
  return u;
}
bool RecordType::canRealize() const {
  return std::all_of(args.begin(), args.end(),
                     [](auto &a) { return a->canRealize(); }) &&
         this->ClassType::canRealize();
}
bool RecordType::isInstantiated() const {
  return std::all_of(args.begin(), args.end(),
                     [](auto &a) { return a->isInstantiated(); }) &&
         this->ClassType::isInstantiated();
}
std::string RecordType::debugString(char mode) const {
  return fmt::format("{}", this->ClassType::debugString(mode));
}
std::shared_ptr<RecordType> RecordType::getHeterogenousTuple() {
  seqassert(canRealize(), "{} not realizable", toString());
  if (args.size() > 1) {
    std::string first = args[0]->realizedName();
    for (int i = 1; i < args.size(); i++)
      if (args[i]->realizedName() != first)
        return getRecord();
  }
  return nullptr;
}

FuncType::FuncType(const std::shared_ptr<RecordType> &baseType, FunctionStmt *ast,
                   std::vector<Generic> funcGenerics, TypePtr funcParent)
    : RecordType(*baseType), ast(ast), funcGenerics(move(funcGenerics)),
      funcParent(move(funcParent)) {}
int FuncType::unify(Type *typ, Unification *us) {
  if (this == typ)
    return 0;
  int s1 = 2, s = 0;
  if (auto t = typ->getFunc()) {
    // Check if names and parents match.
    if (ast->name != t->ast->name || (bool(funcParent) ^ bool(t->funcParent)))
      return -1;
    if (funcParent && (s = funcParent->unify(t->funcParent.get(), us)) == -1)
      return -1;
    s1 += s;
    // Check if function generics match.
    seqassert(funcGenerics.size() == t->funcGenerics.size(),
              "generic size mismatch for {}", ast->name);
    for (int i = 0; i < funcGenerics.size(); i++) {
      if ((s = funcGenerics[i].type->unify(t->funcGenerics[i].type.get(), us)) == -1)
        return -1;
      s1 += s;
    }
  }
  s = this->RecordType::unify(typ, us);
  return s == -1 ? s : s1 + s;
}
TypePtr FuncType::generalize(int atLevel) {
  auto g = funcGenerics;
  for (auto &t : g)
    t.type = t.type ? t.type->generalize(atLevel) : nullptr;
  auto p = funcParent ? funcParent->generalize(atLevel) : nullptr;
  return std::make_shared<FuncType>(
      std::static_pointer_cast<RecordType>(this->RecordType::generalize(atLevel)), ast,
      g, p);
}
TypePtr FuncType::instantiate(int atLevel, int *unboundCount,
                              std::unordered_map<int, TypePtr> *cache) {
  auto g = funcGenerics;
  for (auto &t : g)
    if (t.type) {
      t.type = t.type->instantiate(atLevel, unboundCount, cache);
      if (cache && cache->find(t.id) == cache->end())
        (*cache)[t.id] = t.type;
    }
  auto p = funcParent ? funcParent->instantiate(atLevel, unboundCount, cache) : nullptr;
  return std::make_shared<FuncType>(
      std::static_pointer_cast<RecordType>(
          this->RecordType::instantiate(atLevel, unboundCount, cache)),
      ast, g, p);
}
std::vector<TypePtr> FuncType::getUnbounds() const {
  std::vector<TypePtr> u;
  for (auto &t : funcGenerics)
    if (t.type) {
      auto tu = t.type->getUnbounds();
      u.insert(u.begin(), tu.begin(), tu.end());
    }
  if (funcParent) {
    auto tu = funcParent->getUnbounds();
    u.insert(u.begin(), tu.begin(), tu.end());
  }
  // Important: return type unbounds are not important, so skip them.
  for (auto &a : getArgTypes()) {
    auto tu = a->getUnbounds();
    u.insert(u.begin(), tu.begin(), tu.end());
  }
  return u;
}
bool FuncType::canRealize() const {
  // Important: return type does not have to be realized.
  bool skipSelf = ast->hasAttr(Attr::RealizeWithoutSelf);

  auto args = getArgTypes();
  for (int ai = skipSelf; ai < args.size(); ai++)
    if (!args[ai]->getFunc() && !args[ai]->canRealize())
      return false;
  bool generics = std::all_of(funcGenerics.begin(), funcGenerics.end(),
                              [](auto &a) { return !a.type || a.type->canRealize(); });
  if (!skipSelf)
    generics &= (!funcParent || funcParent->canRealize());
  return generics;
}
bool FuncType::isInstantiated() const {
  TypePtr removed = nullptr;
  auto retType = getRetType();
  if (retType->getFunc() && retType->getFunc()->funcParent.get() == this) {
    removed = retType->getFunc()->funcParent;
    retType->getFunc()->funcParent = nullptr;
  }
  auto res = std::all_of(funcGenerics.begin(), funcGenerics.end(),
                         [](auto &a) { return !a.type || a.type->isInstantiated(); }) &&
             (!funcParent || funcParent->isInstantiated()) &&
             this->RecordType::isInstantiated();
  if (removed)
    retType->getFunc()->funcParent = removed;
  return res;
}
std::string FuncType::debugString(char mode) const {
  std::vector<std::string> gs;
  for (auto &a : funcGenerics)
    if (!a.name.empty())
      gs.push_back(a.type->debugString(mode));
  std::string s = join(gs, ",");
  std::vector<std::string> as;
  // Important: return type does not have to be realized.
  if (mode == 2)
    as.push_back(getRetType()->debugString(mode));
  for (auto &a : getArgTypes())
    as.push_back(a->debugString(mode));
  std::string a = join(as, ",");
  s = s.empty() ? a : join(std::vector<std::string>{a, s}, ",");
  return fmt::format("{}{}", ast->name, s.empty() ? "" : fmt::format("[{}]", s));
}
std::string FuncType::realizedName() const {
  std::vector<std::string> gs;
  for (auto &a : funcGenerics)
    if (!a.name.empty())
      gs.push_back(a.type->realizedName());
  std::string s = join(gs, ",");
  std::vector<std::string> as;
  // Important: return type does not have to be realized.
  for (auto &a : getArgTypes())
    as.push_back(a->getFunc() ? a->getFunc()->realizedName() : a->realizedName());
  std::string a = join(as, ",");
  s = s.empty() ? a : join(std::vector<std::string>{a, s}, ",");
  return fmt::format("{}{}{}", funcParent ? funcParent->realizedName() + ":" : "",
                     ast->name, s.empty() ? "" : fmt::format("[{}]", s));
}

PartialType::PartialType(const std::shared_ptr<RecordType> &baseType,
                         std::shared_ptr<FuncType> func, std::vector<char> known)
    : RecordType(*baseType), func(move(func)), known(move(known)) {}
int PartialType::unify(Type *typ, Unification *us) {
  return this->RecordType::unify(typ, us);
}
TypePtr PartialType::generalize(int atLevel) {
  return std::make_shared<PartialType>(
      std::static_pointer_cast<RecordType>(this->RecordType::generalize(atLevel)), func,
      known);
}
TypePtr PartialType::instantiate(int atLevel, int *unboundCount,
                                 std::unordered_map<int, TypePtr> *cache) {
  auto rec = std::static_pointer_cast<RecordType>(
      this->RecordType::instantiate(atLevel, unboundCount, cache));
  return std::make_shared<PartialType>(rec, func, known);
}
std::string PartialType::debugString(char mode) const {
  std::vector<std::string> gs;
  for (auto &a : generics)
    if (!a.name.empty())
      gs.push_back(a.type->debugString(mode));
  std::vector<std::string> as;
  int i = 0, gi = 0;
  for (; i < known.size(); i++)
    if (func->ast->args[i].status == Param::Normal) {
      if (!known[i])
        as.emplace_back("...");
      else
        as.emplace_back(gs[gi++]);
    }
  return fmt::format("{}[{}]", mode != 2 ? func->ast->name : func->debugString(mode),
                     join(as, ","));
}
std::string PartialType::realizedName() const {
  std::vector<std::string> gs;
  gs.push_back(func->ast->name);
  for (auto &a : generics)
    if (!a.name.empty())
      gs.push_back(a.type->realizedName());
  std::string s = join(gs, ",");
  return fmt::format("{}{}", name, s.empty() ? "" : fmt::format("[{}]", s));
}

StaticType::StaticType(const std::shared_ptr<Expr> &e, std::shared_ptr<TypeContext> ctx)
    : expr(e->clone()), typeCtx(move(ctx)) {
  if (!expr->isStatic() || !expr->staticValue.evaluated) {
    std::unordered_set<std::string> seen;
    parseExpr(expr, seen);
  }
}
StaticType::StaticType(std::vector<ClassType::Generic> generics,
                       const std::shared_ptr<Expr> &e,
                       std::shared_ptr<TypeContext> typeCtx)
    : generics(move(generics)), expr(e->clone()), typeCtx(move(typeCtx)) {}
StaticType::StaticType(int64_t i)
    : expr(std::make_shared<IntExpr>(i)), typeCtx(nullptr) {}
StaticType::StaticType(const std::string &s)
    : expr(std::make_shared<StringExpr>(s)), typeCtx(nullptr) {}
int StaticType::unify(Type *typ, Unification *us) {
  if (auto t = typ->getStatic()) {
    if (canRealize())
      expr->staticValue = evaluate();
    if (t->canRealize())
      t->expr->staticValue = t->evaluate();
    // Check if both types are already evaluated.
    if (expr->staticValue.type != t->expr->staticValue.type)
      return -1;
    if (expr->staticValue.evaluated && t->expr->staticValue.evaluated)
      return expr->staticValue == t->expr->staticValue ? 2 : -1;
    else if (expr->staticValue.evaluated && !t->expr->staticValue.evaluated)
      return typ->unify(this, us);

    // Right now, *this is not evaluated
    // Let us see can we unify it with other _if_ it is a simple IdExpr?
    if (expr->getId() && t->expr->staticValue.evaluated) {
      return generics[0].type->unify(typ, us);
    }

    // At this point, *this is a complex expression (e.g. A+1).
    seqassert(!generics.empty(), "unevaluated simple expression");
    if (generics.size() != t->generics.size())
      return -1;

    int s1 = 2, s = 0;
    if (!(expr->getId() && t->expr->getId()) && expr->toString() != t->expr->toString())
      return -1;
    for (int i = 0; i < generics.size(); i++) {
      if ((s = generics[i].type->unify(t->generics[i].type.get(), us)) == -1)
        return -1;
      s1 += s;
    }
    return s1;
  } else if (auto tl = typ->getLink()) {
    return tl->unify(this, us);
  }
  return -1;
}
TypePtr StaticType::generalize(int atLevel) {
  auto e = generics;
  for (auto &t : e)
    t.type = t.type ? t.type->generalize(atLevel) : nullptr;
  auto c = std::make_shared<StaticType>(e, expr, typeCtx);
  c->setSrcInfo(getSrcInfo());
  return c;
}
TypePtr StaticType::instantiate(int atLevel, int *unboundCount,
                                std::unordered_map<int, TypePtr> *cache) {
  auto e = generics;
  for (auto &t : e)
    t.type = t.type ? t.type->instantiate(atLevel, unboundCount, cache) : nullptr;
  auto c = std::make_shared<StaticType>(e, expr, typeCtx);
  c->setSrcInfo(getSrcInfo());
  return c;
}
std::vector<TypePtr> StaticType::getUnbounds() const {
  std::vector<TypePtr> u;
  for (auto &t : generics)
    if (t.type) {
      auto tu = t.type->getUnbounds();
      u.insert(u.begin(), tu.begin(), tu.end());
    }
  return u;
}
bool StaticType::canRealize() const {
  if (!expr->staticValue.evaluated)
    for (auto &t : generics)
      if (t.type && !t.type->canRealize())
        return false;
  return true;
}
bool StaticType::isInstantiated() const { return expr->staticValue.evaluated; }
std::string StaticType::debugString(char mode) const {
  if (expr->staticValue.evaluated)
    return expr->staticValue.toString();
  if (mode == 2) {
    std::vector<std::string> s;
    for (auto &g : generics)
      s.push_back(g.type->debugString(mode));
    return fmt::format("Static[{};{}]", join(s, ","), expr->toString());
  } else {
    return fmt::format("Static[{}]", FormatVisitor::apply(expr));
  }
}
std::string StaticType::realizedName() const {
  seqassert(canRealize(), "cannot realize {}", toString());
  std::vector<std::string> deps;
  for (auto &e : generics)
    deps.push_back(e.type->realizedName());
  if (!expr->staticValue.evaluated) // If not already evaluated, evaluate!
    const_cast<StaticType *>(this)->expr->staticValue = evaluate();
  seqassert(expr->staticValue.evaluated, "static value not evaluated");
  return expr->staticValue.toString();
}
StaticValue StaticType::evaluate() const {
  if (expr->staticValue.evaluated)
    return expr->staticValue;
  typeCtx->addBlock();
  for (auto &g : generics)
    typeCtx->add(TypecheckItem::Type, g.name, g.type);
  auto en = TypecheckVisitor(typeCtx).transform(expr->clone());
  seqassert(en->isStatic() && en->staticValue.evaluated, "{} cannot be evaluated",
            en->toString());
  typeCtx->popBlock();
  return en->staticValue;
}
void StaticType::parseExpr(const ExprPtr &e, std::unordered_set<std::string> &seen) {
  e->type = nullptr;
  if (auto ei = e->getId()) {
    if (!in(seen, ei->value)) {
      auto val = typeCtx->find(ei->value);
      seqassert(val && val->type->isStaticType(), "invalid static expression");
      auto genTyp = val->type->follow();
      auto id = genTyp->getLink() ? genTyp->getLink()->id
                : genTyp->getStatic()->generics.empty()
                    ? 0
                    : genTyp->getStatic()->generics[0].id;
      generics.emplace_back(ClassType::Generic(
          ei->value, typeCtx->cache->reverseIdentifierLookup[ei->value], genTyp, id));
      seen.insert(ei->value);
    }
  } else if (auto eu = e->getUnary()) {
    parseExpr(eu->expr, seen);
  } else if (auto eb = e->getBinary()) {
    parseExpr(eb->lexpr, seen);
    parseExpr(eb->rexpr, seen);
  } else if (auto ef = e->getIf()) {
    parseExpr(ef->cond, seen);
    parseExpr(ef->ifexpr, seen);
    parseExpr(ef->elsexpr, seen);
  }
}

CallableTrait::CallableTrait(std::vector<TypePtr> args) : args(move(args)) {}
int CallableTrait::unify(Type *typ, Unification *us) {
  if (auto tr = typ->getRecord()) {
    if (tr->name == "NoneType")
      return 1;
    if (args.empty())
      return (tr->name == "Function" || tr->getPartial()) ? 1 : -1;
    auto &inargs = args[0]->getRecord()->args;
    if (tr->name == "Function") {
      // Handle Callable[...]<->Function[...].
      if (args.size() != tr->generics.size())
        return -1;
      if (inargs.size() != tr->generics[0].type->getRecord()->args.size())
        return -1;
      for (int i = 0; i < inargs.size(); i++) {
        if (inargs[i]->unify(tr->generics[0].type->getRecord()->args[i].get(), us) ==
            -1)
          return -1;
      }
      return 1;
    } else if (auto pt = tr->getPartial()) {
      // Handle Callable[...]<->Partial[...].
      std::vector<int> zeros;
      for (int pi = 9; pi < tr->name.size() && tr->name[pi] != '.'; pi++)
        if (tr->name[pi] == '0')
          zeros.emplace_back(pi - 9);
      if (zeros.size() != inargs.size())
        return -1;

      int ic = 0;
      std::unordered_map<int, TypePtr> c;
      auto pf = pt->func->instantiate(0, &ic, &c)->getFunc();
      // For partial functions, we just check can we unify without actually performing
      // unification
      for (int pi = 0, gi = 0; pi < pt->known.size(); pi++)
        if (!pt->known[pi] && pf->ast->args[pi].status == Param::Normal)
          if (inargs[gi++]->unify(pf->getArgTypes()[pi].get(), us) == -1)
            return -1;
      if (us && us->realizator && pf->canRealize()) {
        // Realize if possible to allow deduction of return type [and possible
        // unification!]
        auto rf = us->realizator->realize(pf);
        pf->unify(rf.get(), us);
      }
      if (args[1]->unify(pf->getRetType().get(), us) == -1)
        return -1;
      return 1;
    }
  } else if (auto tl = typ->getLink()) {
    if (tl->kind == LinkType::Link)
      return unify(tl->type.get(), us);
    if (tl->kind == LinkType::Unbound) {
      if (tl->trait) {
        auto tt = dynamic_cast<CallableTrait *>(tl->trait.get());
        if (!tt || tt->args.size() != args.size())
          return -1;
        for (int i = 0; i < args.size(); i++)
          if (args[i]->unify(tt->args[i].get(), us) == -1)
            return -1;
      }
      return 1;
    }
  }
  return -1;
}
TypePtr CallableTrait::generalize(int atLevel) {
  auto g = args;
  for (auto &t : g)
    t = t ? t->generalize(atLevel) : nullptr;
  auto c = std::make_shared<CallableTrait>(g);
  c->setSrcInfo(getSrcInfo());
  return c;
}
TypePtr CallableTrait::instantiate(int atLevel, int *unboundCount,
                                   std::unordered_map<int, TypePtr> *cache) {
  auto g = args;
  for (auto &t : g)
    t = t ? t->instantiate(atLevel, unboundCount, cache) : nullptr;
  auto c = std::make_shared<CallableTrait>(g);
  c->setSrcInfo(getSrcInfo());
  return c;
}
std::string CallableTrait::debugString(char mode) const {
  std::vector<std::string> gs;
  for (auto &a : args)
    gs.push_back(a->debugString(mode));
  return fmt::format("Callable[{}]", join(gs, ","));
}

TypeTrait::TypeTrait(TypePtr typ) : type(std::move(typ)) {}
int TypeTrait::unify(Type *typ, Unification *us) { return typ->unify(type.get(), us); }
TypePtr TypeTrait::generalize(int atLevel) {
  auto c = std::make_shared<TypeTrait>(type->generalize(atLevel));
  c->setSrcInfo(getSrcInfo());
  return c;
}
TypePtr TypeTrait::instantiate(int atLevel, int *unboundCount,
                               std::unordered_map<int, TypePtr> *cache) {
  auto c = std::make_shared<TypeTrait>(type->instantiate(atLevel, unboundCount, cache));
  c->setSrcInfo(getSrcInfo());
  return c;
}
std::string TypeTrait::debugString(char mode) const {
  return fmt::format("Trait[{}]", type->debugString(mode));
}

UnionType::UnionType() : RecordType("Union", "Union") {}
UnionType::UnionType(const std::vector<ClassType::Generic> &generics, bool sealed)
    : RecordType("Union", "Union", generics), sealed(sealed) {}
int UnionType::unify(Type *typ, Unification *us) {
  LOG("--> {} {}", debugString(2), typ->debugString(2));
  if (sealed && typ->getUnion()) {
    auto tr = typ->getUnion();
    // Do not hard-unify if we have unbounds
    if (!canRealize() || !tr->canRealize())
      return 0;

    std::map<std::string, TypePtr> u1, u2;
    for (auto &t : generics) {
      auto key = t.type->realizedName();
      if (auto i = in(u1, key)) {
        if (t.type->unify(i->get(), us) == -1)
          return -1;
      } else {
        u1[key] = t.type;
      }
    }
    for (auto &t : tr->generics) {
      auto key = t.type->realizedName();
      if (auto i = in(u2, key)) {
        if (t.type->unify(i->get(), us) == -1)
          return -1;
      } else {
        u2[key] = t.type;
      }
    }

    if (u1.size() != u2.size())
      return -1;
    int s1 = 2, s = 0;
    for (auto i = u1.begin(), j = u2.begin(); i != u1.end(); i++, j++) {
      if ((s = i->second->unify(j->second.get(), us)) == -1)
        return -1;
      s1 += s;
    }
    return s1;
  } else if (!sealed) {
    if (auto tu = typ->getUnion()) {
      int sc = 0;
      for (auto &t : tu->generics)
        sc += unify(t.type.get(), us);
      return sc;
    } else {
      // Add new type
      for (size_t i = generics.size(); i-- > 0;) {
        if (generics[i].type->unify(typ, nullptr) >= 0)
          return generics[i].type->unify(typ, us);
      }
      generics.emplace_back(
          Generic("u", "u", typ->shared_from_this(), 100 + generics.size()));
      return 1;
    }
  } else if (auto tl = typ->getLink()) {
    return tl->unify(this, us);
  }
  return -1;
}
TypePtr UnionType::generalize(int atLevel) {
  auto c = RecordType::generalize(atLevel);
  return std::make_shared<UnionType>(c->getClass()->generics, sealed);
}
TypePtr UnionType::instantiate(int atLevel, int *unboundCount,
                               std::unordered_map<int, TypePtr> *cache) {
  auto c = RecordType::instantiate(atLevel, unboundCount, cache);
  return std::make_shared<UnionType>(c->getClass()->generics, sealed);
}
std::string UnionType::debugString(char mode) const {
  return this->RecordType::debugString(mode);
}
std::string UnionType::realizedName() const {
  seqassert(canRealize(), "cannot realize {}", toString());
  std::set<std::string> gss;
  for (auto &a : generics)
    gss.insert(a.type->realizedName());
  std::string s;
  for (auto &i : gss)
    s += "," + i;
  return fmt::format("{}{}", name, s.empty() ? "" : fmt::format("[{}]", s.substr(1)));
}

void UnionType::seal() { sealed = true; }
std::vector<types::TypePtr> UnionType::getRealizationTypes() {
  seqassert(canRealize(), "cannot realize {}", debugString(1));
  std::map<std::string, types::TypePtr> unionTypes;
  for (auto &u : generics)
    unionTypes[u.type->realizedName()] = u.type;
  std::vector<types::TypePtr> r;
  r.reserve(unionTypes.size());
  for (auto &[_, t] : unionTypes)
    r.emplace_back(t);
  return r;
}

} // namespace codon::ast::types
