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

#include "hphp/runtime/vm/jit/irlower-internal.h"

#include "hphp/runtime/base/attr.h"
#include "hphp/runtime/base/runtime-option.h"
#include "hphp/runtime/vm/func.h"

#include "hphp/runtime/vm/jit/types.h"
#include "hphp/runtime/vm/jit/abi.h"
#include "hphp/runtime/vm/jit/analysis.h"
#include "hphp/runtime/vm/jit/dce.h"
#include "hphp/runtime/vm/jit/code-gen-helpers.h"
#include "hphp/runtime/vm/jit/extra-data.h"
#include "hphp/runtime/vm/jit/ir-instruction.h"
#include "hphp/runtime/vm/jit/ir-opcode.h"
#include "hphp/runtime/vm/jit/ssa-tmp.h"
#include "hphp/runtime/vm/jit/tc.h"
#include "hphp/runtime/vm/jit/translator-inline.h"
#include "hphp/runtime/vm/jit/unique-stubs.h"
#include "hphp/runtime/vm/jit/vasm-gen.h"
#include "hphp/runtime/vm/jit/vasm-instr.h"
#include "hphp/runtime/vm/jit/vasm-reg.h"

#include "hphp/util/trace.h"

namespace HPHP { namespace jit { namespace irlower {

TRACE_SET_MOD(irlower);

///////////////////////////////////////////////////////////////////////////////

void cgBeginInlining(IRLS& env, const IRInstruction* inst) {
  auto& v = vmain(env);
  auto const extra = inst->extra<BeginInlining>();
  v << inlinestart{extra->func, extra->cost};
}

void cgDefInlineFP(IRLS& env, const IRInstruction* inst) {
  auto const extra = inst->extra<DefInlineFP>();
  auto const callerSP = srcLoc(env, inst, 0).reg();
  auto const callerFP = srcLoc(env, inst, 1).reg();
  auto& v = vmain(env);

  auto const ar = callerSP[cellsToBytes(extra->spOffset.offset)];

  // Do roughly the same work as an HHIR Call.
  v << store{callerFP, ar + AROFF(m_sfp)};
  emitImmStoreq(v, uintptr_t(tc::ustubs().retInlHelper),
                ar + AROFF(m_savedRip));
  v << storeli{extra->callBCOff, ar + AROFF(m_callOff)};
  if (extra->target->attrs() & AttrMayUseVV) {
    v << storeqi{0, ar + AROFF(m_invName)};
  }
  if (extra->asyncEagerReturn) {
    v << orlim{
      static_cast<int32_t>(ActRec::Flags::AsyncEagerRet),
      ar + AROFF(m_numArgsAndFlags),
      v.makeReg()
    };
  }

  v << pushframe{};
  v << lea{ar, dstLoc(env, inst, 0).reg()};
}

namespace {

bool isResumedParent(const IRInstruction* inst) {
  auto const fp = inst->src(0);
  assertx(canonical(fp)->inst()->is(DefInlineFP, DefLabel));

  auto const chaseFpTmp = [](const SSATmp* s) {
    s = canonical(s);
    auto i = s->inst();
    if (UNLIKELY(i->is(DefLabel))) {
      i = resolveFpDefLabel(s);
      assertx(i);
    }
    always_assert(i->is(DefFP, DefInlineFP));
    return i->dst();
  };

  auto const calleeFp = chaseFpTmp(fp);
  assertx(calleeFp->inst()->is(DefInlineFP));

  auto const callerFp = calleeFp->inst()->src(1);
  return callerFp->inst()->marker().resumeMode() != ResumeMode::None;
}

}

void cgInlineReturn(IRLS& env, const IRInstruction* inst) {
  auto& v = vmain(env);
  auto const fp = srcLoc(env, inst, 0).reg();
  if (isResumedParent(inst)) {
    v << load{fp[AROFF(m_sfp)], rvmfp()};
  } else {
    auto const callerFPOff = inst->extra<InlineReturn>()->offset;
    v << lea{fp[cellsToBytes(callerFPOff.offset)], rvmfp()};
  }
  v << popframe{};
  v << inlineend{};
}

void cgInlineSuspend(IRLS& env, const IRInstruction* inst) {
  auto& v = vmain(env);
  auto const fp = srcLoc(env, inst, 0).reg();
  if (isResumedParent(inst)) {
    v << load{fp[AROFF(m_sfp)], rvmfp()};
  } else {
    auto const callerFPOff = inst->extra<InlineSuspend>()->offset;
    v << lea{fp[cellsToBytes(callerFPOff.offset)], rvmfp()};
  }
  v << popframe{};
  v << inlineend{};
}

void cgInlineReturnNoFrame(IRLS& env, const IRInstruction* inst) {
  auto& v = vmain(env);

  if (RuntimeOption::EvalHHIRGenerateAsserts) {
    if (env.unit.context().initSrcKey.resumeMode() == ResumeMode::None) {
      auto const extra = inst->extra<InlineReturnNoFrame>();
      auto const offset = cellsToBytes(extra->offset.offset);
      for (auto i = 0; i < kNumActRecCells; ++i) {
        trashFullTV(v, rvmfp()[offset - cellsToBytes(i)], kTVTrashJITFrame);
      }
    }
  }

  v << inlineend{};
}

void cgSyncReturnBC(IRLS& env, const IRInstruction* inst) {
  auto const extra = inst->extra<SyncReturnBC>();
  auto const spOffset = cellsToBytes(extra->spOffset.offset);
  auto const callBCOffset = safe_cast<int32_t>(extra->callBCOffset);
  auto const sp = srcLoc(env, inst, 0).reg();
  auto const fp = srcLoc(env, inst, 1).reg();

  auto& v = vmain(env);
  v << storeli{callBCOffset, sp[spOffset + AROFF(m_callOff)]};
  v << store{fp, sp[spOffset + AROFF(m_sfp)]};
}

///////////////////////////////////////////////////////////////////////////////

void cgConjure(IRLS& env, const IRInstruction* inst) {
  auto const dst = dstLoc(env, inst, 0);
  auto& v = vmain(env);
  if (dst.hasReg(0)) {
    v << conjure{dst.reg(0)};
  }
  if (dst.hasReg(1)) {
    v << conjure{dst.reg(1)};
  }
}

void cgConjureUse(IRLS& env, const IRInstruction* inst) {
  auto const src = srcLoc(env, inst, 0);
  auto& v = vmain(env);
  if (src.hasReg(0)) {
    v << conjureuse{src.reg(0)};
  }
  if (src.hasReg(1)) {
    v << conjureuse{src.reg(1)};
  }
}

///////////////////////////////////////////////////////////////////////////////

}}}
