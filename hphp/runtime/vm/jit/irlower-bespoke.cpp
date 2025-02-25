/*
  +----------------------------------------------------------------------+
  | HipHop for PHP                                                       |
  +----------------------------------------------------------------------+
  | Copyright (c) 2010-present Facebook, Inc. (http://www.facebook.com)  |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt                                  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
*/

#include "hphp/runtime/vm/jit/irlower-bespoke.h"

#include "hphp/runtime/base/bespoke/escalation-logging.h"
#include "hphp/runtime/base/bespoke/layout.h"
#include "hphp/runtime/base/bespoke/logging-array.h"
#include "hphp/runtime/base/bespoke/logging-profile.h"
#include "hphp/runtime/base/bespoke/monotype-dict.h"
#include "hphp/runtime/base/bespoke/monotype-vec.h"
#include "hphp/runtime/base/bespoke/struct-dict.h"
#include "hphp/runtime/base/vanilla-dict.h"
#include "hphp/runtime/base/vanilla-keyset.h"
#include "hphp/runtime/base/vanilla-vec.h"

#include "hphp/runtime/vm/jit/code-gen-cf.h"
#include "hphp/runtime/vm/jit/code-gen-helpers.h"
#include "hphp/runtime/vm/jit/irlower.h"
#include "hphp/runtime/vm/jit/irlower-internal.h"
#include "hphp/runtime/vm/jit/ir-opcode.h"
#include "hphp/runtime/vm/jit/minstr-helpers.h"
#include "hphp/runtime/vm/jit/translator-inline.h"

namespace HPHP { namespace jit { namespace irlower {

//////////////////////////////////////////////////////////////////////////////
// Generic BespokeArrays

namespace {
static void logGuardFailure(TypedValue tv, uint16_t layout, uint64_t sk) {
  assertx(tvIsArrayLike(tv));
  auto const al = ArrayLayout::FromUint16(layout);
  bespoke::logGuardFailure(val(tv).parr, al, SrcKey(sk));
}

bool maybeLogging(Type t) {
  auto const loggingLayout =
    ArrayLayout(bespoke::LoggingArray::GetLayoutIndex());
  auto const loggingArray = TArrLike.narrowToLayout(loggingLayout);
  return t.maybe(loggingArray);
}
}

void cgLogArrayReach(IRLS& env, const IRInstruction* inst) {
  auto const data = inst->extra<LogArrayReach>();

  auto& v = vmain(env);
  auto const args = argGroup(env, inst).imm(data->profile).ssa(0);
  auto const target = CallSpec::method(&bespoke::SinkProfile::update);
  cgCallHelper(v, env, target, callDest(env, inst), SyncOptions::Sync, args);
}

void cgLogGuardFailure(IRLS& env, const IRInstruction* inst) {
  auto const layout = inst->typeParam().arrSpec().layout().toUint16();
  auto const sk = inst->marker().sk().toAtomicInt();

  auto& v = vmain(env);
  auto const args = argGroup(env, inst).typedValue(0).imm(layout).imm(sk);
  auto const target = CallSpec::direct(logGuardFailure);
  cgCallHelper(v, env, target, callDest(env, inst), SyncOptions::Sync, args);
}

void cgNewLoggingArray(IRLS& env, const IRInstruction* inst) {
  auto const data = inst->extra<NewLoggingArray>();

  auto const target = [&] {
    using Fn = ArrayData*(*)(ArrayData*, bespoke::LoggingProfile*);
    return shouldTestBespokeArrayLikes()
      ? CallSpec::direct(static_cast<Fn>(bespoke::makeBespokeForTesting))
      : CallSpec::direct(static_cast<Fn>(bespoke::maybeMakeLoggingArray));
  }();

  cgCallHelper(vmain(env), env, target, callDest(env, inst), SyncOptions::Sync,
               argGroup(env, inst).ssa(0).immPtr(data->profile));
}

void cgProfileArrLikeProps(IRLS& env, const IRInstruction* inst) {
  auto const target = CallSpec::direct(bespoke::profileArrLikeProps);
  cgCallHelper(vmain(env), env, target, kVoidDest, SyncOptions::Sync,
               argGroup(env, inst).ssa(0));
}

//////////////////////////////////////////////////////////////////////////////

// This macro returns a CallSpec to one of several static functions:
//
//    - the one on a specific, concrete bespoke layout;
//    - the generic one on BespokeArray;
//    - the ones on the vanilla arrays (Packed, Mixed, Set);
//    - failing all those options, the CallSpec Generic
//
#define CALL_TARGET(Type, Fn, Generic)                              \
  [&]{                                                              \
    auto const layout = Type.arrSpec().layout();                    \
    if (layout.bespoke()) {                                         \
      auto const vtable = layout.bespokeLayout()->vtable();         \
      if (vtable->fn##Fn) {                                         \
        return CallSpec::direct(vtable->fn##Fn);                    \
      } else {                                                      \
        return CallSpec::direct(BespokeArray::Fn);                  \
      }                                                             \
    }                                                               \
    if (layout.vanilla()) {                                         \
      if (arr <= TVec)    return CallSpec::direct(VanillaVec::Fn);  \
      if (arr <= TDict)   return CallSpec::direct(VanillaDict::Fn); \
      if (arr <= TKeyset) return CallSpec::direct(VanillaKeyset::Fn);\
    }                                                               \
    return Generic;                                                 \
  }()

#define CALL_TARGET_SYNTH(Type, Fn, Generic)                                 \
  [&]{                                                                       \
    auto const layout = Type.arrSpec().layout();                             \
    if (layout.bespoke()) {                                                  \
      auto const vtable = layout.bespokeLayout()->vtable();                  \
      if (vtable->fn##Fn) {                                                  \
        return CallSpec::direct(vtable->fn##Fn);                             \
      } else {                                                               \
        return CallSpec::direct(BespokeArray::Fn);                           \
      }                                                                      \
    }                                                                        \
    if (layout.vanilla()) {                                                  \
      if (arr <= TVec) {                                                     \
        return CallSpec::direct(SynthesizedArrayFunctions<VanillaVec>::Fn);  \
      }                                                                      \
      if (arr <= TDict) {                                                    \
        return CallSpec::direct(SynthesizedArrayFunctions<VanillaDict>::Fn); \
      }                                                                      \
      if (arr <= TKeyset) {                                                  \
        return CallSpec::direct(SynthesizedArrayFunctions<VanillaKeyset>::Fn);\
      }                                                                      \
    }                                                                        \
    return Generic;                                                          \
  }()

CallSpec destructorForArrayLike(Type arr) {
  assertx(arr <= TArrLike);
  assertx(allowBespokeArrayLikes());
  return CALL_TARGET(arr, Release, CallSpec::method(&ArrayData::release));
}

CallSpec copyFuncForArrayLike(Type arr) {
  assertx(arr <= TArrLike);
  assertx(allowBespokeArrayLikes());
  return CALL_TARGET(arr, Copy, CallSpec::method(&ArrayData::copy));
}

void cgBespokeGet(IRLS& env, const IRInstruction* inst) {
  using GetInt = TypedValue (ArrayData::*)(int64_t) const;
  using GetStr = TypedValue (ArrayData::*)(const StringData*) const;

  auto const getInt =
    CallSpec::method(static_cast<GetInt>(&ArrayData::get));
  auto const getStr =
    CallSpec::method(static_cast<GetStr>(&ArrayData::get));

  auto const arr = inst->src(0)->type();
  auto const key = inst->src(1)->type();
  auto const target = (key <= TInt)
    ? CALL_TARGET(arr, NvGetInt, getInt)
    : CALL_TARGET(arr, NvGetStr, getStr);

  auto const syncMode = maybeLogging(inst->src(0)->type())
    ? SyncOptions::Sync
    : SyncOptions::None;

  auto& v = vmain(env);
  auto const args = argGroup(env, inst).ssa(0).ssa(1);
  cgCallHelper(v, env, target, callDestTV(env, inst), syncMode, args);

}

void cgBespokeGetThrow(IRLS& env, const IRInstruction* inst) {
  using GetInt = TypedValue (ArrayData::*)(int64_t) const;
  using GetStr = TypedValue (ArrayData::*)(const StringData*) const;

  auto const getInt =
    CallSpec::method(static_cast<GetInt>(&ArrayData::getThrow));
  auto const getStr =
    CallSpec::method(static_cast<GetStr>(&ArrayData::getThrow));

  auto const arr = inst->src(0)->type();
  auto const key = inst->src(1)->type();
  auto const target = (key <= TInt)
    ? CALL_TARGET_SYNTH(arr, NvGetIntThrow, getInt)
    : CALL_TARGET_SYNTH(arr, NvGetStrThrow, getStr);

  auto& v = vmain(env);
  auto const args = argGroup(env, inst).ssa(0).ssa(1);
  cgCallHelper(v, env, target, callDestTV(env, inst), SyncOptions::Sync, args);
}

void cgBespokeSet(IRLS& env, const IRInstruction* inst) {
  using SetInt = ArrayData* (ArrayData::*)(int64_t, TypedValue);
  using SetStr = ArrayData* (ArrayData::*)(StringData*, TypedValue);

  auto const setIntMove =
    CallSpec::method(static_cast<SetInt>(&ArrayData::setMove));
  auto const setStrMove =
    CallSpec::method(static_cast<SetStr>(&ArrayData::setMove));

  auto const arr = inst->src(0)->type();
  auto const key = inst->src(1)->type();
  auto const target = (key <= TInt)
    ? CALL_TARGET(arr, SetIntMove, setIntMove)
    : CALL_TARGET(arr, SetStrMove, setStrMove);

  auto& v = vmain(env);
  auto const args = argGroup(env, inst).ssa(0).ssa(1).typedValue(2);
  cgCallHelper(v, env, target, callDest(env, inst), SyncOptions::Sync, args);
}

void cgBespokeUnset(IRLS &env, const IRInstruction *inst) {
  using UnsetInt = ArrayData* (ArrayData::*)(int64_t);
  using UnsetStr = ArrayData* (ArrayData::*)(const StringData*);

  auto const unsetIntMove =
    CallSpec::method(static_cast<UnsetInt>(&ArrayData::removeMove));
  auto const unsetStrMove =
    CallSpec::method(static_cast<UnsetStr>(&ArrayData::removeMove));

  auto const arr = inst->src(0)->type();
  auto const key = inst->src(1)->type();
  auto const target = (key <= TInt)
    ? CALL_TARGET(arr, RemoveIntMove, unsetIntMove)
    : CALL_TARGET(arr, RemoveStrMove, unsetStrMove);

  auto& v = vmain(env);
  auto const args = argGroup(env, inst).ssa(0).ssa(1);
  cgCallHelper(v, env, target, callDest(env, inst), SyncOptions::Sync, args);
}

void cgBespokeAppend(IRLS& env, const IRInstruction* inst) {
  using Append = ArrayData* (ArrayData::*)(TypedValue);

  auto const appendMove =
    CallSpec::method(static_cast<Append>(&ArrayData::appendMove));

  auto const arr = inst->src(0)->type();
  auto const target = CALL_TARGET(arr, AppendMove, appendMove);

  auto& v = vmain(env);
  auto const args = argGroup(env, inst).ssa(0).typedValue(1);
  cgCallHelper(v, env, target, callDest(env, inst), SyncOptions::Sync, args);
}

void cgBespokeIterFirstPos(IRLS& env, const IRInstruction* inst) {
  auto const arr = inst->src(0)->type();
  auto const iterBegin = CallSpec::method(&ArrayData::iter_begin);
  auto const target = CALL_TARGET(arr, IterBegin, iterBegin);

  auto& v = vmain(env);
  auto const args = argGroup(env, inst).ssa(0);
  cgCallHelper(v, env, target, callDest(env, inst), SyncOptions::None, args);
}

void cgBespokeIterLastPos(IRLS& env, const IRInstruction* inst) {
  auto const arr = inst->src(0)->type();
  auto const iterLast = CallSpec::method(&ArrayData::iter_last);
  auto const target = CALL_TARGET(arr, IterLast, iterLast);

  auto& v = vmain(env);
  auto const args = argGroup(env, inst).ssa(0);
  cgCallHelper(v, env, target, callDest(env, inst), SyncOptions::None, args);
}

void cgBespokeIterEnd(IRLS& env, const IRInstruction* inst) {
  auto const arr = inst->src(0)->type();
  auto const iterEnd = CallSpec::method(&ArrayData::iter_end);
  auto const target = CALL_TARGET(arr, IterEnd, iterEnd);

  auto& v = vmain(env);
  auto const args = argGroup(env, inst).ssa(0);
  cgCallHelper(v, env, target, callDest(env, inst), SyncOptions::None, args);
}

void cgBespokeIterGetKey(IRLS& env, const IRInstruction* inst) {
  auto const arr = inst->src(0)->type();
  auto const getPosKey = CallSpec::method(&ArrayData::nvGetKey);
  auto const target = CALL_TARGET(arr, GetPosKey, getPosKey);

  auto& v = vmain(env);
  auto const args = argGroup(env, inst).ssa(0).ssa(1);
  cgCallHelper(v, env, target, callDestTV(env, inst), SyncOptions::None, args);
}

void cgBespokeIterGetVal(IRLS& env, const IRInstruction* inst) {
  auto const arr = inst->src(0)->type();
  auto const getPosVal = CallSpec::method(&ArrayData::nvGetVal);
  auto const target = CALL_TARGET(arr, GetPosVal, getPosVal);

  auto& v = vmain(env);
  auto const args = argGroup(env, inst).ssa(0).ssa(1);
  cgCallHelper(v, env, target, callDestTV(env, inst), SyncOptions::None, args);
}

void cgBespokeEscalateToVanilla(IRLS& env, const IRInstruction* inst) {
  auto const target = [&] {
    auto const layout = inst->src(0)->type().arrSpec().layout();
    auto const vtable = layout.bespokeLayout()->vtable();
    if (vtable->fnEscalateToVanilla) {
      return CallSpec::direct(vtable->fnEscalateToVanilla);
    } else {
      return CallSpec::direct(BespokeArray::ToVanilla);
    }
  }();

  auto const syncMode = maybeLogging(inst->src(0)->type())
    ? SyncOptions::Sync
    : SyncOptions::None;

  auto& v = vmain(env);
  auto const reason = inst->src(1)->strVal()->data();
  auto const args = argGroup(env, inst).ssa(0).imm(reason);
  cgCallHelper(v, env, target, callDest(env, inst), syncMode, args);
}

#undef CALL_TARGET

void cgBespokeElem(IRLS& env, const IRInstruction* inst) {
  auto& v = vmain(env);
  auto const dest = callDest(env, inst);

  auto args = argGroup(env, inst).ssa(0).ssa(1);

  auto const target = [&] {
    auto const arr = inst->src(0);
    auto const key = inst->src(1);
    auto const layout = arr->type().arrSpec().layout();

    // Bespoke arrays always have specific Elem helper functions.
    if (layout.bespoke()) {
      args.ssa(2);
      auto const vtable = layout.bespokeLayout()->vtable();
      if (key->isA(TStr) && vtable->fnElemStr) {
        return CallSpec::direct(vtable->fnElemStr);
      } else if (key->isA(TInt) && vtable->fnElemInt) {
        return CallSpec::direct(vtable->fnElemInt);
      } else {
        return key->isA(TStr) ? CallSpec::direct(BespokeArray::ElemStr)
                              : CallSpec::direct(BespokeArray::ElemInt);
      }
    }

    // Aside from known-bespokes, we only specialize certain Elem cases -
    // the ones we already have symbols for in the MInstrHelpers namespace.
    using namespace MInstrHelpers;
    auto const throwOnMissing = inst->src(2)->boolVal();
    if (layout.vanilla()) {
      if (arr->isA(TDict)) {
        return key->isA(TStr)
          ? CallSpec::direct(throwOnMissing ? elemDictSD : elemDictSU)
          : CallSpec::direct(throwOnMissing ? elemDictID : elemDictIU);
      }
      if (arr->isA(TKeyset) && !throwOnMissing) {
        return key->isA(TStr)
          ? CallSpec::direct(elemKeysetSU)
          : CallSpec::direct(elemKeysetIU);
      }
    }
    return key->isA(TStr)
      ? CallSpec::direct(throwOnMissing ? elemSD : elemSU)
      : CallSpec::direct(throwOnMissing ? elemID : elemIU);
  }();
  cgCallHelper(v, env, target, dest, SyncOptions::Sync, args);
}

//////////////////////////////////////////////////////////////////////////////
// MonotypeVec and MonotypeDict

namespace {
using MonotypeDict = bespoke::MonotypeDict<int64_t>;

// Returns a pointer to a value `off` bytes into the MonotypeDict element at
// the iterator position `pos` in the dict pointed to by `rarr`.
Vptr ptrToMonotypeDictElm(Vout& v, Vreg rarr, Vreg rpos, Type pos, size_t off) {
  auto const base = MonotypeDict::entriesOffset() + off;

  if (pos.hasConstVal()) {
    auto const offset = pos.intVal() * MonotypeDict::elmSize() + base;
    if (deltaFits(offset, sz::dword)) return rarr[offset];
  }

  static_assert(MonotypeDict::elmSize() == 16);
  auto posl = v.makeReg();
  auto scaled_posl = v.makeReg();
  auto scaled_pos = v.makeReg();
  v << movtql{rpos, posl};
  v << shlli{1, posl, scaled_posl, v.makeReg()};
  v << movzlq{scaled_posl, scaled_pos};
  return rarr[scaled_pos * int(MonotypeDict::elmSize() / 2) + base];
}
}

void cgLdMonotypeDictTombstones(IRLS& env, const IRInstruction* inst) {
  static_assert(MonotypeDict::tombstonesSize() == 2);
  auto const rarr = srcLoc(env, inst, 0).reg();
  auto const used = dstLoc(env, inst, 0).reg();
  vmain(env) << loadzwq{rarr[MonotypeDict::tombstonesOffset()], used};
}

void cgLdMonotypeDictKey(IRLS& env, const IRInstruction* inst) {
  auto const rarr = srcLoc(env, inst, 0).reg();
  auto const rpos = srcLoc(env, inst, 1).reg();
  auto const pos = inst->src(1)->type();
  auto const dst = dstLoc(env, inst, 0);

  auto& v = vmain(env);
  auto const off = MonotypeDict::elmKeyOffset();
  auto const ptr = ptrToMonotypeDictElm(v, rarr, rpos, pos, off);
  v << load{ptr, dst.reg(0)};

  if (dst.hasReg(1)) {
    auto const sf = v.makeReg();
    auto const intb = v.cns(KindOfInt64);
    auto const strb = v.cns(KindOfString);
    auto const mask = safe_cast<int32_t>(MonotypeDict::intKeyMask().raw);
    auto const layout = rarr[ArrayData::offsetOfBespokeIndex()];
    v << testwim{mask, layout, sf};
    v << cmovb{CC_Z, sf, intb, strb, dst.reg(1)};
  }
}

void cgLdMonotypeDictVal(IRLS& env, const IRInstruction* inst) {
  auto const rarr = srcLoc(env, inst, 0).reg();
  auto const rpos = srcLoc(env, inst, 1).reg();
  auto const pos = inst->src(1)->type();
  auto const dst = dstLoc(env, inst, 0);

  auto& v = vmain(env);
  auto const off = MonotypeDict::elmValOffset();
  auto const ptr = ptrToMonotypeDictElm(v, rarr, rpos, pos, off);
  v << load{ptr, dst.reg(0)};

  if (dst.hasReg(1)) {
    v << loadb{rarr[MonotypeDict::typeOffset()], dst.reg(1)};
  }
}

void cgLdMonotypeVecElem(IRLS& env, const IRInstruction* inst) {
  auto const rarr = srcLoc(env, inst, 0).reg();
  auto const ridx = srcLoc(env, inst, 1).reg();
  auto const idx = inst->src(1);

  auto const type = rarr[bespoke::MonotypeVec::typeOffset()];
  auto const value = [&] {
    auto const base = bespoke::MonotypeVec::entriesOffset();
    if (idx->hasConstVal()) {
      auto const offset = idx->intVal() * sizeof(Value) + base;
      if (deltaFits(offset, sz::dword)) return rarr[offset];
    }
    return rarr[ridx * int(sizeof(Value)) + base];
  }();

  loadTV(vmain(env), inst->dst()->type(), dstLoc(env, inst, 0),
         type, value);
}

//////////////////////////////////////////////////////////////////////////////
// StructDict

namespace {

using bespoke::StructDict;
using bespoke::StructLayout;

// Returns none if the layout is an abstract struct layout.
Optional<Slot> getStructSlot(const SSATmp* arr, const SSATmp* key) {
  assertx(key->hasConstVal(TStr));
  auto const layout = arr->type().arrSpec().layout();
  assertx(layout.is_struct());

  if (!layout.bespokeLayout()->isConcrete()) return std::nullopt;

  auto const slayout = StructLayout::As(layout.bespokeLayout());
  return slayout->keySlot(key->strVal());
}

}

void cgAllocBespokeStructDict(IRLS& env, const IRInstruction* inst) {
  auto const extra = inst->extra<AllocBespokeStructDict>();
  auto& v = vmain(env);

  auto const layout = StructLayout::As(extra->layout.bespokeLayout());
  auto const target = CallSpec::direct(StructDict::AllocStructDict);
  auto const args = argGroup(env, inst)
    .imm(layout->sizeIndex())
    .imm(layout->extraInitializer());

  cgCallHelper(v, env, target, callDest(env, inst), SyncOptions::None, args);
}

void cgInitStructPositions(IRLS& env, const IRInstruction* inst) {
  auto const arr = inst->src(0);
  auto const rarr = srcLoc(env, inst, 0).reg();
  auto const extra = inst->extra<InitStructPositions>();
  auto& v = vmain(env);

  v << storel{v.cns(extra->numSlots), rarr[ArrayData::offsetofSize()]};

  auto const layout = arr->type().arrSpec().layout();
  auto const slayout = StructLayout::As(layout.bespokeLayout());

  auto constexpr kSlotsPerStore = 8;
  auto const size = extra->numSlots;
  auto const padBytes = slayout->positionOffset() & 0x7;
  for (auto i = 0; i < size + padBytes; i += kSlotsPerStore) {
    uint64_t slots = 0;
    for (auto j = 0; j < kSlotsPerStore; j++) {
      // The type array comes before the positions array, and the types should
      // stay initialized with KindOfUninit. Later positions can be zeroed.
      auto const index = static_cast<int32_t>(i + j - padBytes);
      auto const slot = [&]{
        if (index < 0) return static_cast<uint8_t>(KindOfUninit);
        if (index < size) return safe_cast<uint8_t>(extra->slots[index]);
        return static_cast<uint8_t>(0);
      }();
      slots = slots | (safe_cast<uint64_t>(slot) << (j * 8));
    }
    auto const offset = slayout->positionOffset() + i - padBytes;
    assertx((offset % kSlotsPerStore) == 0);
    v << store{v.cns(slots), rarr[offset]};
  }
}

void cgInitStructElem(IRLS& env, const IRInstruction* inst) {
  auto const arr = inst->src(0);
  auto const val = inst->src(1);
  auto const layout = arr->type().arrSpec().layout();
  auto const slayout = StructLayout::As(layout.bespokeLayout());
  auto const slot = inst->extra<InitStructElem>()->index;
  always_assert(val->isA(slayout->getTypeBound(slot)));

  auto const rarr = srcLoc(env, inst, 0).reg();
  auto const type = rarr[slayout->typeOffsetForSlot(slot)];
  auto const data = rarr[slayout->valueOffsetForSlot(slot)];
  storeTV(vmain(env), val->type(), srcLoc(env, inst, 1), type, data);
}

void cgNewBespokeStructDict(IRLS& env, const IRInstruction* inst) {
  auto const sp = srcLoc(env, inst, 0).reg();
  auto const extra = inst->extra<NewBespokeStructDict>();
  auto& v = vmain(env);

  auto const n = static_cast<size_t>((extra->numSlots + 7) & ~7);
  auto const slots = reinterpret_cast<uint8_t*>(v.allocData<uint64_t>(n / 8));
  for (auto i = 0; i < extra->numSlots; i++) {
    slots[i] = safe_cast<uint8_t>(extra->slots[i]);
  }
  for (auto i = extra->numSlots; i < n; i++) {
    slots[i] = static_cast<uint8_t>(KindOfUninit);
  }

  auto const layout = StructLayout::As(extra->layout.bespokeLayout());
  auto const target = CallSpec::direct(StructDict::MakeStructDict);
  auto const args = argGroup(env, inst)
    .imm(layout->sizeIndex())
    .imm(layout->extraInitializer())
    .imm(extra->numSlots)
    .dataPtr(slots)
    .addr(sp, cellsToBytes(extra->offset.offset));

  cgCallHelper(v, env, target, callDest(env, inst), SyncOptions::None, args);
}

void cgStructDictSlot(IRLS& env, const IRInstruction* inst) {
  auto const arr = inst->src(0);
  auto const key = inst->src(1);

  auto const rarr = srcLoc(env, inst, 0).reg();
  auto const rkey = srcLoc(env, inst, 1).reg();
  auto const dst = dstLoc(env, inst, 0).reg();

  auto& v = vmain(env);

  auto const& layout = arr->type().arrSpec().layout();
  assertx(layout.is_struct());

  // If we know both the layout and the key, we can calculate this as
  // a constant.
  if (key->hasConstVal(TStr)) {
    if (auto const slot = getStructSlot(arr, key)) {
      if (*slot == kInvalidSlot) {
        v << jmp{label(env, inst->taken())};
      } else {
        v << copy{v.cns(*slot), dst};
      }
      return;
    }
  }

  // Handle the case where the key definitely isn't a static
  // string. In this case, we rely on C++ to map the key to slot.
  auto const nonStaticCase = [&] (Vout& v) {
    auto const slot = v.makeReg();
    if (layout.bespokeLayout()->isConcrete()) {
      auto const slayout = StructLayout::As(layout.bespokeLayout());
      Slot(StructLayout::*fn)(const StringData*) const =
        &StructLayout::keySlotNonStatic;
      auto const target = CallSpec::method(fn);
      auto const args = argGroup(env, inst).immPtr(slayout).ssa(1);
      cgCallHelper(v, env, target, callDest(slot), SyncOptions::None, args);
    } else {
      auto const layoutIndex = v.makeReg();
      v << loadzwq{rarr[ArrayData::offsetOfBespokeIndex()], layoutIndex};
      Slot(*fn)(bespoke::LayoutIndex, const StringData*) =
        &StructLayout::keySlotNonStatic;
      auto const target = CallSpec::direct(fn);
      auto const args = argGroup(env, inst).reg(layoutIndex).ssa(1);
      cgCallHelper(v, env, target, callDest(slot), SyncOptions::None, args);
    }

    static_assert(sizeof(Slot) == 4);
    auto const sf = v.makeReg();
    v << cmpli{int32_t(kInvalidSlot), slot, sf};
    fwdJcc(v, env, CC_E, sf, inst->taken());
    return slot;
  };

  // Is the key certain to be non-static? If so, invoke the non-static
  // helper.
  if (!key->type().maybe(TStaticStr)) {
    auto const slot = nonStaticCase(v);
    v << copy{slot, dst};
    return;
  }

  // Calculate pointers to the hash table string and slot value, based
  // on what we know about the layout and key.
  auto const pair = [&] {
    using PerfectHashEntry = StructLayout::PerfectHashEntry;
    auto constexpr layoutMask = StructLayout::kMaxColor;
    auto constexpr hashEntrySize = sizeof(PerfectHashEntry);

    auto constexpr strHashOffset  = offsetof(PerfectHashEntry, str);
    auto constexpr slotHashOffset = offsetof(PerfectHashEntry, slot);

    // Read the string's color. This may be junk if the string is
    // non-static.
    auto const color = [&] {
      auto const colorMasked = [&] {
        static_assert(folly::isPowTwo(layoutMask + 1));
        static_assert(layoutMask <= std::numeric_limits<uint8_t>::max());
        auto const colorPremask = v.makeReg();
        v << loadzbq{rkey[StringData::colorOffset()], colorPremask};
        if (layoutMask == std::numeric_limits<uint8_t>::max()) {
          return colorPremask;
        } else {
          auto const colorMasked = v.makeReg();
          v << andqi{
            uint8_t(layoutMask), colorPremask, colorMasked, v.makeReg()
          };
          return colorMasked;
        }
      }();

      static_assert(hashEntrySize == 8 || hashEntrySize == 16);
      if constexpr (hashEntrySize == 16) {
        // Without lowptr enabled, we have to use a 16-byte stride.
        auto const colorFinal = v.makeReg();
        v << lea{colorMasked[colorMasked], colorFinal};
        return colorFinal;
      } else {
        return colorMasked;
      }
    };

    if (layout.bespokeLayout()->isConcrete()) {
      auto const hashTable = StructLayout::hashTable(layout.bespokeLayout());
      auto const base = v.cns(uintptr_t(hashTable));
      auto const c = color();
      return std::make_pair(
        base[c * 8 + slotHashOffset],
        base[c * 8 + strHashOffset]
      );
    }

    auto const hashTableSet = (uintptr_t) StructLayout::hashTableSet();
    auto const layoutIndex = v.makeReg();
    v << loadzwq{rarr[ArrayData::offsetOfBespokeIndex()], layoutIndex};

    auto constexpr hashTableSize = sizeof(StructLayout::PerfectHashTable);
    static_assert(folly::isPowTwo(hashTableSize));
    auto const layoutShift =
      safe_cast<uint8_t>(folly::findLastSet(hashTableSize) - 1);

    auto const hashTableOffset = v.makeReg();
    v << shlqi{layoutShift, layoutIndex, hashTableOffset, v.makeReg()};

    if (key->hasConstVal(TStr)) {
      auto const offWithColor =
        (key->strVal()->color() & layoutMask) * hashEntrySize;
      return std::make_pair(
        hashTableOffset[v.cns(hashTableSet) + (offWithColor + slotHashOffset)],
        hashTableOffset[v.cns(hashTableSet) + (offWithColor + strHashOffset)]
      );
    }

    auto const base = v.makeReg();
    v << lea{hashTableOffset[v.cns(hashTableSet)], base};
    auto const c = color();
    return std::make_pair(
      base[c * 8 + slotHashOffset],
      base[c * 8 + strHashOffset]
    );
  }();
  auto const slotPtr = pair.first;
  auto const strPtr = pair.second;

  auto const hashStr = v.makeReg();
  auto const hashSF = v.makeReg();
  // Load the string in the hash table and check if it matches the
  // key.
  emitLdLowPtr(v, strPtr, hashStr, sizeof(LowStringPtr));
  v << cmpq{rkey, hashStr, hashSF};

  // If the key is definitely static, we're done. The key is present
  // if and only if they match.
  if (key->isA(TStaticStr)) {
    fwdJcc(v, env, CC_NE, hashSF, inst->taken());
    v << loadzbq{slotPtr, dst};
    return;
  }

  // Otherwise the the key could be non-static. If they match, we're
  // good. Otherwise, check if the string is static. If it is, then we
  // definitely don't have a match. If it's non-static, evoke a C++
  // helper to do the lookup.
  cond(
    v, vcold(env), CC_E, hashSF, dst,
    [&] (Vout& v) {
      auto const slot = v.makeReg();
      v << loadzbq{slotPtr, slot};
      return slot;
    },
    [&] (Vout& v) {
      auto const refSF = emitCmpRefCount(v, StaticValue, rkey);
      fwdJcc(v, env, CC_E, refSF, inst->taken());
      return nonStaticCase(v);
    },
    StringTag{}
  );
}

void cgStructDictElemAddr(IRLS& env, const IRInstruction* inst) {
  auto const arr   = inst->src(0);
  auto const slot  = inst->src(2);
  auto const rarr  = srcLoc(env, inst, 0).reg();
  auto const rslot = srcLoc(env, inst, 2).reg();
  auto const dst   = dstLoc(env, inst, 0);
  auto& v = vmain(env);

  auto const& layout = arr->type().arrSpec().layout();
  assertx(layout.is_struct());

  if (layout.bespokeLayout()->isConcrete()) {
    auto const slayout = StructLayout::As(layout.bespokeLayout());

    if (slot->hasConstVal(TInt)) {
      v << lea{
        rarr[slayout->valueOffsetForSlot(slot->intVal())],
        dst.reg(tv_lval::val_idx)
      };
      v << lea{
        rarr[slayout->typeOffsetForSlot(slot->intVal())],
        dst.reg(tv_lval::type_idx)
      };
      return;
    }

    auto const valBegin = slayout->valueOffset();
    v << lea{
      rarr[rslot * safe_cast<int>(sizeof(Value)) + valBegin],
      dst.reg(tv_lval::val_idx)
    };
  } else {
    static_assert(StructDict::valueOffsetSize() == 1);
    auto const valBegin = v.makeReg();
    auto const valIdx = v.makeReg();
    v << loadzbq{rarr[StructDict::valueOffsetOffset()], valBegin};
    v << addq{valBegin, rslot, valIdx, v.makeReg()};
    v << lea{
      rarr[valIdx * safe_cast<int>(sizeof(Value))],
      dst.reg(tv_lval::val_idx)
    };
  }

  v << lea{
    rarr[rslot * safe_cast<int>(sizeof(DataType))
         + StructLayout::staticTypeOffset()],
    dst.reg(tv_lval::type_idx)
  };
}

void cgStructDictTypeBoundCheck(IRLS& env, const IRInstruction* inst) {
  auto const val = inst->src(0);
  auto const arr = inst->src(1);

  auto const rarr  = srcLoc(env, inst, 1).reg();
  auto const rslot = srcLoc(env, inst, 2).reg();

  auto const valLoc = srcLoc(env, inst, 0);
  auto const dst    = dstLoc(env, inst, 0);

  auto& v = vmain(env);

  auto const layout = [&] {
    auto const& layout = arr->type().arrSpec().layout();
    assertx(layout.is_struct());
    if (layout.bespokeLayout()->isConcrete()) {
      auto const slayout = StructLayout::As(layout.bespokeLayout());
      return v.cns((uintptr_t)slayout);
    } else {
      auto const layoutIndex = v.makeReg();
      auto const layout = v.makeReg();
      auto const layouts = v.cns((uintptr_t)bespoke::layoutsForJIT());
      v << loadzwq{rarr[ArrayData::offsetOfBespokeIndex()], layoutIndex};
      v << load{
        layouts[layoutIndex * safe_cast<int>(sizeof(bespoke::Layout*))],
        layout
      };
      return layout;
    }
  }();

  static_assert(StructLayout::Field::typeMaskSize() == 1);
  static_assert(sizeof(StructLayout::Field) == 8 ||
                sizeof(StructLayout::Field) == 16);
  auto const adjustedSlot = [&] {
    if (sizeof(StructLayout::Field) == 16) {
      auto const doubled = v.makeReg();
      v << addq{rslot, rslot, doubled, v.makeReg()};
      return doubled;
    }
    return rslot;
  }();

  auto const typeMask = v.makeReg();
  v << loadb{
    layout[adjustedSlot * 8 +
           StructLayout::Field::typeMaskOffset() +
           StructLayout::fieldsOffset()],
    typeMask
  };

  auto const sf = v.makeReg();
  if (val->type().isKnownDataType()) {
    auto const dataType = val->type().toDataType();
    v << testbi{static_cast<int8_t>(dataType), typeMask, sf};
  } else {
    auto const rtype = valLoc.reg(1);
    v << testb{rtype, typeMask, sf};
  }

  fwdJcc(v, env, CC_NE, sf, inst->taken());
  copyTV(v, valLoc, dst, inst->dst()->type());
}

void cgStructDictAddNextSlot(IRLS& env, const IRInstruction* inst) {
  auto const arr   = inst->src(0);
  auto const rarr  = srcLoc(env, inst, 0).reg();
  auto const rslot = srcLoc(env, inst, 1).reg();
  auto& v = vmain(env);

  auto const& layout = arr->type().arrSpec().layout();
  assertx(layout.is_struct());

  static_assert(ArrayData::sizeofSize() == 4);
  static_assert(StructDict::numFieldsSize() == 1);

  auto const size = v.makeReg();
  v << loadzlq{rarr[ArrayData::offsetofSize()], size};

  if (layout.bespokeLayout()->isConcrete()) {
    auto const slayout = StructLayout::As(layout.bespokeLayout());
    auto const smallSlot = v.makeReg();
    v << movtqb{rslot, smallSlot};
    v << storeb{smallSlot, rarr[size + slayout->positionOffset()]};
  } else {
    auto const numFields = v.makeReg();
    auto const positionsOffset = v.makeReg();
    v << loadzbq{rarr[StructDict::numFieldsOffset()], numFields};
    v << addq{size, numFields, positionsOffset, v.makeReg()};
    auto const smallSlot = v.makeReg();
    v << movtqb{rslot, smallSlot};
    v << storeb{smallSlot, rarr[positionsOffset + sizeof(StructDict)]};
  }

  auto const newSize = v.makeReg();
  auto const truncatedSize = v.makeReg();
  v << incq{size, newSize, v.makeReg()};
  v << movtql{newSize, truncatedSize};
  v << storel{truncatedSize, rarr[ArrayData::offsetofSize()]};
}

void cgStructDictUnset(IRLS& env, const IRInstruction* inst) {
  auto const arr = inst->src(0);
  auto const key = inst->src(1);
  auto const slot = getStructSlot(arr, key);

  if (!slot) return cgBespokeUnset(env, inst);

  auto& v = vmain(env);
  if (*slot == kInvalidSlot) {
    v << copy{srcLoc(env, inst, 0).reg(), dstLoc(env, inst, 0).reg()};
    return;
  }

  // Removing a required field escalates, so we don't JIT a fast path for it.
  auto const layout = arr->type().arrSpec().layout();
  auto const slayout = StructLayout::As(layout.bespokeLayout());
  if (slayout->field(*slot).required) return cgBespokeUnset(env, inst);

  auto const target = CallSpec::direct(StructDict::RemoveStrInSlot);
  auto const args = argGroup(env, inst).ssa(0).imm(*slot);
  cgCallHelper(v, env, target, callDest(env, inst), SyncOptions::None, args);
}

//////////////////////////////////////////////////////////////////////////////

}}}
