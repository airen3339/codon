#include "escape.h"

#include <algorithm>
#include <iterator>
#include <unordered_map>
#include <utility>

#include "codon/sir/util/irtools.h"

namespace codon {
namespace ir {
namespace util {
namespace {

template <typename T> bool shouldTrack(T *x) {
  // We only care about things with pointers,
  // since you can't capture primitive types
  // like int, float, etc.
  return x && !x->getType()->isAtomic();
}

enum DerivedKind {
  ORIGIN = 0,
  ASSIGN,
  MEMBER,
  INSERT,
  CALL,
  REFERENCE,
};

struct CaptureInfo {
  std::vector<unsigned> argCaptures; // what other arguments capture this?
  bool returnCaptures = false;       // does return capture this?
  bool externCaptures = false;       // is this externally captured by a global?

  operator bool() const {
    return !argCaptures.empty() || returnCaptures || externCaptures;
  }

  static CaptureInfo nothing() { return {}; }

  static CaptureInfo unknown(Func *func) {
    CaptureInfo c;
    unsigned i = 0;
    for (auto it = func->arg_begin(); it != func->arg_end(); ++it) {
      if (shouldTrack(*it))
        c.argCaptures.push_back(i);
      ++i;
    }
    c.returnCaptures = true;
    c.externCaptures = true;
    return c;
  }
};

struct DerivedValInfo {
  struct Element {
    DerivedKind kind;
    Var *var;
  };

  std::vector<Element> info;
};

struct DerivedVarInfo {
  struct Element {
    DerivedKind kind;
    Value *val;
  };

  std::vector<Element> info;
};

struct DerivedSet {
  unsigned argno;
  std::unordered_map<id_t, DerivedValInfo> derivedVals;
  std::unordered_map<id_t, DerivedVarInfo> derivedVars;
  CaptureInfo result;

  bool isDerived(Var *v) const {
    return derivedVars.find(v->getId()) != derivedVars.end();
  }

  bool isDerived(Value *v) const {
    return derivedVals.find(v->getId()) != derivedVals.end();
  }

  void setDerived(Var *v, DerivedKind kind, Value *cause) {
    if (v->isGlobal())
      result.externCaptures = true;
    // TODO: args

    auto it = derivedVars.find(v->getId());
    if (it == derivedVars.end()) {
      DerivedVarInfo info = {{{kind, cause}}};
      derivedVars.emplace(v->getId(), info);
    } else {
      it->second.info.push_back({kind, cause});
    }
  }

  void setDerived(Value *v, DerivedKind kind, Var *cause = nullptr) {
    auto it = derivedVals.find(v->getId());
    if (it == derivedVals.end()) {
      DerivedValInfo info = {{{kind, cause}}};
      derivedVals.emplace(v->getId(), info);
    } else {
      it->second.info.push_back({kind, cause});
    }
  }

  unsigned size() const {
    unsigned total = 0;
    for (auto &e : derivedVals) {
      total += e.second.info.size();
    }
    for (auto &e : derivedVars) {
      total += e.second.info.size();
    }
    return total;
  }

  explicit DerivedSet(unsigned argno)
      : argno(argno), derivedVals(), derivedVars(), result() {}

  DerivedSet(unsigned argno, Var *var, Value *cause) : DerivedSet(argno) {
    setDerived(var, DerivedKind::ORIGIN, cause);
  }
};

struct CaptureContext {
  std::unordered_map<id_t, std::vector<CaptureInfo>> results;

  std::vector<CaptureInfo> get(Func *func) {
    // TODO: handle other func types: internal/external/llvm
    // TODO: handle generators
    auto it = results.find(func->getId());
    if (it == results.end()) {
      std::vector<CaptureInfo> result;
      for (auto arg = func->arg_begin(); arg != func->arg_end(); ++arg) {
        result.push_back(shouldTrack(*arg) ? CaptureInfo::unknown(func)
                                           : CaptureInfo::nothing());
      }
      return result;
    } else {
      return it->second;
    }
  }

  void set(Func *func, const std::vector<CaptureInfo> &result) {
    results[func->getId()] = result;
  }
};

// This visitor answers the questions of what vars are
// releavant to track in a capturing expression. For
// example, in "a[i] = x", the expression "a[i]" captures
// "x"; in this case we need to track "a" but the variable
// "i" (typically) we would not care about.
struct ExtractVars : public util::Visitor {
  CaptureContext &cc;
  std::unordered_set<id_t> vars;
  bool escapes;

  explicit ExtractVars(CaptureContext &cc)
      : util::Visitor(), cc(cc), vars(), escapes(false) {}

  template <typename Node> void process(Node *v) { v->accept(*this); }

  void add(Var *v) {
    if (shouldTrack(v))
      vars.insert(v->getId());
  }

  void defaultVisit(Node *) override {}

  void visit(VarValue *v) override { add(v->getVar()); }

  void visit(PointerValue *v) override { add(v->getVar()); }

  void visit(CallInstr *v) override {
    auto capInfo = cc.get(util::getFunc(v->getCallee()));
    unsigned i = 0;
    for (auto *arg : *v) {
      if (shouldTrack(arg) && capInfo[i].returnCaptures)
        process(arg);
      ++i;
    }
  }

  void visit(YieldInInstr *v) override {
    // We have no idea what the yield-in
    // value could be, so just assume we
    // escape in this case.
    escapes = true;
  }

  void visit(TernaryInstr *v) override {
    process(v->getTrueValue());
    process(v->getFalseValue());
  }

  void visit(ExtractInstr *v) override { process(v->getVal()); }

  void visit(FlowInstr *v) override { process(v->getValue()); }

  void visit(dsl::CustomInstr *v) override {
    // TODO
  }
};

bool extractVars(CaptureContext &cc, Value *v, std::vector<Var *> &result) {
  auto *M = v->getModule();
  ExtractVars ev(cc);
  v->accept(ev);
  for (auto id : ev.vars) {
    result.push_back(M->getVar(id));
  }
  return ev.escapes;
}

struct DerivedFinder : public Operator {
  CaptureContext &cc;
  BodiedFunc *func;
  analyze::dataflow::CFGraph *cfg;
  analyze::dataflow::RDInspector *rd;
  std::vector<DerivedSet> dsets;

  DerivedFinder(CaptureContext &cc, BodiedFunc *func, analyze::dataflow::CFGraph *cfg,
                analyze::dataflow::RDInspector *rd)
      : Operator(), cc(cc), func(func), cfg(cfg), rd(rd), dsets() {
    using analyze::dataflow::SyntheticAssignInstr;

    // find synthetic assignments in CFG for argument vars
    auto *entry = cfg->getEntryBlock();
    std::unordered_map<id_t, SyntheticAssignInstr *> synthAssigns;

    for (auto *v : *entry) {
      if (auto *synth = cast<SyntheticAssignInstr>(v)) {
        if (shouldTrack(synth->getLhs()))
          synthAssigns[synth->getLhs()->getId()] =
              const_cast<SyntheticAssignInstr *>(synth);
      }
    }

    // make a derived set for each function argument
    unsigned argno = 0;
    for (auto it = func->arg_begin(); it != func->arg_end(); ++it) {
      if (!shouldTrack(*it))
        continue;

      auto it2 = synthAssigns.find((*it)->getId());
      seqassert(it2 != synthAssigns.end(),
                "could not find synthetic assignment for arg var");
      dsets.push_back(DerivedSet(argno++, *it, it2->second));
    }
  }

  unsigned size() const {
    unsigned total = 0;
    for (auto &dset : dsets) {
      total += dset.size();
    }
    return total;
  }

  DerivedSet *setFor(Value *v) {
    for (auto &dset : dsets) {
      if (dset.isDerived(v))
        return &dset;
    }
    return nullptr;
  }

  void forEachDSetOf(Value *v, std::function<void(DerivedSet &)> func) {
    for (auto &dset : dsets) {
      if (dset.isDerived(v))
        func(dset);
    }
  }

  void forEachDSetOf(Var *v, std::function<void(DerivedSet &)> func) {
    for (auto &dset : dsets) {
      if (dset.isDerived(v))
        func(dset);
    }
  }

  void handleVarReference(Value *v, Var *var) {
    forEachDSetOf(var, [&](DerivedSet &dset) {
      // Make sure the var at this point is reached by
      // at least one definition that has led to a
      // derived value.
      auto mySet = rd->getReachingDefinitions(var, v);
      auto it = dset.derivedVars.find(var->getId());
      if (it == dset.derivedVars.end())
        return;

      for (auto &e : it->second.info) {
        auto otherSet = rd->getReachingDefinitions(var, e.val);
        bool derived = false;
        for (auto &elem : mySet) {
          if (otherSet.count(elem)) {
            derived = true;
            break;
          }
        }
        if (derived) {
          dset.setDerived(v, DerivedKind::REFERENCE, var);
          return;
        }
      }
    });
  }

  void handle(VarValue *v) override { handleVarReference(v, v->getVar()); }

  void handle(PointerValue *v) override { handleVarReference(v, v->getVar()); }

  void handle(AssignInstr *v) override {
    forEachDSetOf(v->getRhs(), [&](DerivedSet &dset) {
      dset.setDerived(v->getLhs(), DerivedKind::ASSIGN, v);
    });
  }

  void handle(ExtractInstr *v) override {
    if (!shouldTrack(v))
      return;

    forEachDSetOf(v->getVal(),
                  [&](DerivedSet &dset) { dset.setDerived(v, DerivedKind::MEMBER); });
  }

  void handle(InsertInstr *v) override {
    std::vector<Var *> vars;
    bool escapes = extractVars(cc, v->getLhs(), vars);

    forEachDSetOf(v->getRhs(), [&](DerivedSet &dset) {
      if (escapes)
        dset.result.externCaptures = true;

      for (auto *var : vars) {
        dset.setDerived(var, DerivedKind::INSERT, v);
      }
    });
  }

  void handle(CallInstr *v) override {
    std::vector<Value *> args(v->begin(), v->end());
    std::vector<CaptureInfo> capInfo;

    if (auto *func = util::getFunc(v->getCallee())) {
      capInfo = cc.get(func);
    } else {
      std::vector<unsigned> argCaptures;
      unsigned i = 0;
      for (auto *arg : args) {
        if (shouldTrack(arg))
          argCaptures.push_back(i);
        ++i;
      }

      for (auto *arg : args) {
        CaptureInfo info = CaptureInfo::nothing();
        if (shouldTrack(arg)) {
          info.argCaptures = argCaptures;
          info.returnCaptures = true;
          info.externCaptures = true;
        }
        capInfo.push_back(info);
      }
    }

    unsigned i = 0;
    for (auto *arg : args) {
      forEachDSetOf(arg, [&](DerivedSet &dset) {
        auto &info = capInfo[i];

        // Process all other arguments that capture us.
        for (auto argno : info.argCaptures) {
          Value *arg = args[argno];
          std::vector<Var *> vars;
          bool escapes = extractVars(cc, arg, vars);
          if (escapes)
            dset.result.externCaptures = true;

          for (auto *var : vars) {
            dset.setDerived(var, DerivedKind::CALL, v);
          }
        }

        // Check if the return value captures.
        if (info.returnCaptures)
          dset.setDerived(v, DerivedKind::CALL);

        // Check if we're externally captured.
        if (info.externCaptures)
          dset.result.externCaptures = true;
      });
      ++i;
    }
  }

  void handle(dsl::CustomInstr *v) override {
    // TODO
  }

  void handle(ReturnInstr *v) override {
    if (!v->getValue())
      return;

    forEachDSetOf(v->getValue(),
                  [&](DerivedSet &dset) { dset.result.returnCaptures = true; });
  }

  void handle(YieldInstr *v) override {
    if (!v->getValue())
      return;

    forEachDSetOf(v->getValue(),
                  [&](DerivedSet &dset) { dset.result.returnCaptures = true; });
  }

  void handle(ThrowInstr *v) override {
    if (!v->getValue())
      return;

    forEachDSetOf(v->getValue(),
                  [&](DerivedSet &dset) { dset.result.externCaptures = true; });
  }
};

} // namespace

EscapeResult escapes(BodiedFunc *parent, Value *value,
                     analyze::dataflow::RDResult *reaching) {
  auto it1 = reaching->cfgResult->graphs.find(parent->getId());
  seqassert(it1 != reaching->cfgResult->graphs.end(),
            "could not find parent function in CFG results");

  auto it2 = reaching->results.find(parent->getId());
  seqassert(it2 != reaching->results.end(),
            "could not find parent function in reaching-definitions results");

  CaptureContext cc;
  DerivedFinder df(cc, parent, it1->second.get(), it2->second.get());
  unsigned oldSize = 0;
  do {
    oldSize = df.size();
    parent->accept(df);
  } while (df.size() != oldSize);

  return EscapeResult::YES;
}

} // namespace util
} // namespace ir
} // namespace codon
