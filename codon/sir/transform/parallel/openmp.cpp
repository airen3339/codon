#include "openmp.h"

#include <algorithm>
#include <iterator>
#include <limits>

#include "codon/sir/transform/parallel/schedule.h"
#include "codon/sir/util/cloning.h"
#include "codon/sir/util/irtools.h"
#include "codon/sir/util/outlining.h"

namespace codon {
namespace ir {
namespace transform {
namespace parallel {
namespace {
const std::string ompModule = "std.openmp";
const std::string builtinModule = "std.internal.builtin";

struct OMPTypes {
  types::Type *i32 = nullptr;
  types::Type *i8ptr = nullptr;
  types::Type *i32ptr = nullptr;
  types::Type *micro = nullptr;
  types::Type *routine = nullptr;
  types::Type *ident = nullptr;
  types::Type *task = nullptr;

  explicit OMPTypes(Module *M) {
    i32 = M->getIntNType(32, /*sign=*/true);
    i8ptr = M->getPointerType(M->getByteType());
    i32ptr = M->getPointerType(i32);
    micro = M->getFuncType(M->getVoidType(), {i32ptr, i32ptr});
    routine = M->getFuncType(i32, {i32ptr, i8ptr});
    ident = M->getOrRealizeType("Ident", {}, ompModule);
    task = M->getOrRealizeType("Task", {}, ompModule);
    seqassert(ident, "openmp.Ident type not found");
    seqassert(task, "openmp.Task type not found");
  }
};

Var *getVarFromOutlinedArg(Value *arg) {
  if (auto *val = cast<VarValue>(arg)) {
    return val->getVar();
  } else if (auto *val = cast<PointerValue>(arg)) {
    return val->getVar();
  } else {
    seqassert(false, "unknown outline var");
  }
  return nullptr;
}

Value *ptrFromFunc(Func *func) {
  auto *M = func->getModule();
  auto *funcType = func->getType();
  auto *rawMethod = M->getOrRealizeMethod(funcType, "__raw__", {funcType});
  seqassert(rawMethod, "cannot find function __raw__ method");
  return util::call(rawMethod, {M->Nr<VarValue>(func)});
}

// we create the locks lazily to avoid them when they're not needed
struct ReductionLocks {
  Var *mainLock =
      nullptr; // lock used in calls to _reduce_no_wait and _end_reduce_no_wait
  Var *critLock = nullptr; // lock used in reduction critical sections

  Var *createLock(Module *M) {
    auto *main = cast<BodiedFunc>(M->getMainFunc());
    auto *lck = util::alloc(M->getByteType(), 32);
    auto *val = util::makeVar(lck, cast<SeriesFlow>(main->getBody()),
                              /*parent=*/nullptr, /*prepend=*/true);
    return val->getVar();
  }

  Var *getMainLock(Module *M) {
    if (!mainLock)
      mainLock = createLock(M);
    return mainLock;
  }

  Var *getCritLock(Module *M) {
    if (!critLock)
      critLock = createLock(M);
    return critLock;
  }
};

struct Reduction {
  enum Kind {
    NONE,
    ADD,
    MUL,
    AND,
    OR,
    XOR,
    MIN,
    MAX,
  };

  Kind kind = Kind::NONE;
  Var *shared = nullptr;

  types::Type *getType() {
    auto *ptrType = cast<types::PointerType>(shared->getType());
    seqassert(ptrType, "expected shared var to be of pointer type");
    return ptrType->getBase();
  }

  Value *getInitial() {
    if (!*this)
      return nullptr;
    auto *M = shared->getModule();
    auto *type = getType();

    if (isA<types::IntType>(type)) {
      switch (kind) {
      case Kind::ADD:
        return M->getInt(0);
      case Kind::MUL:
        return M->getInt(1);
      case Kind::AND:
        return M->getInt(~0);
      case Kind::OR:
        return M->getInt(0);
      case Kind::XOR:
        return M->getInt(0);
      case Kind::MIN:
        return M->getInt(std::numeric_limits<int64_t>::max());
      case Kind::MAX:
        return M->getInt(std::numeric_limits<int64_t>::min());
      default:
        return nullptr;
      }
    } else if (isA<types::FloatType>(type)) {
      switch (kind) {
      case Kind::ADD:
        return M->getFloat(0.);
      case Kind::MUL:
        return M->getFloat(1.);
      case Kind::MIN:
        return M->getFloat(std::numeric_limits<double>::max());
      case Kind::MAX:
        return M->getFloat(std::numeric_limits<double>::min());
      default:
        return nullptr;
      }
    }

    auto *init = (*type)();
    if (!init || !init->getType()->is(type))
      return nullptr;
    return init;
  }

  Value *generateNonAtomicReduction(Value *ptr, Value *arg) {
    auto *M = ptr->getModule();
    Value *lhs = util::ptrLoad(ptr);
    Value *result = nullptr;
    switch (kind) {
    case Kind::ADD:
      result = *lhs + *arg;
      break;
    case Kind::MUL:
      result = *lhs * *arg;
      break;
    case Kind::AND:
      result = *lhs & *arg;
      break;
    case Kind::OR:
      result = *lhs | *arg;
      break;
    case Kind::XOR:
      result = *lhs ^ *arg;
      break;
    case Kind::MIN: {
      auto *tup = util::makeTuple({lhs, arg});
      auto *fn = M->getOrRealizeFunc("min", {tup->getType()}, {}, builtinModule);
      seqassert(fn, "min function not found");
      result = util::call(fn, {tup});
      break;
    }
    case Kind::MAX: {
      auto *tup = util::makeTuple({lhs, arg});
      auto *fn = M->getOrRealizeFunc("max", {tup->getType()}, {}, builtinModule);
      seqassert(fn, "max function not found");
      result = util::call(fn, {tup});
      break;
    }
    default:
      return nullptr;
    }
    return util::ptrStore(ptr, result);
  }

  Value *generateAtomicReduction(Value *ptr, Value *arg, Var *loc, Var *gtid,
                                 ReductionLocks &locks) {
    auto *M = ptr->getModule();
    auto *type = getType();
    std::string func = "";

    if (isA<types::IntType>(type)) {
      switch (kind) {
      case Kind::ADD:
        func = "_atomic_int_add";
        break;
      case Kind::MUL:
        func = "_atomic_int_mul";
        break;
      case Kind::AND:
        func = "_atomic_int_and";
        break;
      case Kind::OR:
        func = "_atomic_int_or";
        break;
      case Kind::XOR:
        func = "_atomic_int_xor";
        break;
      case Kind::MIN:
        func = "_atomic_int_min";
        break;
      case Kind::MAX:
        func = "_atomic_int_max";
        break;
      default:
        break;
      }
    } else if (isA<types::FloatType>(type)) {
      switch (kind) {
      case Kind::ADD:
        func = "_atomic_float_add";
        break;
      case Kind::MUL:
        func = "_atomic_float_mul";
        break;
      case Kind::MIN:
        func = "_atomic_float_min";
        break;
      case Kind::MAX:
        func = "_atomic_float_max";
        break;
      default:
        break;
      }
    }

    if (!func.empty()) {
      auto *atomicOp =
          M->getOrRealizeFunc(func, {ptr->getType(), arg->getType()}, {}, ompModule);
      seqassert(atomicOp, "atomic op '{}' not found", func);
      return util::call(atomicOp, {ptr, arg});
    }

    switch (kind) {
    case Kind::ADD:
      func = "__atomic_add__";
      break;
    case Kind::MUL:
      func = "__atomic_mul__";
      break;
    case Kind::AND:
      func = "__atomic_and__";
      break;
    case Kind::OR:
      func = "__atomic_or__";
      break;
    case Kind::XOR:
      func = "__atomic_xor__";
      break;
    case Kind::MIN:
      func = "__atomic_min__";
      break;
    case Kind::MAX:
      func = "__atomic_max__";
      break;
    default:
      break;
    }

    if (!func.empty()) {
      auto *atomicOp =
          M->getOrRealizeMethod(arg->getType(), func, {ptr->getType(), arg->getType()});
      if (atomicOp)
        return util::call(atomicOp, {ptr, arg});
    }

    seqassert(loc && gtid, "loc and/or gtid are null");
    auto *lck = locks.getCritLock(M);
    auto *critBegin = M->getOrRealizeFunc(
        "_critical_begin", {loc->getType(), gtid->getType(), lck->getType()}, {},
        ompModule);
    seqassert(critBegin, "critical begin function not found");
    auto *critEnd = M->getOrRealizeFunc(
        "_critical_end", {loc->getType(), gtid->getType(), lck->getType()}, {},
        ompModule);
    seqassert(critEnd, "critical end function not found");

    auto *critEnter = util::call(
        critBegin, {M->Nr<VarValue>(loc), M->Nr<VarValue>(gtid), M->Nr<VarValue>(lck)});
    auto *operation = generateNonAtomicReduction(ptr, arg);
    auto *critExit = util::call(
        critEnd, {M->Nr<VarValue>(loc), M->Nr<VarValue>(gtid), M->Nr<VarValue>(lck)});
    // make sure the unlock is in a finally-block
    return util::series(critEnter, M->Nr<TryCatchFlow>(util::series(operation),
                                                       util::series(critExit)));
  }

  operator bool() const { return kind != Kind::NONE; }
};

struct ReductionFunction {
  std::string name;
  Reduction::Kind kind;
  bool method;
};

struct ReductionIdentifier : public util::Operator {
  std::vector<Var *> shareds;
  std::unordered_map<id_t, Reduction> reductions;

  explicit ReductionIdentifier(std::vector<Var *> shareds)
      : util::Operator(), shareds(std::move(shareds)), reductions() {}

  bool isShared(Var *shared) {
    for (auto *v : shareds) {
      if (shared->getId() == v->getId())
        return true;
    }
    return false;
  }

  bool isSharedDeref(Var *shared, Value *v) {
    auto *M = v->getModule();
    auto *ptrType = cast<types::PointerType>(shared->getType());
    seqassert(ptrType, "expected shared var to be of pointer type");
    auto *type = ptrType->getBase();

    if (util::isCallOf(v, Module::GETITEM_MAGIC_NAME, {ptrType, M->getIntType()}, type,
                       /*method=*/true)) {
      auto *call = cast<CallInstr>(v);
      auto *var = util::getVar(call->front());
      return util::isConst<int64_t>(call->back(), 0) && var &&
             var->getId() == shared->getId();
    }

    return false;
  }

  Reduction getReductionFromCall(CallInstr *v) {
    auto *M = v->getModule();
    auto *func = util::getFunc(v->getCallee());
    if (v->numArgs() != 3 || !func ||
        func->getUnmangledName() != Module::SETITEM_MAGIC_NAME)
      return {};

    std::vector<Value *> args(v->begin(), v->end());
    Value *self = args[0];
    Value *idx = args[1];
    Value *item = args[2];

    Var *shared = util::getVar(self);
    if (!shared || !isShared(shared) || !util::isConst<int64_t>(idx, 0))
      return {};

    auto *ptrType = cast<types::PointerType>(shared->getType());
    seqassert(ptrType, "expected shared var to be of pointer type");
    auto *type = ptrType->getBase();

    // double-check the call
    if (!util::isCallOf(v, Module::SETITEM_MAGIC_NAME,
                        {self->getType(), idx->getType(), item->getType()},
                        M->getVoidType(), /*method=*/true))
      return {};

    const std::vector<ReductionFunction> reductionFunctions = {
        {Module::ADD_MAGIC_NAME, Reduction::Kind::ADD, true},
        {Module::MUL_MAGIC_NAME, Reduction::Kind::MUL, true},
        {Module::AND_MAGIC_NAME, Reduction::Kind::AND, true},
        {Module::OR_MAGIC_NAME, Reduction::Kind::OR, true},
        {Module::XOR_MAGIC_NAME, Reduction::Kind::XOR, true},
        {"min", Reduction::Kind::MIN, false},
        {"max", Reduction::Kind::MAX, false},
    };

    for (auto &rf : reductionFunctions) {
      if (rf.method) {
        if (!util::isCallOf(item, rf.name, {type, type}, type, /*method=*/true))
          continue;
      } else {
        if (!util::isCallOf(item, rf.name, {M->getTupleType({type, type})}, type,
                            /*method=*/false))
          continue;
      }

      auto *callRHS = cast<CallInstr>(item);
      if (!rf.method) {
        callRHS = cast<CallInstr>(callRHS->front()); // this will be Tuple.__new__
        if (!callRHS || callRHS->numArgs() != 2)
          continue;
      }

      auto *arg1 = callRHS->front();
      auto *arg2 = callRHS->back();
      Value *deref = nullptr;
      Value *other = nullptr;

      if (isSharedDeref(shared, arg1)) {
        deref = arg1;
        other = arg2;
      } else if (isSharedDeref(shared, arg2)) {
        deref = arg2;
        other = arg1;
      }

      if (!deref)
        return {};

      Reduction reduction = {rf.kind, shared};
      if (!reduction.getInitial())
        return {};

      return reduction;
    }

    return {};
  }

  Reduction getReduction(Var *shared) {
    auto it = reductions.find(shared->getId());
    return (it != reductions.end()) ? it->second : Reduction();
  }

  void handle(CallInstr *v) override {
    if (auto reduction = getReductionFromCall(v)) {
      auto it = reductions.find(reduction.shared->getId());
      // if we've seen the var before, make sure it's consistent
      // otherwise mark as invalid via an empty reduction
      if (it == reductions.end()) {
        reductions.emplace(reduction.shared->getId(), reduction);
      } else if (it->second && it->second.kind != reduction.kind) {
        it->second = {};
      }
    }
  }
};

struct SharedInfo {
  unsigned memb;       // member index in template's `extra` arg
  Var *local;          // the local var we create to store current value
  Reduction reduction; // the reduction we're performing, or empty if none
};

struct ImperativeLoopTemplateReplacer : public util::Operator {
  BodiedFunc *parent;
  CallInstr *replacement;
  Var *loopVar;
  OMPSched *sched;
  ReductionIdentifier *reds;
  int64_t step;
  std::vector<SharedInfo> sharedInfo;
  ReductionLocks locks;
  Var *locRef;
  Var *reductionLocRef;
  Var *gtid;

  ImperativeLoopTemplateReplacer(BodiedFunc *parent, CallInstr *replacement,
                                 Var *loopVar, OMPSched *sched,
                                 ReductionIdentifier *reds, int64_t step)
      : util::Operator(), parent(parent), replacement(replacement), loopVar(loopVar),
        sched(sched), reds(reds), step(step), sharedInfo(), locks(), locRef(nullptr),
        reductionLocRef(nullptr), gtid(nullptr) {}

  unsigned numReductions() {
    unsigned num = 0;
    for (auto &info : sharedInfo) {
      if (info.reduction)
        num += 1;
    }
    return num;
  }

  Value *getReductionTuple() {
    auto *M = parent->getModule();
    std::vector<Value *> elements;
    for (auto &info : sharedInfo) {
      if (info.reduction)
        elements.push_back(M->Nr<PointerValue>(info.local));
    }
    return util::makeTuple(elements, M);
  }

  BodiedFunc *makeReductionFunc() {
    auto *M = parent->getModule();
    auto *tupleType = getReductionTuple()->getType();
    auto *argType = M->getPointerType(tupleType);
    auto *funcType = M->getFuncType(M->getVoidType(), {argType, argType});
    auto *reducer = M->Nr<BodiedFunc>("__omp_reducer");
    reducer->realize(funcType, {"lhs", "rhs"});

    auto *lhsVar = reducer->arg_front();
    auto *rhsVar = reducer->arg_back();
    auto *body = M->Nr<SeriesFlow>();
    unsigned next = 0;
    for (auto &info : sharedInfo) {
      if (info.reduction) {
        auto *lhs = util::ptrLoad(M->Nr<VarValue>(lhsVar));
        auto *rhs = util::ptrLoad(M->Nr<VarValue>(rhsVar));
        auto *lhsElem = util::tupleGet(lhs, next);
        auto *rhsElem = util::tupleGet(rhs, next);
        body->push_back(
            info.reduction.generateNonAtomicReduction(lhsElem, util::ptrLoad(rhsElem)));
        ++next;
      }
    }
    reducer->setBody(body);
    return reducer;
  }

  void handle(CallInstr *v) override {
    auto *M = v->getModule();
    auto *func = util::getFunc(v->getCallee());
    if (!func)
      return;
    auto name = func->getUnmangledName();

    if (name == "_loop_step") {
      v->replaceAll(M->getInt(step));
    }

    if (name == "_loop_loc_and_gtid") {
      seqassert(v->numArgs() == 3 &&
                    std::all_of(v->begin(), v->end(),
                                [](auto x) { return isA<VarValue>(x); }),
                "unexpected loop loc and gtid stub");
      std::vector<Value *> args(v->begin(), v->end());
      locRef = util::getVar(args[0]);
      reductionLocRef = util::getVar(args[1]);
      gtid = util::getVar(args[2]);
    }

    if (name == "_loop_body_stub") {
      seqassert(replacement, "unexpected double replacement");
      seqassert(v->numArgs() == 2 && isA<VarValue>(v->front()) &&
                    isA<VarValue>(v->back()),
                "unexpected loop body stub");

      auto *outlinedFunc = util::getFunc(replacement->getCallee());

      // the template passes the new loop var and extra args
      // to the body stub for convenience
      auto *newLoopVar = util::getVar(v->front());
      auto *extras = util::getVar(v->back());

      std::vector<Value *> newArgs;
      auto outlinedArgs = outlinedFunc->arg_begin(); // arg vars of *outlined func*
      unsigned next = 0; // next index in "extra" args tuple, passed to template
      // `arg` is an argument of the original outlined func call
      for (auto *arg : *replacement) {
        if (getVarFromOutlinedArg(arg)->getId() != loopVar->getId()) {
          Value *newArg = nullptr;

          // shared vars will be stored in a new var
          if (isA<PointerValue>(arg)) {
            types::Type *base = cast<types::PointerType>(arg->getType())->getBase();

            // get extras again since we'll be inserting the new var before extras local
            Var *lastArg = parent->arg_back(); // ptr to {chunk, start, stop, extras}
            Value *val = util::tupleGet(util::ptrLoad(M->Nr<VarValue>(lastArg)), 3);
            Value *initVal = util::ptrLoad(util::tupleGet(val, next));

            Reduction reduction = reds->getReduction(*outlinedArgs);
            if (reduction) {
              initVal = reduction.getInitial();
              seqassert(initVal && initVal->getType()->is(base),
                        "unknown reduction init value");
            }

            VarValue *newVar = util::makeVar(
                initVal, cast<SeriesFlow>(parent->getBody()), parent, /*prepend=*/true);
            sharedInfo.push_back({next, newVar->getVar(), reduction});

            newArg = M->Nr<PointerValue>(newVar->getVar());
            ++next;
          } else {
            newArg = util::tupleGet(M->Nr<VarValue>(extras), next++);
          }

          newArgs.push_back(newArg);
        } else {
          if (isA<VarValue>(arg)) {
            newArgs.push_back(M->Nr<VarValue>(newLoopVar));
          } else if (isA<PointerValue>(arg)) {
            newArgs.push_back(M->Nr<PointerValue>(newLoopVar));
          } else {
            seqassert(false, "unknown outline var");
          }
        }

        ++outlinedArgs;
      }

      v->replaceAll(util::call(outlinedFunc, newArgs));
      replacement = nullptr;
    }

    if (name == "_loop_shared_updates") {
      // for all non-reduction shareds, set the final values
      // this will be similar to OpenMP's "lastprivate"
      seqassert(v->numArgs() == 1 && isA<VarValue>(v->front()),
                "unexpected shared updates stub");
      auto *extras = util::getVar(v->front());
      auto *series = M->Nr<SeriesFlow>();

      for (auto &info : sharedInfo) {
        if (info.reduction)
          continue;

        auto *finalValue = M->Nr<VarValue>(info.local);
        auto *val = M->Nr<VarValue>(extras);
        auto *origPtr = util::tupleGet(val, info.memb);
        series->push_back(util::ptrStore(origPtr, finalValue));
      }

      v->replaceAll(series);
    }

    if (name == "_loop_reductions") {
      seqassert(reductionLocRef && gtid, "bad visit order in template");
      seqassert(v->numArgs() == 1 && isA<VarValue>(v->front()),
                "unexpected shared updates stub");
      if (numReductions() == 0)
        return;

      auto *M = parent->getModule();
      auto *extras = util::getVar(v->front());
      auto *reductionTuple = getReductionTuple();
      auto *reducer = makeReductionFunc();
      auto *lck = locks.getMainLock(M);
      auto *rawReducer = ptrFromFunc(reducer);

      auto *reduceNoWait = M->getOrRealizeFunc(
          "_reduce_nowait",
          {reductionLocRef->getType(), gtid->getType(), reductionTuple->getType(),
           rawReducer->getType(), lck->getType()},
          {}, ompModule);
      seqassert(reduceNoWait, "reduce nowait function not found");
      auto *reduceNoWaitEnd = M->getOrRealizeFunc(
          "_end_reduce_nowait",
          {reductionLocRef->getType(), gtid->getType(), lck->getType()}, {}, ompModule);
      seqassert(reduceNoWaitEnd, "end reduce nowait function not found");

      auto *series = M->Nr<SeriesFlow>();
      auto *tupleVal = util::makeVar(reductionTuple, series, parent);
      auto *reduceCode = util::call(reduceNoWait, {M->Nr<VarValue>(reductionLocRef),
                                                   M->Nr<VarValue>(gtid), tupleVal,
                                                   rawReducer, M->Nr<VarValue>(lck)});
      auto *codeVar = util::makeVar(reduceCode, series, parent)->getVar();
      seqassert(codeVar->getType()->is(M->getIntType()), "wrong reduce code type");

      auto *sectionNonAtomic = M->Nr<SeriesFlow>();
      auto *sectionAtomic = M->Nr<SeriesFlow>();

      for (auto &info : sharedInfo) {
        if (info.reduction) {
          Value *ptr = util::tupleGet(M->Nr<VarValue>(extras), info.memb);
          Value *arg = M->Nr<VarValue>(info.local);
          sectionNonAtomic->push_back(
              info.reduction.generateNonAtomicReduction(ptr, arg));
        }
      }
      sectionNonAtomic->push_back(
          util::call(reduceNoWaitEnd, {M->Nr<VarValue>(reductionLocRef),
                                       M->Nr<VarValue>(gtid), M->Nr<VarValue>(lck)}));

      for (auto &info : sharedInfo) {
        if (info.reduction) {
          Value *ptr = util::tupleGet(M->Nr<VarValue>(extras), info.memb);
          Value *arg = M->Nr<VarValue>(info.local);
          sectionAtomic->push_back(
              info.reduction.generateAtomicReduction(ptr, arg, locRef, gtid, locks));
        }
      }

      // make: if code == 1 { sectionNonAtomic } elif code == 2 { sectionAtomic }
      auto *theSwitch = M->Nr<IfFlow>(
          *M->Nr<VarValue>(codeVar) == *M->getInt(1), sectionNonAtomic,
          util::series(M->Nr<IfFlow>(*M->Nr<VarValue>(codeVar) == *M->getInt(2),
                                     sectionAtomic)));
      series->push_back(theSwitch);
      v->replaceAll(series);
    }

    if (name == "_loop_schedule") {
      v->replaceAll(M->getInt(sched->code));
    }

    if (name == "_loop_ordered") {
      v->replaceAll(M->getBool(sched->ordered));
    }
  }
};

struct TaskLoopReductionVarReplacer : public util::Operator {
  std::vector<Var *> reductionArgs;
  std::vector<std::pair<Var *, Var *>> reductionRemap;
  BodiedFunc *parent;

  void setupReductionRemap() {
    auto *M = parent->getModule();

    for (auto *var : reductionArgs) {
      auto *newVar = M->Nr<Var>(var->getType(), /*global=*/false);
      reductionRemap.emplace_back(var, newVar);
    }
  }

  TaskLoopReductionVarReplacer(std::vector<Var *> reductionArgs, BodiedFunc *parent)
      : util::Operator(), reductionArgs(std::move(reductionArgs)), reductionRemap(),
        parent(parent) {
    setupReductionRemap();
  }

  void preHook(Node *v) override {
    for (auto &p : reductionRemap) {
      v->replaceUsedVariable(p.first->getId(), p.second);
    }
  }

  // need to do this as a separate step since otherwise the old variable
  // in the assignment will be replaced, which we don't want
  void finalize() {
    auto *M = parent->getModule();
    auto *body = cast<SeriesFlow>(parent->getBody());
    auto *gtid = parent->arg_back();

    for (auto &p : reductionRemap) {
      auto *taskRedData = M->getOrRealizeFunc(
          "_taskred_data", {M->getIntType(), p.first->getType()}, {}, ompModule);
      seqassert(taskRedData, "could not find '_taskred_data'");

      auto *assign = M->Nr<AssignInstr>(
          p.second,
          util::call(taskRedData, {M->Nr<VarValue>(gtid), M->Nr<VarValue>(p.first)}));
      body->insert(body->begin(), assign);
      parent->push_back(p.second);
    }
  }
};

struct TaskLoopBodyStubReplacer : public util::Operator {
  CallInstr *replacement;
  bool hasReductions;

  TaskLoopBodyStubReplacer(CallInstr *replacement, bool hasReductions)
      : util::Operator(), replacement(replacement), hasReductions(hasReductions) {}

  void handle(CallInstr *v) override {
    auto *func = util::getFunc(v->getCallee());
    if (func && func->getUnmangledName() == "_task_loop_body_stub") {
      seqassert(replacement, "unexpected double replacement");
      seqassert(v->numArgs() == 3 && isA<VarValue>(v->front()) &&
                    isA<VarValue>(v->back()),
                "unexpected loop body stub");

      // the template passes gtid, privs and shareds to the body stub for convenience
      std::vector<Value *> args(v->begin(), v->end());
      auto *gtid = args[0];
      auto *privatesTuple = args[1];
      auto *sharedsTuple = args[2];
      unsigned privatesNext = 0;
      unsigned sharedsNext = 0;
      std::vector<Value *> newArgs;
      std::vector<bool> isArgShared;

      for (auto *arg : *replacement) {
        if (isA<VarValue>(arg)) {
          newArgs.push_back(util::tupleGet(privatesTuple, privatesNext++));
          isArgShared.push_back(false);
        } else if (isA<PointerValue>(arg)) {
          newArgs.push_back(util::tupleGet(sharedsTuple, sharedsNext++));
          isArgShared.push_back(true);
        } else {
          // make sure we're on the last arg, which should be gtid
          // in case of reductions
          seqassert(hasReductions && arg == replacement->back(), "unknown outline var");
        }
      }

      auto *outlinedFunc = cast<BodiedFunc>(util::getFunc(replacement->getCallee()));

      if (hasReductions) {
        newArgs.push_back(gtid);
        isArgShared.push_back(false);

        std::vector<Var *> reductionArgs;
        unsigned i = 0;
        for (auto it = outlinedFunc->arg_begin(); it != outlinedFunc->arg_end(); ++it) {
          if (isArgShared[i++])
            reductionArgs.push_back(*it);
        }
        TaskLoopReductionVarReplacer redrep(reductionArgs, outlinedFunc);
        outlinedFunc->accept(redrep);
        redrep.finalize();
      }

      v->replaceAll(util::call(outlinedFunc, newArgs));
      replacement = nullptr;
    }
  }
};

struct TaskLoopRoutineStubReplacer : public util::Operator {
  BodiedFunc *parent;
  std::vector<Value *> privates;
  std::vector<Value *> shareds;
  CallInstr *replacement;
  Var *loopVar;
  ReductionIdentifier *reds;
  std::vector<SharedInfo> sharedInfo;
  ReductionLocks locks;
  Var *locRef;
  Var *reductionLocRef;
  Var *gtid;

  Var *array;  // task reduction input array
  Var *tskgrp; // task group identifier

  void setupSharedInfo(std::vector<Reduction> &sharedRedux) {
    unsigned sharedsNext = 0;
    for (auto *val : shareds) {
      if (getVarFromOutlinedArg(val)->getId() != loopVar->getId()) {
        if (auto &reduction = sharedRedux[sharedsNext]) {
          Var *newVar = util::getVar(util::makeVar(
              reduction.getInitial(), cast<SeriesFlow>(parent->getBody()), parent,
              /*prepend=*/true));
          sharedInfo.push_back({sharedsNext, newVar, reduction});
        }
      }
      ++sharedsNext;
    }
  }

  TaskLoopRoutineStubReplacer(BodiedFunc *parent, std::vector<Value *> privates,
                              std::vector<Value *> shareds, CallInstr *replacement,
                              Var *loopVar, ReductionIdentifier *reds,
                              std::vector<Reduction> sharedRedux)
      : util::Operator(), parent(parent), privates(std::move(privates)),
        shareds(std::move(shareds)), replacement(replacement), loopVar(loopVar),
        reds(reds), sharedInfo(), locks(), locRef(nullptr), reductionLocRef(nullptr),
        gtid(nullptr), array(nullptr), tskgrp(nullptr) {
    setupSharedInfo(sharedRedux);
  }

  unsigned numReductions() {
    unsigned num = 0;
    for (auto &info : sharedInfo) {
      if (info.reduction)
        num += 1;
    }
    return num;
  }

  Value *getReductionTuple() {
    auto *M = parent->getModule();
    std::vector<Value *> elements;
    for (auto &info : sharedInfo) {
      if (info.reduction)
        elements.push_back(M->Nr<PointerValue>(info.local));
    }
    return util::makeTuple(elements, M);
  }

  BodiedFunc *makeTaskRedInitFunc(Reduction *reduction) {
    auto *M = parent->getModule();
    auto *argType = M->getPointerType(reduction->getType());
    auto *funcType = M->getFuncType(M->getVoidType(), {argType, argType});
    auto *initializer = M->Nr<BodiedFunc>("__red_init");
    initializer->realize(funcType, {"lhs", "rhs"});

    auto *lhsVar = initializer->arg_front();
    auto *body = M->Nr<SeriesFlow>();
    auto *lhsPtr = M->Nr<VarValue>(lhsVar);
    body->push_back(util::ptrStore(lhsPtr, reduction->getInitial()));
    initializer->setBody(body);
    return initializer;
  }

  BodiedFunc *makeTaskRedCombFunc(Reduction *reduction) {
    auto *M = parent->getModule();
    auto *argType = M->getPointerType(reduction->getType());
    auto *funcType = M->getFuncType(M->getVoidType(), {argType, argType});
    auto *reducer = M->Nr<BodiedFunc>("__red_comb");
    reducer->realize(funcType, {"lhs", "rhs"});

    auto *lhsVar = reducer->arg_front();
    auto *rhsVar = reducer->arg_back();
    auto *body = M->Nr<SeriesFlow>();
    auto *lhsPtr = M->Nr<VarValue>(lhsVar);
    auto *rhsPtr = M->Nr<VarValue>(rhsVar);
    body->push_back(
        reduction->generateNonAtomicReduction(lhsPtr, util::ptrLoad(rhsPtr)));
    reducer->setBody(body);
    return reducer;
  }

  Value *makeTaskRedInput(Reduction *reduction, Value *shar, Value *orig) {
    auto *M = shar->getModule();
    auto *size = M->Nr<TypePropertyInstr>(reduction->getType(),
                                          TypePropertyInstr::Property::SIZEOF);
    auto *init = ptrFromFunc(makeTaskRedInitFunc(reduction));
    auto *comb = ptrFromFunc(makeTaskRedCombFunc(reduction));

    auto *taskRedInputType = M->getOrRealizeType("TaskReductionInput", {}, ompModule);
    seqassert(taskRedInputType, "could not find 'TaskReductionInput' type");
    auto *result = taskRedInputType->construct({shar, orig, size, init, comb});
    seqassert(result, "bad construction of 'TaskReductionInput' type");
    return result;
  }

  BodiedFunc *makeReductionFunc() {
    auto *M = parent->getModule();
    auto *tupleType = getReductionTuple()->getType();
    auto *argType = M->getPointerType(tupleType);
    auto *funcType = M->getFuncType(M->getVoidType(), {argType, argType});
    auto *reducer = M->Nr<BodiedFunc>("__omp_reducer");
    reducer->realize(funcType, {"lhs", "rhs"});

    auto *lhsVar = reducer->arg_front();
    auto *rhsVar = reducer->arg_back();
    auto *body = M->Nr<SeriesFlow>();
    unsigned next = 0;
    for (auto &info : sharedInfo) {
      if (info.reduction) {
        auto *lhs = util::ptrLoad(M->Nr<VarValue>(lhsVar));
        auto *rhs = util::ptrLoad(M->Nr<VarValue>(rhsVar));
        auto *lhsElem = util::tupleGet(lhs, next);
        auto *rhsElem = util::tupleGet(rhs, next);
        body->push_back(
            info.reduction.generateNonAtomicReduction(lhsElem, util::ptrLoad(rhsElem)));
        ++next;
      }
    }
    reducer->setBody(body);
    return reducer;
  }

  void handle(VarValue *v) override {
    auto *M = v->getModule();
    auto *func = util::getFunc(v);
    if (func && func->getUnmangledName() == "_routine_stub") {
      util::CloneVisitor cv(M);
      auto *newRoutine = cv.forceClone(func);
      TaskLoopBodyStubReplacer rep(replacement, (numReductions() > 0));
      newRoutine->accept(rep);
      v->setVar(newRoutine);
    }
  }

  void handle(CallInstr *v) override {
    auto *M = v->getModule();
    auto *func = util::getFunc(v->getCallee());
    if (!func)
      return;
    auto name = func->getUnmangledName();

    if (name == "_loop_loc_and_gtid") {
      seqassert(v->numArgs() == 3 &&
                    std::all_of(v->begin(), v->end(),
                                [](auto x) { return isA<VarValue>(x); }),
                "unexpected loop loc and gtid stub");
      std::vector<Value *> args(v->begin(), v->end());
      locRef = util::getVar(args[0]);
      reductionLocRef = util::getVar(args[1]);
      gtid = util::getVar(args[2]);
    }

    if (name == "_taskred_setup") {
      seqassert(reductionLocRef && gtid, "bad visit order in template");
      seqassert(v->numArgs() == 1 && isA<VarValue>(v->front()),
                "unexpected shared updates stub");
      unsigned numRed = numReductions();
      if (numRed == 0)
        return;

      auto *M = parent->getModule();
      auto *extras = util::getVar(v->front());

      // add task reduction inputs
      auto *taskRedInitSeries = M->Nr<SeriesFlow>();
      auto *taskRedInputType = M->getOrRealizeType("TaskReductionInput", {}, ompModule);
      seqassert(taskRedInputType, "could not find 'TaskReductionInput' type");
      auto *irArrayType = M->getOrRealizeType("TaskReductionInputArray", {}, ompModule);
      seqassert(irArrayType, "could not find 'TaskReductionInputArray' type");
      auto *taskRedInputsArray = util::makeVar(
          M->Nr<StackAllocInstr>(irArrayType, numRed), taskRedInitSeries, parent);
      array = util::getVar(taskRedInputsArray);
      auto *taskRedInputsArrayType = taskRedInputsArray->getType();

      auto *taskRedSetItem = M->getOrRealizeMethod(
          taskRedInputsArrayType, Module::SETITEM_MAGIC_NAME,
          {taskRedInputsArrayType, M->getIntType(), taskRedInputType});
      seqassert(taskRedSetItem,
                "could not find 'TaskReductionInputArray.__setitem__' method");
      int i = 0;
      for (auto &info : sharedInfo) {
        if (info.reduction) {
          Value *shar = M->Nr<PointerValue>(info.local);
          Value *orig = util::tupleGet(M->Nr<VarValue>(extras), info.memb);
          auto *taskRedInput = makeTaskRedInput(&info.reduction, shar, orig);
          taskRedInitSeries->push_back(util::call(
              taskRedSetItem, {M->Nr<VarValue>(array), M->getInt(i++), taskRedInput}));
        }
      }

      auto *arrayPtr = M->Nr<ExtractInstr>(M->Nr<VarValue>(array), "ptr");
      auto *taskRedInitFunc =
          M->getOrRealizeFunc("_taskred_init",
                              {reductionLocRef->getType(), gtid->getType(),
                               M->getIntType(), arrayPtr->getType()},
                              {}, ompModule);
      seqassert(taskRedInitFunc, "task red init function not found");
      auto *taskRedInitResult =
          util::makeVar(util::call(taskRedInitFunc, {M->Nr<VarValue>(reductionLocRef),
                                                     M->Nr<VarValue>(gtid),
                                                     M->getInt(numRed), arrayPtr}),
                        taskRedInitSeries, parent);
      tskgrp = util::getVar(taskRedInitResult);
      v->replaceAll(taskRedInitSeries);
    }

    if (name == "_fix_privates_and_shareds") {
      std::vector<Value *> args(v->begin(), v->end());
      seqassert(args.size() == 3, "invalid _fix_privates_and_shareds call found");
      unsigned numRed = numReductions();
      auto *newLoopVar = args[0];
      auto *privatesTuple = args[1];
      auto *sharedsTuple = args[2];

      unsigned privatesNext = 0;
      unsigned sharedsNext = 0;
      unsigned infoNext = 0;

      bool needNewPrivates = false;
      bool needNewShareds = false;

      std::vector<Value *> newPrivates;
      std::vector<Value *> newShareds;

      for (auto *val : privates) {
        if (numRed > 0 && val == privates.back()) { // i.e. task group identifier
          seqassert(tskgrp, "tskgrp var not set");
          newPrivates.push_back(M->Nr<VarValue>(tskgrp));
          needNewPrivates = true;
        } else if (getVarFromOutlinedArg(val)->getId() != loopVar->getId()) {
          newPrivates.push_back(util::tupleGet(privatesTuple, privatesNext));
        } else {
          newPrivates.push_back(newLoopVar);
          needNewPrivates = true;
        }
        ++privatesNext;
      }

      for (auto *val : shareds) {
        if (getVarFromOutlinedArg(val)->getId() != loopVar->getId()) {
          auto &info = sharedInfo[infoNext];
          if (info.memb == sharedsNext && info.reduction) {
            newShareds.push_back(M->Nr<PointerValue>(info.local));
            needNewShareds = true;
            ++infoNext;
          } else {
            newShareds.push_back(util::tupleGet(sharedsTuple, sharedsNext));
          }
        } else {
          newShareds.push_back(M->Nr<PointerValue>(util::getVar(newLoopVar)));
          needNewShareds = true;
        }
        ++sharedsNext;
      }

      privatesTuple = needNewPrivates ? util::makeTuple(newPrivates, M) : privatesTuple;
      sharedsTuple = needNewShareds ? util::makeTuple(newShareds, M) : sharedsTuple;

      Value *result = util::makeTuple({privatesTuple, sharedsTuple}, M);
      v->replaceAll(result);
    }

    if (name == "_loop_reductions") {
      seqassert(reductionLocRef && gtid, "bad visit order in template");
      seqassert(v->numArgs() == 1 && isA<VarValue>(v->front()),
                "unexpected shared updates stub");
      if (numReductions() == 0)
        return;

      auto *M = parent->getModule();
      auto *extras = util::getVar(v->front());
      auto *reductionTuple = getReductionTuple();
      auto *reducer = makeReductionFunc();
      auto *lck = locks.getMainLock(M);
      auto *rawReducer = ptrFromFunc(reducer);

      auto *taskRedFini = M->getOrRealizeFunc(
          "_taskred_fini", {reductionLocRef->getType(), gtid->getType()}, {},
          ompModule);
      seqassert(taskRedFini, "taskred finish function not found not found");
      auto *reduceNoWait = M->getOrRealizeFunc(
          "_reduce_nowait",
          {reductionLocRef->getType(), gtid->getType(), reductionTuple->getType(),
           rawReducer->getType(), lck->getType()},
          {}, ompModule);
      seqassert(reduceNoWait, "reduce nowait function not found");
      auto *reduceNoWaitEnd = M->getOrRealizeFunc(
          "_end_reduce_nowait",
          {reductionLocRef->getType(), gtid->getType(), lck->getType()}, {}, ompModule);
      seqassert(reduceNoWaitEnd, "end reduce nowait function not found");

      auto *series = M->Nr<SeriesFlow>();
      series->push_back(util::call(
          taskRedFini, {M->Nr<VarValue>(reductionLocRef), M->Nr<VarValue>(gtid)}));
      auto *tupleVal = util::makeVar(reductionTuple, series, parent);
      auto *reduceCode = util::call(reduceNoWait, {M->Nr<VarValue>(reductionLocRef),
                                                   M->Nr<VarValue>(gtid), tupleVal,
                                                   rawReducer, M->Nr<VarValue>(lck)});
      auto *codeVar = util::makeVar(reduceCode, series, parent)->getVar();
      seqassert(codeVar->getType()->is(M->getIntType()), "wrong reduce code type");

      auto *sectionNonAtomic = M->Nr<SeriesFlow>();
      auto *sectionAtomic = M->Nr<SeriesFlow>();

      for (auto &info : sharedInfo) {
        if (info.reduction) {
          Value *ptr = util::tupleGet(M->Nr<VarValue>(extras), info.memb);
          Value *arg = M->Nr<VarValue>(info.local);
          sectionNonAtomic->push_back(
              info.reduction.generateNonAtomicReduction(ptr, arg));
        }
      }
      sectionNonAtomic->push_back(
          util::call(reduceNoWaitEnd, {M->Nr<VarValue>(reductionLocRef),
                                       M->Nr<VarValue>(gtid), M->Nr<VarValue>(lck)}));

      for (auto &info : sharedInfo) {
        if (info.reduction) {
          Value *ptr = util::tupleGet(M->Nr<VarValue>(extras), info.memb);
          Value *arg = M->Nr<VarValue>(info.local);
          sectionAtomic->push_back(
              info.reduction.generateAtomicReduction(ptr, arg, locRef, gtid, locks));
        }
      }

      // make: if code == 1 { sectionNonAtomic } elif code == 2 { sectionAtomic }
      auto *theSwitch = M->Nr<IfFlow>(
          *M->Nr<VarValue>(codeVar) == *M->getInt(1), sectionNonAtomic,
          util::series(M->Nr<IfFlow>(*M->Nr<VarValue>(codeVar) == *M->getInt(2),
                                     sectionAtomic)));
      series->push_back(theSwitch);
      v->replaceAll(series);
    }
  }
};

template <typename T> void unpar(T *v) { v->setParallel(false); }
} // namespace

const std::string OpenMPPass::KEY = "core-parallel-openmp";

void OpenMPPass::handle(ForFlow *v) {
  if (!v->isParallel())
    return unpar(v);
  auto *M = v->getModule();
  auto *parent = cast<BodiedFunc>(getParentFunc());
  auto *body = cast<SeriesFlow>(v->getBody());
  if (!parent || !body)
    return unpar(v);
  auto outline = util::outlineRegion(parent, body, /*allowOutflows=*/false,
                                     /*outlineGlobals=*/true);
  if (!outline)
    return unpar(v);

  // set up args to pass fork_call
  auto *sched = v->getSchedule();
  Var *loopVar = v->getVar();
  OMPTypes types(M);

  // shared argument vars
  std::vector<Var *> sharedVars;
  unsigned i = 0;
  for (auto it = outline.func->arg_begin(); it != outline.func->arg_end(); ++it) {
    if (outline.argKinds[i++] == util::OutlineResult::ArgKind::MODIFIED)
      sharedVars.push_back(*it);
  }
  ReductionIdentifier reds(sharedVars);
  outline.func->accept(reds);

  // separate arguments into 'private' and 'shared'
  std::vector<Reduction> sharedRedux; // reductions corresponding to shared vars
  std::vector<Value *> privates, shareds;
  i = 0;
  for (auto *arg : *outline.call) {
    if (isA<VarValue>(arg)) {
      privates.push_back(arg);
    } else {
      shareds.push_back(arg);
      sharedRedux.push_back(reds.getReduction(sharedVars[i++]));
    }
  }

  util::CloneVisitor cv(M);

  // We need to pass the task group identifier returned from
  // __kmpc_taskred_modifier_init to the task entry, so append
  // it to private data (initially as null void pointer). Also
  // we add an argument to the end of the outlined function for
  // the gtid.
  if (reds.reductions.size() > 0) {
    auto *nullPtr = types.i8ptr->construct({});
    privates.push_back(nullPtr);

    auto *outlinedFuncType = cast<types::FuncType>(outline.func->getType());
    std::vector<types::Type *> argTypes(outlinedFuncType->begin(),
                                        outlinedFuncType->end());
    argTypes.push_back(M->getIntType());
    auto *retType = outlinedFuncType->getReturnType();

    std::vector<Var *> oldArgVars(outline.func->arg_begin(), outline.func->arg_end());
    std::vector<std::string> argNames;

    for (auto *var : oldArgVars) {
      argNames.push_back(var->getName());
    }
    argNames.push_back("gtid");

    auto *newOutlinedFunc = M->Nr<BodiedFunc>("__outlined_new");
    newOutlinedFunc->realize(M->getFuncType(retType, argTypes), argNames);

    std::vector<Var *> newArgVars(newOutlinedFunc->arg_begin(),
                                  newOutlinedFunc->arg_end());

    std::unordered_map<id_t, Var *> remaps;
    for (unsigned i = 0; i < oldArgVars.size(); i++) {
      remaps.emplace(oldArgVars[i]->getId(), newArgVars[i]);
    }
    auto *newBody =
        cast<SeriesFlow>(cv.clone(outline.func->getBody(), newOutlinedFunc, remaps));
    newOutlinedFunc->setBody(newBody);

    // update outline struct
    outline.func = newOutlinedFunc;
    outline.call->setCallee(M->Nr<VarValue>(newOutlinedFunc));
    outline.call->insert(outline.call->end(), M->getInt(0));
    outline.argKinds.push_back(util::OutlineResult::ArgKind::CONSTANT);
  }

  auto *privatesTuple = util::makeTuple(privates, M);
  auto *sharedsTuple = util::makeTuple(shareds, M);

  // template call
  std::vector<types::Type *> templateFuncArgs = {
      types.i32ptr, types.i32ptr,
      M->getPointerType(
          M->getTupleType({v->getIter()->getType(), privatesTuple->getType(),
                           sharedsTuple->getType()}))};
  auto *templateFunc = M->getOrRealizeFunc("_task_loop_outline_template",
                                           templateFuncArgs, {}, ompModule);
  seqassert(templateFunc, "task loop outline template not found");

  templateFunc = cv.forceClone(templateFunc);
  TaskLoopRoutineStubReplacer rep(cast<BodiedFunc>(templateFunc), privates, shareds,
                                  outline.call, loopVar, &reds, sharedRedux);
  templateFunc->accept(rep);
  auto *rawTemplateFunc = ptrFromFunc(templateFunc);

  // fork call
  std::vector<Value *> forkExtraArgs = {v->getIter(), privatesTuple, sharedsTuple};
  auto *forkExtra = util::makeTuple(forkExtraArgs, M);
  std::vector<types::Type *> forkArgTypes = {types.i8ptr, forkExtra->getType()};
  auto *forkFunc = M->getOrRealizeFunc("_fork_call", forkArgTypes, {}, ompModule);
  seqassert(forkFunc, "fork call function not found");
  auto *fork = util::call(forkFunc, {rawTemplateFunc, forkExtra});

  if (sched->threads && sched->threads->getType()->is(M->getIntType())) {
    auto *pushNumThreadsFunc =
        M->getOrRealizeFunc("_push_num_threads", {M->getIntType()}, {}, ompModule);
    seqassert(pushNumThreadsFunc, "push num threads func not found");
    auto *pushNumThreads = util::call(pushNumThreadsFunc, {sched->threads});
    insertBefore(pushNumThreads);
  }

  v->replaceAll(fork);
}

void OpenMPPass::handle(ImperativeForFlow *v) {
  if (!v->isParallel() || v->getStep() == 0)
    return unpar(v);
  auto *M = v->getModule();
  auto *parent = cast<BodiedFunc>(getParentFunc());
  auto *body = cast<SeriesFlow>(v->getBody());
  if (!parent || !body)
    return unpar(v);
  auto outline = util::outlineRegion(parent, body, /*allowOutflows=*/false,
                                     /*outlineGlobals=*/true);
  if (!outline)
    return unpar(v);

  // set up args to pass fork_call
  auto *sched = v->getSchedule();
  Var *loopVar = v->getVar();
  OMPTypes types(M);

  // shared argument vars
  std::vector<Var *> shareds;
  unsigned i = 0;
  for (auto it = outline.func->arg_begin(); it != outline.func->arg_end(); ++it) {
    if (outline.argKinds[i++] == util::OutlineResult::ArgKind::MODIFIED)
      shareds.push_back(*it);
  }
  ReductionIdentifier reds(shareds);
  outline.func->accept(reds);

  // gather extra arguments
  std::vector<Value *> extraArgs;
  std::vector<types::Type *> extraArgTypes;
  for (auto *arg : *outline.call) {
    if (getVarFromOutlinedArg(arg)->getId() != loopVar->getId()) {
      extraArgs.push_back(arg);
      extraArgTypes.push_back(arg->getType());
    }
  }

  // template call
  std::string templateFuncName;
  if (sched->dynamic) {
    templateFuncName = "_dynamic_loop_outline_template";
  } else if (sched->chunk) {
    templateFuncName = "_static_chunked_loop_outline_template";
  } else {
    templateFuncName = "_static_loop_outline_template";
  }
  auto *intType = M->getIntType();
  std::vector<types::Type *> templateFuncArgs = {
      types.i32ptr, types.i32ptr,
      M->getPointerType(M->getTupleType(
          {intType, intType, intType, M->getTupleType(extraArgTypes)}))};
  auto *templateFunc =
      M->getOrRealizeFunc(templateFuncName, templateFuncArgs, {}, ompModule);
  seqassert(templateFunc, "imperative loop outline template not found");

  util::CloneVisitor cv(M);
  templateFunc = cast<Func>(cv.forceClone(templateFunc));
  ImperativeLoopTemplateReplacer rep(cast<BodiedFunc>(templateFunc), outline.call,
                                     loopVar, sched, &reds, v->getStep());
  templateFunc->accept(rep);
  auto *rawTemplateFunc = ptrFromFunc(templateFunc);

  // fork call
  auto *chunk = (sched->chunk && sched->chunk->getType()->is(intType)) ? sched->chunk
                                                                       : M->getInt(1);
  std::vector<Value *> forkExtraArgs = {chunk, v->getStart(), v->getEnd()};
  for (auto *arg : extraArgs) {
    forkExtraArgs.push_back(arg);
  }
  auto *forkExtra = util::makeTuple(forkExtraArgs, M);
  std::vector<types::Type *> forkArgTypes = {types.i8ptr, forkExtra->getType()};
  auto *forkFunc = M->getOrRealizeFunc("_fork_call", forkArgTypes, {}, ompModule);
  seqassert(forkFunc, "fork call function not found");
  auto *fork = util::call(forkFunc, {rawTemplateFunc, forkExtra});

  if (sched->threads && sched->threads->getType()->is(intType)) {
    auto *pushNumThreadsFunc =
        M->getOrRealizeFunc("_push_num_threads", {intType}, {}, ompModule);
    seqassert(pushNumThreadsFunc, "push num threads func not found");
    auto *pushNumThreads = util::call(pushNumThreadsFunc, {sched->threads});
    insertBefore(pushNumThreads);
  }

  v->replaceAll(fork);
}

} // namespace parallel
} // namespace transform
} // namespace ir
} // namespace codon
