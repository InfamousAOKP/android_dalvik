/*
 * This file was generated automatically by gen-mterp.py for 'portdbg'.
 *
 * --> DO NOT EDIT <--
 */

/* File: c/header.c */
/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* common includes */
#include "Dalvik.h"
#include "interp/InterpDefs.h"
#include "mterp/Mterp.h"
#include <math.h>                   // needed for fmod, fmodf
#include "mterp/common/FindInterface.h"

/*
 * Configuration defines.  These affect the C implementations, i.e. the
 * portable interpreter(s) and C stubs.
 *
 * Some defines are controlled by the Makefile, e.g.:
 *   WITH_INSTR_CHECKS
 *   WITH_TRACKREF_CHECKS
 *   EASY_GDB
 *   NDEBUG
 *
 * If THREADED_INTERP is not defined, we use a classic "while true / switch"
 * interpreter.  If it is defined, then the tail end of each instruction
 * handler fetches the next instruction and jumps directly to the handler.
 * This increases the size of the "Std" interpreter by about 10%, but
 * provides a speedup of about the same magnitude.
 *
 * There's a "hybrid" approach that uses a goto table instead of a switch
 * statement, avoiding the "is the opcode in range" tests required for switch.
 * The performance is close to the threaded version, and without the 10%
 * size increase, but the benchmark results are off enough that it's not
 * worth adding as a third option.
 */
#define THREADED_INTERP             /* threaded vs. while-loop interpreter */

#ifdef WITH_INSTR_CHECKS            /* instruction-level paranoia (slow!) */
# define CHECK_BRANCH_OFFSETS
# define CHECK_REGISTER_INDICES
#endif

/*
 * ARM EABI requires 64-bit alignment for access to 64-bit data types.  We
 * can't just use pointers to copy 64-bit values out of our interpreted
 * register set, because gcc will generate ldrd/strd.
 *
 * The __UNION version copies data in and out of a union.  The __MEMCPY
 * version uses a memcpy() call to do the transfer; gcc is smart enough to
 * not actually call memcpy().  The __UNION version is very bad on ARM;
 * it only uses one more instruction than __MEMCPY, but for some reason
 * gcc thinks it needs separate storage for every instance of the union.
 * On top of that, it feels the need to zero them out at the start of the
 * method.  Net result is we zero out ~700 bytes of stack space at the top
 * of the interpreter using ARM STM instructions.
 */
#if defined(__ARM_EABI__)
//# define NO_UNALIGN_64__UNION
# define NO_UNALIGN_64__MEMCPY
#endif

//#define LOG_INSTR                   /* verbose debugging */
/* set and adjust ANDROID_LOG_TAGS='*:i jdwp:i dalvikvm:i dalvikvmi:i' */

/*
 * Keep a tally of accesses to fields.  Currently only works if full DEX
 * optimization is disabled.
 */
#ifdef PROFILE_FIELD_ACCESS
# define UPDATE_FIELD_GET(_field) { (_field)->gets++; }
# define UPDATE_FIELD_PUT(_field) { (_field)->puts++; }
#else
# define UPDATE_FIELD_GET(_field) ((void)0)
# define UPDATE_FIELD_PUT(_field) ((void)0)
#endif

/*
 * Export another copy of the PC on every instruction; this is largely
 * redundant with EXPORT_PC and the debugger code.  This value can be
 * compared against what we have stored on the stack with EXPORT_PC to
 * help ensure that we aren't missing any export calls.
 */
#if WITH_EXTRA_GC_CHECKS > 1
# define EXPORT_EXTRA_PC() (self->currentPc2 = pc)
#else
# define EXPORT_EXTRA_PC()
#endif

/*
 * Adjust the program counter.  "_offset" is a signed int, in 16-bit units.
 *
 * Assumes the existence of "const u2* pc" and "const u2* curMethod->insns".
 *
 * We don't advance the program counter until we finish an instruction or
 * branch, because we do want to have to unroll the PC if there's an
 * exception.
 */
#ifdef CHECK_BRANCH_OFFSETS
# define ADJUST_PC(_offset) do {                                            \
        int myoff = _offset;        /* deref only once */                   \
        if (pc + myoff < curMethod->insns ||                                \
            pc + myoff >= curMethod->insns + dvmGetMethodInsnsSize(curMethod)) \
        {                                                                   \
            char* desc;                                                     \
            desc = dexProtoCopyMethodDescriptor(&curMethod->prototype);     \
            LOGE("Invalid branch %d at 0x%04x in %s.%s %s\n",               \
                myoff, (int) (pc - curMethod->insns),                       \
                curMethod->clazz->descriptor, curMethod->name, desc);       \
            free(desc);                                                     \
            dvmAbort();                                                     \
        }                                                                   \
        pc += myoff;                                                        \
        EXPORT_EXTRA_PC();                                                  \
    } while (false)
#else
# define ADJUST_PC(_offset) do {                                            \
        pc += _offset;                                                      \
        EXPORT_EXTRA_PC();                                                  \
    } while (false)
#endif

/*
 * If enabled, log instructions as we execute them.
 */
#ifdef LOG_INSTR
# define ILOGD(...) ILOG(LOG_DEBUG, __VA_ARGS__)
# define ILOGV(...) ILOG(LOG_VERBOSE, __VA_ARGS__)
# define ILOG(_level, ...) do {                                             \
        char debugStrBuf[128];                                              \
        snprintf(debugStrBuf, sizeof(debugStrBuf), __VA_ARGS__);            \
        if (curMethod != NULL)                                                 \
            LOG(_level, LOG_TAG"i", "%-2d|%04x%s\n",                        \
                self->threadId, (int)(pc - curMethod->insns), debugStrBuf); \
        else                                                                \
            LOG(_level, LOG_TAG"i", "%-2d|####%s\n",                        \
                self->threadId, debugStrBuf);                               \
    } while(false)
void dvmDumpRegs(const Method* method, const u4* framePtr, bool inOnly);
# define DUMP_REGS(_meth, _frame, _inOnly) dvmDumpRegs(_meth, _frame, _inOnly)
static const char kSpacing[] = "            ";
#else
# define ILOGD(...) ((void)0)
# define ILOGV(...) ((void)0)
# define DUMP_REGS(_meth, _frame, _inOnly) ((void)0)
#endif

/* get a long from an array of u4 */
static inline s8 getLongFromArray(const u4* ptr, int idx)
{
#if defined(NO_UNALIGN_64__UNION)
    union { s8 ll; u4 parts[2]; } conv;

    ptr += idx;
    conv.parts[0] = ptr[0];
    conv.parts[1] = ptr[1];
    return conv.ll;
#elif defined(NO_UNALIGN_64__MEMCPY)
    s8 val;
    memcpy(&val, &ptr[idx], 8);
    return val;
#else
    return *((s8*) &ptr[idx]);
#endif
}

/* store a long into an array of u4 */
static inline void putLongToArray(u4* ptr, int idx, s8 val)
{
#if defined(NO_UNALIGN_64__UNION)
    union { s8 ll; u4 parts[2]; } conv;

    ptr += idx;
    conv.ll = val;
    ptr[0] = conv.parts[0];
    ptr[1] = conv.parts[1];
#elif defined(NO_UNALIGN_64__MEMCPY)
    memcpy(&ptr[idx], &val, 8);
#else
    *((s8*) &ptr[idx]) = val;
#endif
}

/* get a double from an array of u4 */
static inline double getDoubleFromArray(const u4* ptr, int idx)
{
#if defined(NO_UNALIGN_64__UNION)
    union { double d; u4 parts[2]; } conv;

    ptr += idx;
    conv.parts[0] = ptr[0];
    conv.parts[1] = ptr[1];
    return conv.d;
#elif defined(NO_UNALIGN_64__MEMCPY)
    double dval;
    memcpy(&dval, &ptr[idx], 8);
    return dval;
#else
    return *((double*) &ptr[idx]);
#endif
}

/* store a double into an array of u4 */
static inline void putDoubleToArray(u4* ptr, int idx, double dval)
{
#if defined(NO_UNALIGN_64__UNION)
    union { double d; u4 parts[2]; } conv;

    ptr += idx;
    conv.d = dval;
    ptr[0] = conv.parts[0];
    ptr[1] = conv.parts[1];
#elif defined(NO_UNALIGN_64__MEMCPY)
    memcpy(&ptr[idx], &dval, 8);
#else
    *((double*) &ptr[idx]) = dval;
#endif
}

/*
 * If enabled, validate the register number on every access.  Otherwise,
 * just do an array access.
 *
 * Assumes the existence of "u4* fp".
 *
 * "_idx" may be referenced more than once.
 */
#ifdef CHECK_REGISTER_INDICES
# define GET_REGISTER(_idx) \
    ( (_idx) < curMethod->registersSize ? \
        (fp[(_idx)]) : (assert(!"bad reg"),1969) )
# define SET_REGISTER(_idx, _val) \
    ( (_idx) < curMethod->registersSize ? \
        (fp[(_idx)] = (u4)(_val)) : (assert(!"bad reg"),1969) )
# define GET_REGISTER_AS_OBJECT(_idx)       ((Object *)GET_REGISTER(_idx))
# define SET_REGISTER_AS_OBJECT(_idx, _val) SET_REGISTER(_idx, (s4)_val)
# define GET_REGISTER_INT(_idx) ((s4) GET_REGISTER(_idx))
# define SET_REGISTER_INT(_idx, _val) SET_REGISTER(_idx, (s4)_val)
# define GET_REGISTER_WIDE(_idx) \
    ( (_idx) < curMethod->registersSize-1 ? \
        getLongFromArray(fp, (_idx)) : (assert(!"bad reg"),1969) )
# define SET_REGISTER_WIDE(_idx, _val) \
    ( (_idx) < curMethod->registersSize-1 ? \
        putLongToArray(fp, (_idx), (_val)) : (assert(!"bad reg"),1969) )
# define GET_REGISTER_FLOAT(_idx) \
    ( (_idx) < curMethod->registersSize ? \
        (*((float*) &fp[(_idx)])) : (assert(!"bad reg"),1969.0f) )
# define SET_REGISTER_FLOAT(_idx, _val) \
    ( (_idx) < curMethod->registersSize ? \
        (*((float*) &fp[(_idx)]) = (_val)) : (assert(!"bad reg"),1969.0f) )
# define GET_REGISTER_DOUBLE(_idx) \
    ( (_idx) < curMethod->registersSize-1 ? \
        getDoubleFromArray(fp, (_idx)) : (assert(!"bad reg"),1969.0) )
# define SET_REGISTER_DOUBLE(_idx, _val) \
    ( (_idx) < curMethod->registersSize-1 ? \
        putDoubleToArray(fp, (_idx), (_val)) : (assert(!"bad reg"),1969.0) )
#else
# define GET_REGISTER(_idx)                 (fp[(_idx)])
# define SET_REGISTER(_idx, _val)           (fp[(_idx)] = (_val))
# define GET_REGISTER_AS_OBJECT(_idx)       ((Object*) fp[(_idx)])
# define SET_REGISTER_AS_OBJECT(_idx, _val) (fp[(_idx)] = (u4)(_val))
# define GET_REGISTER_INT(_idx)             ((s4)GET_REGISTER(_idx))
# define SET_REGISTER_INT(_idx, _val)       SET_REGISTER(_idx, (s4)_val)
# define GET_REGISTER_WIDE(_idx)            getLongFromArray(fp, (_idx))
# define SET_REGISTER_WIDE(_idx, _val)      putLongToArray(fp, (_idx), (_val))
# define GET_REGISTER_FLOAT(_idx)           (*((float*) &fp[(_idx)]))
# define SET_REGISTER_FLOAT(_idx, _val)     (*((float*) &fp[(_idx)]) = (_val))
# define GET_REGISTER_DOUBLE(_idx)          getDoubleFromArray(fp, (_idx))
# define SET_REGISTER_DOUBLE(_idx, _val)    putDoubleToArray(fp, (_idx), (_val))
#endif

/*
 * Get 16 bits from the specified offset of the program counter.  We always
 * want to load 16 bits at a time from the instruction stream -- it's more
 * efficient than 8 and won't have the alignment problems that 32 might.
 *
 * Assumes existence of "const u2* pc".
 */
#define FETCH(_offset)     (pc[(_offset)])

/*
 * Extract instruction byte from 16-bit fetch (_inst is a u2).
 */
#define INST_INST(_inst)    ((_inst) & 0xff)

/*
 * Replace the opcode (used when handling breakpoints).  _opcode is a u1.
 */
#define INST_REPLACE_OP(_inst, _opcode) (((_inst) & 0xff00) | _opcode)

/*
 * Extract the "vA, vB" 4-bit registers from the instruction word (_inst is u2).
 */
#define INST_A(_inst)       (((_inst) >> 8) & 0x0f)
#define INST_B(_inst)       ((_inst) >> 12)

/*
 * Get the 8-bit "vAA" 8-bit register index from the instruction word.
 * (_inst is u2)
 */
#define INST_AA(_inst)      ((_inst) >> 8)

/*
 * The current PC must be available to Throwable constructors, e.g.
 * those created by dvmThrowException(), so that the exception stack
 * trace can be generated correctly.  If we don't do this, the offset
 * within the current method won't be shown correctly.  See the notes
 * in Exception.c.
 *
 * This is also used to determine the address for precise GC.
 *
 * Assumes existence of "u4* fp" and "const u2* pc".
 */
#define EXPORT_PC()         (SAVEAREA_FROM_FP(fp)->xtra.currentPc = pc)

/*
 * Determine if we need to switch to a different interpreter.  "_current"
 * is either INTERP_STD or INTERP_DBG.  It should be fixed for a given
 * interpreter generation file, which should remove the outer conditional
 * from the following.
 *
 * If we're building without debug and profiling support, we never switch.
 */
#if defined(WITH_JIT)
# define NEED_INTERP_SWITCH(_current) (                                     \
    (_current == INTERP_STD) ?                                              \
        dvmJitDebuggerOrProfilerActive() : !dvmJitDebuggerOrProfilerActive() )
#else
# define NEED_INTERP_SWITCH(_current) (                                     \
    (_current == INTERP_STD) ?                                              \
        dvmDebuggerOrProfilerActive() : !dvmDebuggerOrProfilerActive() )
#endif

/*
 * Check to see if "obj" is NULL.  If so, throw an exception.  Assumes the
 * pc has already been exported to the stack.
 *
 * Perform additional checks on debug builds.
 *
 * Use this to check for NULL when the instruction handler calls into
 * something that could throw an exception (so we have already called
 * EXPORT_PC at the top).
 */
static inline bool checkForNull(Object* obj)
{
    if (obj == NULL) {
        dvmThrowException("Ljava/lang/NullPointerException;", NULL);
        return false;
    }
#ifdef WITH_EXTRA_OBJECT_VALIDATION
    if (!dvmIsValidObject(obj)) {
        LOGE("Invalid object %p\n", obj);
        dvmAbort();
    }
#endif
#ifndef NDEBUG
    if (obj->clazz == NULL || ((u4) obj->clazz) <= 65536) {
        /* probable heap corruption */
        LOGE("Invalid object class %p (in %p)\n", obj->clazz, obj);
        dvmAbort();
    }
#endif
    return true;
}

/*
 * Check to see if "obj" is NULL.  If so, export the PC into the stack
 * frame and throw an exception.
 *
 * Perform additional checks on debug builds.
 *
 * Use this to check for NULL when the instruction handler doesn't do
 * anything else that can throw an exception.
 */
static inline bool checkForNullExportPC(Object* obj, u4* fp, const u2* pc)
{
    if (obj == NULL) {
        EXPORT_PC();
        dvmThrowException("Ljava/lang/NullPointerException;", NULL);
        return false;
    }
#ifdef WITH_EXTRA_OBJECT_VALIDATION
    if (!dvmIsValidObject(obj)) {
        LOGE("Invalid object %p\n", obj);
        dvmAbort();
    }
#endif
#ifndef NDEBUG
    if (obj->clazz == NULL || ((u4) obj->clazz) <= 65536) {
        /* probable heap corruption */
        LOGE("Invalid object class %p (in %p)\n", obj->clazz, obj);
        dvmAbort();
    }
#endif
    return true;
}

/* File: portable/portdbg.c */
#define INTERP_FUNC_NAME dvmInterpretDbg
#define INTERP_TYPE INTERP_DBG

#define CHECK_DEBUG_AND_PROF() \
    checkDebugAndProf(pc, fp, self, curMethod, &debugIsMethodEntry)

#if defined(WITH_JIT)
#define CHECK_JIT_BOOL() (dvmCheckJit(pc, self, interpState, callsiteClass,\
                          methodToCall))
#define CHECK_JIT_VOID() (dvmCheckJit(pc, self, interpState, callsiteClass,\
                          methodToCall))
#define END_JIT_TSELECT() (dvmJitEndTraceSelect(interpState))
#else
#define CHECK_JIT_BOOL() (false)
#define CHECK_JIT_VOID()
#define END_JIT_TSELECT(x) ((void)0)
#endif

/* File: portable/stubdefs.c */
/*
 * In the C mterp stubs, "goto" is a function call followed immediately
 * by a return.
 */

#define GOTO_TARGET_DECL(_target, ...)

#define GOTO_TARGET(_target, ...) _target:

#define GOTO_TARGET_END

/* ugh */
#define STUB_HACK(x)

/*
 * Instruction framing.  For a switch-oriented implementation this is
 * case/break, for a threaded implementation it's a goto label and an
 * instruction fetch/computed goto.
 *
 * Assumes the existence of "const u2* pc" and (for threaded operation)
 * "u2 inst".
 *
 * TODO: remove "switch" version.
 */
#ifdef THREADED_INTERP
# define H(_op)             &&op_##_op
# define HANDLE_OPCODE(_op) op_##_op:
# define FINISH(_offset) {                                                  \
        ADJUST_PC(_offset);                                                 \
        inst = FETCH(0);                                                    \
        CHECK_DEBUG_AND_PROF();                                             \
        CHECK_TRACKED_REFS();                                               \
        if (CHECK_JIT_BOOL()) GOTO_bail_switch();                           \
        goto *handlerTable[INST_INST(inst)];                                \
    }
# define FINISH_BKPT(_opcode) {                                             \
        goto *handlerTable[_opcode];                                        \
    }
# define DISPATCH_EXTENDED(_opcode) {                                       \
        goto *handlerTable[0x100 + _opcode];                                \
    }
#else
# define HANDLE_OPCODE(_op) case _op:
# define FINISH(_offset)    { ADJUST_PC(_offset); break; }
# define FINISH_BKPT(opcode) { > not implemented < }
# define DISPATCH_EXTENDED(opcode) goto case (0x100 + opcode);
#endif

#define OP_END

#if defined(WITH_TRACKREF_CHECKS)
# define CHECK_TRACKED_REFS() \
    dvmInterpCheckTrackedRefs(self, curMethod, debugTrackedRefStart)
#else
# define CHECK_TRACKED_REFS() ((void)0)
#endif


/*
 * The "goto" targets just turn into goto statements.  The "arguments" are
 * passed through local variables.
 */

#define GOTO_exceptionThrown() goto exceptionThrown;

#define GOTO_returnFromMethod() goto returnFromMethod;

#define GOTO_invoke(_target, _methodCallRange, _jumboFormat)                \
    do {                                                                    \
        methodCallRange = _methodCallRange;                                 \
        jumboFormat = _jumboFormat;                                         \
        goto _target;                                                       \
    } while(false)

/* for this, the "args" are already in the locals */
#define GOTO_invokeMethod(_methodCallRange, _methodToCall, _vsrc1, _vdst) goto invokeMethod;

#define GOTO_bail() goto bail;
#define GOTO_bail_switch() goto bail_switch;

/*
 * Periodically check for thread suspension.
 *
 * While we're at it, see if a debugger has attached or the profiler has
 * started.  If so, switch to a different "goto" table.
 */
#define PERIODIC_CHECKS(_entryPoint, _pcadj) {                              \
        if (dvmCheckSuspendQuick(self)) {                                   \
            EXPORT_PC();  /* need for precise GC */                         \
            dvmCheckSuspendPending(self);                                   \
        }                                                                   \
        if (NEED_INTERP_SWITCH(INTERP_TYPE)) {                              \
            ADJUST_PC(_pcadj);                                              \
            interpState->entryPoint = _entryPoint;                          \
            LOGVV("threadid=%d: switch to %s ep=%d adj=%d\n",               \
                self->threadId,                                             \
                (interpState->nextMode == INTERP_STD) ? "STD" : "DBG",      \
                (_entryPoint), (_pcadj));                                   \
            GOTO_bail_switch();                                             \
        }                                                                   \
    }

/* File: c/opcommon.c */
/* forward declarations of goto targets */
GOTO_TARGET_DECL(filledNewArray, bool methodCallRange, bool jumboFormat);
GOTO_TARGET_DECL(invokeVirtual, bool methodCallRange, bool jumboFormat);
GOTO_TARGET_DECL(invokeSuper, bool methodCallRange, bool jumboFormat);
GOTO_TARGET_DECL(invokeInterface, bool methodCallRange, bool jumboFormat);
GOTO_TARGET_DECL(invokeDirect, bool methodCallRange, bool jumboFormat);
GOTO_TARGET_DECL(invokeStatic, bool methodCallRange, bool jumboFormat);
GOTO_TARGET_DECL(invokeVirtualQuick, bool methodCallRange, bool jumboFormat);
GOTO_TARGET_DECL(invokeSuperQuick, bool methodCallRange, bool jumboFormat);
GOTO_TARGET_DECL(invokeMethod, bool methodCallRange, const Method* methodToCall,
    u2 count, u2 regs);
GOTO_TARGET_DECL(returnFromMethod);
GOTO_TARGET_DECL(exceptionThrown);

/*
 * ===========================================================================
 *
 * What follows are opcode definitions shared between multiple opcodes with
 * minor substitutions handled by the C pre-processor.  These should probably
 * use the mterp substitution mechanism instead, with the code here moved
 * into common fragment files (like the asm "binop.S"), although it's hard
 * to give up the C preprocessor in favor of the much simpler text subst.
 *
 * ===========================================================================
 */

#define HANDLE_NUMCONV(_opcode, _opname, _fromtype, _totype)                \
    HANDLE_OPCODE(_opcode /*vA, vB*/)                                       \
        vdst = INST_A(inst);                                                \
        vsrc1 = INST_B(inst);                                               \
        ILOGV("|%s v%d,v%d", (_opname), vdst, vsrc1);                       \
        SET_REGISTER##_totype(vdst,                                         \
            GET_REGISTER##_fromtype(vsrc1));                                \
        FINISH(1);

#define HANDLE_FLOAT_TO_INT(_opcode, _opname, _fromvtype, _fromrtype,       \
        _tovtype, _tortype)                                                 \
    HANDLE_OPCODE(_opcode /*vA, vB*/)                                       \
    {                                                                       \
        /* spec defines specific handling for +/- inf and NaN values */     \
        _fromvtype val;                                                     \
        _tovtype intMin, intMax, result;                                    \
        vdst = INST_A(inst);                                                \
        vsrc1 = INST_B(inst);                                               \
        ILOGV("|%s v%d,v%d", (_opname), vdst, vsrc1);                       \
        val = GET_REGISTER##_fromrtype(vsrc1);                              \
        intMin = (_tovtype) 1 << (sizeof(_tovtype) * 8 -1);                 \
        intMax = ~intMin;                                                   \
        result = (_tovtype) val;                                            \
        if (val >= intMax)          /* +inf */                              \
            result = intMax;                                                \
        else if (val <= intMin)     /* -inf */                              \
            result = intMin;                                                \
        else if (val != val)        /* NaN */                               \
            result = 0;                                                     \
        else                                                                \
            result = (_tovtype) val;                                        \
        SET_REGISTER##_tortype(vdst, result);                               \
    }                                                                       \
    FINISH(1);

#define HANDLE_INT_TO_SMALL(_opcode, _opname, _type)                        \
    HANDLE_OPCODE(_opcode /*vA, vB*/)                                       \
        vdst = INST_A(inst);                                                \
        vsrc1 = INST_B(inst);                                               \
        ILOGV("|int-to-%s v%d,v%d", (_opname), vdst, vsrc1);                \
        SET_REGISTER(vdst, (_type) GET_REGISTER(vsrc1));                    \
        FINISH(1);

/* NOTE: the comparison result is always a signed 4-byte integer */
#define HANDLE_OP_CMPX(_opcode, _opname, _varType, _type, _nanVal)          \
    HANDLE_OPCODE(_opcode /*vAA, vBB, vCC*/)                                \
    {                                                                       \
        int result;                                                         \
        u2 regs;                                                            \
        _varType val1, val2;                                                \
        vdst = INST_AA(inst);                                               \
        regs = FETCH(1);                                                    \
        vsrc1 = regs & 0xff;                                                \
        vsrc2 = regs >> 8;                                                  \
        ILOGV("|cmp%s v%d,v%d,v%d", (_opname), vdst, vsrc1, vsrc2);         \
        val1 = GET_REGISTER##_type(vsrc1);                                  \
        val2 = GET_REGISTER##_type(vsrc2);                                  \
        if (val1 == val2)                                                   \
            result = 0;                                                     \
        else if (val1 < val2)                                               \
            result = -1;                                                    \
        else if (val1 > val2)                                               \
            result = 1;                                                     \
        else                                                                \
            result = (_nanVal);                                             \
        ILOGV("+ result=%d\n", result);                                     \
        SET_REGISTER(vdst, result);                                         \
    }                                                                       \
    FINISH(2);

#define HANDLE_OP_IF_XX(_opcode, _opname, _cmp)                             \
    HANDLE_OPCODE(_opcode /*vA, vB, +CCCC*/)                                \
        vsrc1 = INST_A(inst);                                               \
        vsrc2 = INST_B(inst);                                               \
        if ((s4) GET_REGISTER(vsrc1) _cmp (s4) GET_REGISTER(vsrc2)) {       \
            int branchOffset = (s2)FETCH(1);    /* sign-extended */         \
            ILOGV("|if-%s v%d,v%d,+0x%04x", (_opname), vsrc1, vsrc2,        \
                branchOffset);                                              \
            ILOGV("> branch taken");                                        \
            if (branchOffset < 0)                                           \
                PERIODIC_CHECKS(kInterpEntryInstr, branchOffset);           \
            FINISH(branchOffset);                                           \
        } else {                                                            \
            ILOGV("|if-%s v%d,v%d,-", (_opname), vsrc1, vsrc2);             \
            FINISH(2);                                                      \
        }

#define HANDLE_OP_IF_XXZ(_opcode, _opname, _cmp)                            \
    HANDLE_OPCODE(_opcode /*vAA, +BBBB*/)                                   \
        vsrc1 = INST_AA(inst);                                              \
        if ((s4) GET_REGISTER(vsrc1) _cmp 0) {                              \
            int branchOffset = (s2)FETCH(1);    /* sign-extended */         \
            ILOGV("|if-%s v%d,+0x%04x", (_opname), vsrc1, branchOffset);    \
            ILOGV("> branch taken");                                        \
            if (branchOffset < 0)                                           \
                PERIODIC_CHECKS(kInterpEntryInstr, branchOffset);           \
            FINISH(branchOffset);                                           \
        } else {                                                            \
            ILOGV("|if-%s v%d,-", (_opname), vsrc1);                        \
            FINISH(2);                                                      \
        }

#define HANDLE_UNOP(_opcode, _opname, _pfx, _sfx, _type)                    \
    HANDLE_OPCODE(_opcode /*vA, vB*/)                                       \
        vdst = INST_A(inst);                                                \
        vsrc1 = INST_B(inst);                                               \
        ILOGV("|%s v%d,v%d", (_opname), vdst, vsrc1);                       \
        SET_REGISTER##_type(vdst, _pfx GET_REGISTER##_type(vsrc1) _sfx);    \
        FINISH(1);

#define HANDLE_OP_X_INT(_opcode, _opname, _op, _chkdiv)                     \
    HANDLE_OPCODE(_opcode /*vAA, vBB, vCC*/)                                \
    {                                                                       \
        u2 srcRegs;                                                         \
        vdst = INST_AA(inst);                                               \
        srcRegs = FETCH(1);                                                 \
        vsrc1 = srcRegs & 0xff;                                             \
        vsrc2 = srcRegs >> 8;                                               \
        ILOGV("|%s-int v%d,v%d", (_opname), vdst, vsrc1);                   \
        if (_chkdiv != 0) {                                                 \
            s4 firstVal, secondVal, result;                                 \
            firstVal = GET_REGISTER(vsrc1);                                 \
            secondVal = GET_REGISTER(vsrc2);                                \
            if (secondVal == 0) {                                           \
                EXPORT_PC();                                                \
                dvmThrowException("Ljava/lang/ArithmeticException;",        \
                    "divide by zero");                                      \
                GOTO_exceptionThrown();                                     \
            }                                                               \
            if ((u4)firstVal == 0x80000000 && secondVal == -1) {            \
                if (_chkdiv == 1)                                           \
                    result = firstVal;  /* division */                      \
                else                                                        \
                    result = 0;         /* remainder */                     \
            } else {                                                        \
                result = firstVal _op secondVal;                            \
            }                                                               \
            SET_REGISTER(vdst, result);                                     \
        } else {                                                            \
            /* non-div/rem case */                                          \
            SET_REGISTER(vdst,                                              \
                (s4) GET_REGISTER(vsrc1) _op (s4) GET_REGISTER(vsrc2));     \
        }                                                                   \
    }                                                                       \
    FINISH(2);

#define HANDLE_OP_SHX_INT(_opcode, _opname, _cast, _op)                     \
    HANDLE_OPCODE(_opcode /*vAA, vBB, vCC*/)                                \
    {                                                                       \
        u2 srcRegs;                                                         \
        vdst = INST_AA(inst);                                               \
        srcRegs = FETCH(1);                                                 \
        vsrc1 = srcRegs & 0xff;                                             \
        vsrc2 = srcRegs >> 8;                                               \
        ILOGV("|%s-int v%d,v%d", (_opname), vdst, vsrc1);                   \
        SET_REGISTER(vdst,                                                  \
            _cast GET_REGISTER(vsrc1) _op (GET_REGISTER(vsrc2) & 0x1f));    \
    }                                                                       \
    FINISH(2);

#define HANDLE_OP_X_INT_LIT16(_opcode, _opname, _op, _chkdiv)               \
    HANDLE_OPCODE(_opcode /*vA, vB, #+CCCC*/)                               \
        vdst = INST_A(inst);                                                \
        vsrc1 = INST_B(inst);                                               \
        vsrc2 = FETCH(1);                                                   \
        ILOGV("|%s-int/lit16 v%d,v%d,#+0x%04x",                             \
            (_opname), vdst, vsrc1, vsrc2);                                 \
        if (_chkdiv != 0) {                                                 \
            s4 firstVal, result;                                            \
            firstVal = GET_REGISTER(vsrc1);                                 \
            if ((s2) vsrc2 == 0) {                                          \
                EXPORT_PC();                                                \
                dvmThrowException("Ljava/lang/ArithmeticException;",        \
                    "divide by zero");                                      \
                GOTO_exceptionThrown();                                      \
            }                                                               \
            if ((u4)firstVal == 0x80000000 && ((s2) vsrc2) == -1) {         \
                /* won't generate /lit16 instr for this; check anyway */    \
                if (_chkdiv == 1)                                           \
                    result = firstVal;  /* division */                      \
                else                                                        \
                    result = 0;         /* remainder */                     \
            } else {                                                        \
                result = firstVal _op (s2) vsrc2;                           \
            }                                                               \
            SET_REGISTER(vdst, result);                                     \
        } else {                                                            \
            /* non-div/rem case */                                          \
            SET_REGISTER(vdst, GET_REGISTER(vsrc1) _op (s2) vsrc2);         \
        }                                                                   \
        FINISH(2);

#define HANDLE_OP_X_INT_LIT8(_opcode, _opname, _op, _chkdiv)                \
    HANDLE_OPCODE(_opcode /*vAA, vBB, #+CC*/)                               \
    {                                                                       \
        u2 litInfo;                                                         \
        vdst = INST_AA(inst);                                               \
        litInfo = FETCH(1);                                                 \
        vsrc1 = litInfo & 0xff;                                             \
        vsrc2 = litInfo >> 8;       /* constant */                          \
        ILOGV("|%s-int/lit8 v%d,v%d,#+0x%02x",                              \
            (_opname), vdst, vsrc1, vsrc2);                                 \
        if (_chkdiv != 0) {                                                 \
            s4 firstVal, result;                                            \
            firstVal = GET_REGISTER(vsrc1);                                 \
            if ((s1) vsrc2 == 0) {                                          \
                EXPORT_PC();                                                \
                dvmThrowException("Ljava/lang/ArithmeticException;",        \
                    "divide by zero");                                      \
                GOTO_exceptionThrown();                                     \
            }                                                               \
            if ((u4)firstVal == 0x80000000 && ((s1) vsrc2) == -1) {         \
                if (_chkdiv == 1)                                           \
                    result = firstVal;  /* division */                      \
                else                                                        \
                    result = 0;         /* remainder */                     \
            } else {                                                        \
                result = firstVal _op ((s1) vsrc2);                         \
            }                                                               \
            SET_REGISTER(vdst, result);                                     \
        } else {                                                            \
            SET_REGISTER(vdst,                                              \
                (s4) GET_REGISTER(vsrc1) _op (s1) vsrc2);                   \
        }                                                                   \
    }                                                                       \
    FINISH(2);

#define HANDLE_OP_SHX_INT_LIT8(_opcode, _opname, _cast, _op)                \
    HANDLE_OPCODE(_opcode /*vAA, vBB, #+CC*/)                               \
    {                                                                       \
        u2 litInfo;                                                         \
        vdst = INST_AA(inst);                                               \
        litInfo = FETCH(1);                                                 \
        vsrc1 = litInfo & 0xff;                                             \
        vsrc2 = litInfo >> 8;       /* constant */                          \
        ILOGV("|%s-int/lit8 v%d,v%d,#+0x%02x",                              \
            (_opname), vdst, vsrc1, vsrc2);                                 \
        SET_REGISTER(vdst,                                                  \
            _cast GET_REGISTER(vsrc1) _op (vsrc2 & 0x1f));                  \
    }                                                                       \
    FINISH(2);

#define HANDLE_OP_X_INT_2ADDR(_opcode, _opname, _op, _chkdiv)               \
    HANDLE_OPCODE(_opcode /*vA, vB*/)                                       \
        vdst = INST_A(inst);                                                \
        vsrc1 = INST_B(inst);                                               \
        ILOGV("|%s-int-2addr v%d,v%d", (_opname), vdst, vsrc1);             \
        if (_chkdiv != 0) {                                                 \
            s4 firstVal, secondVal, result;                                 \
            firstVal = GET_REGISTER(vdst);                                  \
            secondVal = GET_REGISTER(vsrc1);                                \
            if (secondVal == 0) {                                           \
                EXPORT_PC();                                                \
                dvmThrowException("Ljava/lang/ArithmeticException;",        \
                    "divide by zero");                                      \
                GOTO_exceptionThrown();                                     \
            }                                                               \
            if ((u4)firstVal == 0x80000000 && secondVal == -1) {            \
                if (_chkdiv == 1)                                           \
                    result = firstVal;  /* division */                      \
                else                                                        \
                    result = 0;         /* remainder */                     \
            } else {                                                        \
                result = firstVal _op secondVal;                            \
            }                                                               \
            SET_REGISTER(vdst, result);                                     \
        } else {                                                            \
            SET_REGISTER(vdst,                                              \
                (s4) GET_REGISTER(vdst) _op (s4) GET_REGISTER(vsrc1));      \
        }                                                                   \
        FINISH(1);

#define HANDLE_OP_SHX_INT_2ADDR(_opcode, _opname, _cast, _op)               \
    HANDLE_OPCODE(_opcode /*vA, vB*/)                                       \
        vdst = INST_A(inst);                                                \
        vsrc1 = INST_B(inst);                                               \
        ILOGV("|%s-int-2addr v%d,v%d", (_opname), vdst, vsrc1);             \
        SET_REGISTER(vdst,                                                  \
            _cast GET_REGISTER(vdst) _op (GET_REGISTER(vsrc1) & 0x1f));     \
        FINISH(1);

#define HANDLE_OP_X_LONG(_opcode, _opname, _op, _chkdiv)                    \
    HANDLE_OPCODE(_opcode /*vAA, vBB, vCC*/)                                \
    {                                                                       \
        u2 srcRegs;                                                         \
        vdst = INST_AA(inst);                                               \
        srcRegs = FETCH(1);                                                 \
        vsrc1 = srcRegs & 0xff;                                             \
        vsrc2 = srcRegs >> 8;                                               \
        ILOGV("|%s-long v%d,v%d,v%d", (_opname), vdst, vsrc1, vsrc2);       \
        if (_chkdiv != 0) {                                                 \
            s8 firstVal, secondVal, result;                                 \
            firstVal = GET_REGISTER_WIDE(vsrc1);                            \
            secondVal = GET_REGISTER_WIDE(vsrc2);                           \
            if (secondVal == 0LL) {                                         \
                EXPORT_PC();                                                \
                dvmThrowException("Ljava/lang/ArithmeticException;",        \
                    "divide by zero");                                      \
                GOTO_exceptionThrown();                                     \
            }                                                               \
            if ((u8)firstVal == 0x8000000000000000ULL &&                    \
                secondVal == -1LL)                                          \
            {                                                               \
                if (_chkdiv == 1)                                           \
                    result = firstVal;  /* division */                      \
                else                                                        \
                    result = 0;         /* remainder */                     \
            } else {                                                        \
                result = firstVal _op secondVal;                            \
            }                                                               \
            SET_REGISTER_WIDE(vdst, result);                                \
        } else {                                                            \
            SET_REGISTER_WIDE(vdst,                                         \
                (s8) GET_REGISTER_WIDE(vsrc1) _op (s8) GET_REGISTER_WIDE(vsrc2)); \
        }                                                                   \
    }                                                                       \
    FINISH(2);

#define HANDLE_OP_SHX_LONG(_opcode, _opname, _cast, _op)                    \
    HANDLE_OPCODE(_opcode /*vAA, vBB, vCC*/)                                \
    {                                                                       \
        u2 srcRegs;                                                         \
        vdst = INST_AA(inst);                                               \
        srcRegs = FETCH(1);                                                 \
        vsrc1 = srcRegs & 0xff;                                             \
        vsrc2 = srcRegs >> 8;                                               \
        ILOGV("|%s-long v%d,v%d,v%d", (_opname), vdst, vsrc1, vsrc2);       \
        SET_REGISTER_WIDE(vdst,                                             \
            _cast GET_REGISTER_WIDE(vsrc1) _op (GET_REGISTER(vsrc2) & 0x3f)); \
    }                                                                       \
    FINISH(2);

#define HANDLE_OP_X_LONG_2ADDR(_opcode, _opname, _op, _chkdiv)              \
    HANDLE_OPCODE(_opcode /*vA, vB*/)                                       \
        vdst = INST_A(inst);                                                \
        vsrc1 = INST_B(inst);                                               \
        ILOGV("|%s-long-2addr v%d,v%d", (_opname), vdst, vsrc1);            \
        if (_chkdiv != 0) {                                                 \
            s8 firstVal, secondVal, result;                                 \
            firstVal = GET_REGISTER_WIDE(vdst);                             \
            secondVal = GET_REGISTER_WIDE(vsrc1);                           \
            if (secondVal == 0LL) {                                         \
                EXPORT_PC();                                                \
                dvmThrowException("Ljava/lang/ArithmeticException;",        \
                    "divide by zero");                                      \
                GOTO_exceptionThrown();                                     \
            }                                                               \
            if ((u8)firstVal == 0x8000000000000000ULL &&                    \
                secondVal == -1LL)                                          \
            {                                                               \
                if (_chkdiv == 1)                                           \
                    result = firstVal;  /* division */                      \
                else                                                        \
                    result = 0;         /* remainder */                     \
            } else {                                                        \
                result = firstVal _op secondVal;                            \
            }                                                               \
            SET_REGISTER_WIDE(vdst, result);                                \
        } else {                                                            \
            SET_REGISTER_WIDE(vdst,                                         \
                (s8) GET_REGISTER_WIDE(vdst) _op (s8)GET_REGISTER_WIDE(vsrc1));\
        }                                                                   \
        FINISH(1);

#define HANDLE_OP_SHX_LONG_2ADDR(_opcode, _opname, _cast, _op)              \
    HANDLE_OPCODE(_opcode /*vA, vB*/)                                       \
        vdst = INST_A(inst);                                                \
        vsrc1 = INST_B(inst);                                               \
        ILOGV("|%s-long-2addr v%d,v%d", (_opname), vdst, vsrc1);            \
        SET_REGISTER_WIDE(vdst,                                             \
            _cast GET_REGISTER_WIDE(vdst) _op (GET_REGISTER(vsrc1) & 0x3f)); \
        FINISH(1);

#define HANDLE_OP_X_FLOAT(_opcode, _opname, _op)                            \
    HANDLE_OPCODE(_opcode /*vAA, vBB, vCC*/)                                \
    {                                                                       \
        u2 srcRegs;                                                         \
        vdst = INST_AA(inst);                                               \
        srcRegs = FETCH(1);                                                 \
        vsrc1 = srcRegs & 0xff;                                             \
        vsrc2 = srcRegs >> 8;                                               \
        ILOGV("|%s-float v%d,v%d,v%d", (_opname), vdst, vsrc1, vsrc2);      \
        SET_REGISTER_FLOAT(vdst,                                            \
            GET_REGISTER_FLOAT(vsrc1) _op GET_REGISTER_FLOAT(vsrc2));       \
    }                                                                       \
    FINISH(2);

#define HANDLE_OP_X_DOUBLE(_opcode, _opname, _op)                           \
    HANDLE_OPCODE(_opcode /*vAA, vBB, vCC*/)                                \
    {                                                                       \
        u2 srcRegs;                                                         \
        vdst = INST_AA(inst);                                               \
        srcRegs = FETCH(1);                                                 \
        vsrc1 = srcRegs & 0xff;                                             \
        vsrc2 = srcRegs >> 8;                                               \
        ILOGV("|%s-double v%d,v%d,v%d", (_opname), vdst, vsrc1, vsrc2);     \
        SET_REGISTER_DOUBLE(vdst,                                           \
            GET_REGISTER_DOUBLE(vsrc1) _op GET_REGISTER_DOUBLE(vsrc2));     \
    }                                                                       \
    FINISH(2);

#define HANDLE_OP_X_FLOAT_2ADDR(_opcode, _opname, _op)                      \
    HANDLE_OPCODE(_opcode /*vA, vB*/)                                       \
        vdst = INST_A(inst);                                                \
        vsrc1 = INST_B(inst);                                               \
        ILOGV("|%s-float-2addr v%d,v%d", (_opname), vdst, vsrc1);           \
        SET_REGISTER_FLOAT(vdst,                                            \
            GET_REGISTER_FLOAT(vdst) _op GET_REGISTER_FLOAT(vsrc1));        \
        FINISH(1);

#define HANDLE_OP_X_DOUBLE_2ADDR(_opcode, _opname, _op)                     \
    HANDLE_OPCODE(_opcode /*vA, vB*/)                                       \
        vdst = INST_A(inst);                                                \
        vsrc1 = INST_B(inst);                                               \
        ILOGV("|%s-double-2addr v%d,v%d", (_opname), vdst, vsrc1);          \
        SET_REGISTER_DOUBLE(vdst,                                           \
            GET_REGISTER_DOUBLE(vdst) _op GET_REGISTER_DOUBLE(vsrc1));      \
        FINISH(1);

#define HANDLE_OP_AGET(_opcode, _opname, _type, _regsize)                   \
    HANDLE_OPCODE(_opcode /*vAA, vBB, vCC*/)                                \
    {                                                                       \
        ArrayObject* arrayObj;                                              \
        u2 arrayInfo;                                                       \
        EXPORT_PC();                                                        \
        vdst = INST_AA(inst);                                               \
        arrayInfo = FETCH(1);                                               \
        vsrc1 = arrayInfo & 0xff;    /* array ptr */                        \
        vsrc2 = arrayInfo >> 8;      /* index */                            \
        ILOGV("|aget%s v%d,v%d,v%d", (_opname), vdst, vsrc1, vsrc2);        \
        arrayObj = (ArrayObject*) GET_REGISTER(vsrc1);                      \
        if (!checkForNull((Object*) arrayObj))                              \
            GOTO_exceptionThrown();                                         \
        if (GET_REGISTER(vsrc2) >= arrayObj->length) {                      \
            dvmThrowAIOOBE(GET_REGISTER(vsrc2), arrayObj->length);          \
            GOTO_exceptionThrown();                                         \
        }                                                                   \
        SET_REGISTER##_regsize(vdst,                                        \
            ((_type*) arrayObj->contents)[GET_REGISTER(vsrc2)]);            \
        ILOGV("+ AGET[%d]=0x%x", GET_REGISTER(vsrc2), GET_REGISTER(vdst));  \
    }                                                                       \
    FINISH(2);

#define HANDLE_OP_APUT(_opcode, _opname, _type, _regsize)                   \
    HANDLE_OPCODE(_opcode /*vAA, vBB, vCC*/)                                \
    {                                                                       \
        ArrayObject* arrayObj;                                              \
        u2 arrayInfo;                                                       \
        EXPORT_PC();                                                        \
        vdst = INST_AA(inst);       /* AA: source value */                  \
        arrayInfo = FETCH(1);                                               \
        vsrc1 = arrayInfo & 0xff;   /* BB: array ptr */                     \
        vsrc2 = arrayInfo >> 8;     /* CC: index */                         \
        ILOGV("|aput%s v%d,v%d,v%d", (_opname), vdst, vsrc1, vsrc2);        \
        arrayObj = (ArrayObject*) GET_REGISTER(vsrc1);                      \
        if (!checkForNull((Object*) arrayObj))                              \
            GOTO_exceptionThrown();                                         \
        if (GET_REGISTER(vsrc2) >= arrayObj->length) {                      \
            dvmThrowAIOOBE(GET_REGISTER(vsrc2), arrayObj->length);          \
            GOTO_exceptionThrown();                                         \
        }                                                                   \
        ILOGV("+ APUT[%d]=0x%08x", GET_REGISTER(vsrc2), GET_REGISTER(vdst));\
        ((_type*) arrayObj->contents)[GET_REGISTER(vsrc2)] =                \
            GET_REGISTER##_regsize(vdst);                                   \
    }                                                                       \
    FINISH(2);

/*
 * It's possible to get a bad value out of a field with sub-32-bit stores
 * because the -quick versions always operate on 32 bits.  Consider:
 *   short foo = -1  (sets a 32-bit register to 0xffffffff)
 *   iput-quick foo  (writes all 32 bits to the field)
 *   short bar = 1   (sets a 32-bit register to 0x00000001)
 *   iput-short      (writes the low 16 bits to the field)
 *   iget-quick foo  (reads all 32 bits from the field, yielding 0xffff0001)
 * This can only happen when optimized and non-optimized code has interleaved
 * access to the same field.  This is unlikely but possible.
 *
 * The easiest way to fix this is to always read/write 32 bits at a time.  On
 * a device with a 16-bit data bus this is sub-optimal.  (The alternative
 * approach is to have sub-int versions of iget-quick, but now we're wasting
 * Dalvik instruction space and making it less likely that handler code will
 * already be in the CPU i-cache.)
 */
#define HANDLE_IGET_X(_opcode, _opname, _ftype, _regsize)                   \
    HANDLE_OPCODE(_opcode /*vA, vB, field@CCCC*/)                           \
    {                                                                       \
        InstField* ifield;                                                  \
        Object* obj;                                                        \
        EXPORT_PC();                                                        \
        vdst = INST_A(inst);                                                \
        vsrc1 = INST_B(inst);   /* object ptr */                            \
        ref = FETCH(1);         /* field ref */                             \
        ILOGV("|iget%s v%d,v%d,field@0x%04x", (_opname), vdst, vsrc1, ref); \
        obj = (Object*) GET_REGISTER(vsrc1);                                \
        if (!checkForNull(obj))                                             \
            GOTO_exceptionThrown();                                         \
        ifield = (InstField*) dvmDexGetResolvedField(methodClassDex, ref);  \
        if (ifield == NULL) {                                               \
            ifield = dvmResolveInstField(curMethod->clazz, ref);            \
            if (ifield == NULL)                                             \
                GOTO_exceptionThrown();                                     \
        }                                                                   \
        SET_REGISTER##_regsize(vdst,                                        \
            dvmGetField##_ftype(obj, ifield->byteOffset));                  \
        ILOGV("+ IGET '%s'=0x%08llx", ifield->field.name,                   \
            (u8) GET_REGISTER##_regsize(vdst));                             \
        UPDATE_FIELD_GET(&ifield->field);                                   \
    }                                                                       \
    FINISH(2);

#define HANDLE_IGET_X_JUMBO(_opcode, _opname, _ftype, _regsize)             \
    HANDLE_OPCODE(_opcode /*vBBBB, vCCCC, class@AAAAAAAA*/)                 \
    {                                                                       \
        InstField* ifield;                                                  \
        Object* obj;                                                        \
        EXPORT_PC();                                                        \
        ref = FETCH(1) | (u4)FETCH(2) << 16;   /* field ref */              \
        vdst = FETCH(3);                                                    \
        vsrc1 = FETCH(4);                      /* object ptr */             \
        ILOGV("|iget%s/jumbo v%d,v%d,field@0x%08x",                         \
            (_opname), vdst, vsrc1, ref);                                   \
        obj = (Object*) GET_REGISTER(vsrc1);                                \
        if (!checkForNull(obj))                                             \
            GOTO_exceptionThrown();                                         \
        ifield = (InstField*) dvmDexGetResolvedField(methodClassDex, ref);  \
        if (ifield == NULL) {                                               \
            ifield = dvmResolveInstField(curMethod->clazz, ref);            \
            if (ifield == NULL)                                             \
                GOTO_exceptionThrown();                                     \
        }                                                                   \
        SET_REGISTER##_regsize(vdst,                                        \
            dvmGetField##_ftype(obj, ifield->byteOffset));                  \
        ILOGV("+ IGET '%s'=0x%08llx", ifield->field.name,                   \
            (u8) GET_REGISTER##_regsize(vdst));                             \
        UPDATE_FIELD_GET(&ifield->field);                                   \
    }                                                                       \
    FINISH(5);

#define HANDLE_IGET_X_QUICK(_opcode, _opname, _ftype, _regsize)             \
    HANDLE_OPCODE(_opcode /*vA, vB, field@CCCC*/)                           \
    {                                                                       \
        Object* obj;                                                        \
        vdst = INST_A(inst);                                                \
        vsrc1 = INST_B(inst);   /* object ptr */                            \
        ref = FETCH(1);         /* field offset */                          \
        ILOGV("|iget%s-quick v%d,v%d,field@+%u",                            \
            (_opname), vdst, vsrc1, ref);                                   \
        obj = (Object*) GET_REGISTER(vsrc1);                                \
        if (!checkForNullExportPC(obj, fp, pc))                             \
            GOTO_exceptionThrown();                                         \
        SET_REGISTER##_regsize(vdst, dvmGetField##_ftype(obj, ref));        \
        ILOGV("+ IGETQ %d=0x%08llx", ref,                                   \
            (u8) GET_REGISTER##_regsize(vdst));                             \
    }                                                                       \
    FINISH(2);

#define HANDLE_IPUT_X(_opcode, _opname, _ftype, _regsize)                   \
    HANDLE_OPCODE(_opcode /*vA, vB, field@CCCC*/)                           \
    {                                                                       \
        InstField* ifield;                                                  \
        Object* obj;                                                        \
        EXPORT_PC();                                                        \
        vdst = INST_A(inst);                                                \
        vsrc1 = INST_B(inst);   /* object ptr */                            \
        ref = FETCH(1);         /* field ref */                             \
        ILOGV("|iput%s v%d,v%d,field@0x%04x", (_opname), vdst, vsrc1, ref); \
        obj = (Object*) GET_REGISTER(vsrc1);                                \
        if (!checkForNull(obj))                                             \
            GOTO_exceptionThrown();                                         \
        ifield = (InstField*) dvmDexGetResolvedField(methodClassDex, ref);  \
        if (ifield == NULL) {                                               \
            ifield = dvmResolveInstField(curMethod->clazz, ref);            \
            if (ifield == NULL)                                             \
                GOTO_exceptionThrown();                                     \
        }                                                                   \
        dvmSetField##_ftype(obj, ifield->byteOffset,                        \
            GET_REGISTER##_regsize(vdst));                                  \
        ILOGV("+ IPUT '%s'=0x%08llx", ifield->field.name,                   \
            (u8) GET_REGISTER##_regsize(vdst));                             \
        UPDATE_FIELD_PUT(&ifield->field);                                   \
    }                                                                       \
    FINISH(2);

#define HANDLE_IPUT_X_JUMBO(_opcode, _opname, _ftype, _regsize)             \
    HANDLE_OPCODE(_opcode /*vBBBB, vCCCC, class@AAAAAAAA*/)                 \
    {                                                                       \
        InstField* ifield;                                                  \
        Object* obj;                                                        \
        EXPORT_PC();                                                        \
        ref = FETCH(1) | (u4)FETCH(2) << 16;   /* field ref */              \
        vdst = FETCH(3);                                                    \
        vsrc1 = FETCH(4);                      /* object ptr */             \
        ILOGV("|iput%s/jumbo v%d,v%d,field@0x%08x",                         \
            (_opname), vdst, vsrc1, ref);                                   \
        obj = (Object*) GET_REGISTER(vsrc1);                                \
        if (!checkForNull(obj))                                             \
            GOTO_exceptionThrown();                                         \
        ifield = (InstField*) dvmDexGetResolvedField(methodClassDex, ref);  \
        if (ifield == NULL) {                                               \
            ifield = dvmResolveInstField(curMethod->clazz, ref);            \
            if (ifield == NULL)                                             \
                GOTO_exceptionThrown();                                     \
        }                                                                   \
        dvmSetField##_ftype(obj, ifield->byteOffset,                        \
            GET_REGISTER##_regsize(vdst));                                  \
        ILOGV("+ IPUT '%s'=0x%08llx", ifield->field.name,                   \
            (u8) GET_REGISTER##_regsize(vdst));                             \
        UPDATE_FIELD_PUT(&ifield->field);                                   \
    }                                                                       \
    FINISH(5);

#define HANDLE_IPUT_X_QUICK(_opcode, _opname, _ftype, _regsize)             \
    HANDLE_OPCODE(_opcode /*vA, vB, field@CCCC*/)                           \
    {                                                                       \
        Object* obj;                                                        \
        vdst = INST_A(inst);                                                \
        vsrc1 = INST_B(inst);   /* object ptr */                            \
        ref = FETCH(1);         /* field offset */                          \
        ILOGV("|iput%s-quick v%d,v%d,field@0x%04x",                         \
            (_opname), vdst, vsrc1, ref);                                   \
        obj = (Object*) GET_REGISTER(vsrc1);                                \
        if (!checkForNullExportPC(obj, fp, pc))                             \
            GOTO_exceptionThrown();                                         \
        dvmSetField##_ftype(obj, ref, GET_REGISTER##_regsize(vdst));        \
        ILOGV("+ IPUTQ %d=0x%08llx", ref,                                   \
            (u8) GET_REGISTER##_regsize(vdst));                             \
    }                                                                       \
    FINISH(2);

/*
 * The JIT needs dvmDexGetResolvedField() to return non-null.
 * Since we use the portable interpreter to build the trace, the extra
 * checks in HANDLE_SGET_X and HANDLE_SPUT_X are not needed for mterp.
 */
#define HANDLE_SGET_X(_opcode, _opname, _ftype, _regsize)                   \
    HANDLE_OPCODE(_opcode /*vAA, field@BBBB*/)                              \
    {                                                                       \
        StaticField* sfield;                                                \
        vdst = INST_AA(inst);                                               \
        ref = FETCH(1);         /* field ref */                             \
        ILOGV("|sget%s v%d,sfield@0x%04x", (_opname), vdst, ref);           \
        sfield = (StaticField*)dvmDexGetResolvedField(methodClassDex, ref); \
        if (sfield == NULL) {                                               \
            EXPORT_PC();                                                    \
            sfield = dvmResolveStaticField(curMethod->clazz, ref);          \
            if (sfield == NULL)                                             \
                GOTO_exceptionThrown();                                     \
            if (dvmDexGetResolvedField(methodClassDex, ref) == NULL) {      \
                END_JIT_TSELECT();                                        \
            }                                                               \
        }                                                                   \
        SET_REGISTER##_regsize(vdst, dvmGetStaticField##_ftype(sfield));    \
        ILOGV("+ SGET '%s'=0x%08llx",                                       \
            sfield->field.name, (u8)GET_REGISTER##_regsize(vdst));          \
        UPDATE_FIELD_GET(&sfield->field);                                   \
    }                                                                       \
    FINISH(2);

#define HANDLE_SGET_X_JUMBO(_opcode, _opname, _ftype, _regsize)             \
    HANDLE_OPCODE(_opcode /*vBBBB, class@AAAAAAAA*/)                        \
    {                                                                       \
        StaticField* sfield;                                                \
        ref = FETCH(1) | (u4)FETCH(2) << 16;   /* field ref */              \
        vdst = FETCH(3);                                                    \
        ILOGV("|sget%s/jumbo v%d,sfield@0x%08x", (_opname), vdst, ref);     \
        sfield = (StaticField*)dvmDexGetResolvedField(methodClassDex, ref); \
        if (sfield == NULL) {                                               \
            EXPORT_PC();                                                    \
            sfield = dvmResolveStaticField(curMethod->clazz, ref);          \
            if (sfield == NULL)                                             \
                GOTO_exceptionThrown();                                     \
            if (dvmDexGetResolvedField(methodClassDex, ref) == NULL) {      \
                END_JIT_TSELECT();                                        \
            }                                                               \
        }                                                                   \
        SET_REGISTER##_regsize(vdst, dvmGetStaticField##_ftype(sfield));    \
        ILOGV("+ SGET '%s'=0x%08llx",                                       \
            sfield->field.name, (u8)GET_REGISTER##_regsize(vdst));          \
        UPDATE_FIELD_GET(&sfield->field);                                   \
    }                                                                       \
    FINISH(4);

#define HANDLE_SPUT_X(_opcode, _opname, _ftype, _regsize)                   \
    HANDLE_OPCODE(_opcode /*vAA, field@BBBB*/)                              \
    {                                                                       \
        StaticField* sfield;                                                \
        vdst = INST_AA(inst);                                               \
        ref = FETCH(1);         /* field ref */                             \
        ILOGV("|sput%s v%d,sfield@0x%04x", (_opname), vdst, ref);           \
        sfield = (StaticField*)dvmDexGetResolvedField(methodClassDex, ref); \
        if (sfield == NULL) {                                               \
            EXPORT_PC();                                                    \
            sfield = dvmResolveStaticField(curMethod->clazz, ref);          \
            if (sfield == NULL)                                             \
                GOTO_exceptionThrown();                                     \
            if (dvmDexGetResolvedField(methodClassDex, ref) == NULL) {      \
                END_JIT_TSELECT();                                        \
            }                                                               \
        }                                                                   \
        dvmSetStaticField##_ftype(sfield, GET_REGISTER##_regsize(vdst));    \
        ILOGV("+ SPUT '%s'=0x%08llx",                                       \
            sfield->field.name, (u8)GET_REGISTER##_regsize(vdst));          \
        UPDATE_FIELD_PUT(&sfield->field);                                   \
    }                                                                       \
    FINISH(2);

#define HANDLE_SPUT_X_JUMBO(_opcode, _opname, _ftype, _regsize)             \
    HANDLE_OPCODE(_opcode /*vBBBB, class@AAAAAAAA*/)                        \
    {                                                                       \
        StaticField* sfield;                                                \
        ref = FETCH(1) | (u4)FETCH(2) << 16;   /* field ref */              \
        vdst = FETCH(3);                                                    \
        ILOGV("|sput%s/jumbo v%d,sfield@0x%08x", (_opname), vdst, ref);     \
        sfield = (StaticField*)dvmDexGetResolvedField(methodClassDex, ref); \
        if (sfield == NULL) {                                               \
            EXPORT_PC();                                                    \
            sfield = dvmResolveStaticField(curMethod->clazz, ref);          \
            if (sfield == NULL)                                             \
                GOTO_exceptionThrown();                                     \
            if (dvmDexGetResolvedField(methodClassDex, ref) == NULL) {      \
                END_JIT_TSELECT();                                        \
            }                                                               \
        }                                                                   \
        dvmSetStaticField##_ftype(sfield, GET_REGISTER##_regsize(vdst));    \
        ILOGV("+ SPUT '%s'=0x%08llx",                                       \
            sfield->field.name, (u8)GET_REGISTER##_regsize(vdst));          \
        UPDATE_FIELD_PUT(&sfield->field);                                   \
    }                                                                       \
    FINISH(4);

/* File: portable/debug.c */
/* code in here is only included in portable-debug interpreter */

/*
 * Update the debugger on interesting events, such as hitting a breakpoint
 * or a single-step point.  This is called from the top of the interpreter
 * loop, before the current instruction is processed.
 *
 * Set "methodEntry" if we've just entered the method.  This detects
 * method exit by checking to see if the next instruction is "return".
 *
 * This can't catch native method entry/exit, so we have to handle that
 * at the point of invocation.  We also need to catch it in dvmCallMethod
 * if we want to capture native->native calls made through JNI.
 *
 * Notes to self:
 * - Don't want to switch to VMWAIT while posting events to the debugger.
 *   Let the debugger code decide if we need to change state.
 * - We may want to check for debugger-induced thread suspensions on
 *   every instruction.  That would make a "suspend all" more responsive
 *   and reduce the chances of multiple simultaneous events occurring.
 *   However, it could change the behavior some.
 *
 * TODO: method entry/exit events are probably less common than location
 * breakpoints.  We may be able to speed things up a bit if we don't query
 * the event list unless we know there's at least one lurking within.
 */
static void updateDebugger(const Method* method, const u2* pc, const u4* fp,
    bool methodEntry, Thread* self)
{
    int eventFlags = 0;

    /*
     * Update xtra.currentPc on every instruction.  We need to do this if
     * there's a chance that we could get suspended.  This can happen if
     * eventFlags != 0 here, or somebody manually requests a suspend
     * (which gets handled at PERIOD_CHECKS time).  One place where this
     * needs to be correct is in dvmAddSingleStep().
     */
    EXPORT_PC();

    if (methodEntry)
        eventFlags |= DBG_METHOD_ENTRY;

    /*
     * See if we have a breakpoint here.
     *
     * Depending on the "mods" associated with event(s) on this address,
     * we may or may not actually send a message to the debugger.
     */
    if (INST_INST(*pc) == OP_BREAKPOINT) {
        LOGV("+++ breakpoint hit at %p\n", pc);
        eventFlags |= DBG_BREAKPOINT;
    }

    /*
     * If the debugger is single-stepping one of our threads, check to
     * see if we're that thread and we've reached a step point.
     */
    const StepControl* pCtrl = &gDvm.stepControl;
    if (pCtrl->active && pCtrl->thread == self) {
        int frameDepth;
        bool doStop = false;
        const char* msg = NULL;

        assert(!dvmIsNativeMethod(method));

        if (pCtrl->depth == SD_INTO) {
            /*
             * Step into method calls.  We break when the line number
             * or method pointer changes.  If we're in SS_MIN mode, we
             * always stop.
             */
            if (pCtrl->method != method) {
                doStop = true;
                msg = "new method";
            } else if (pCtrl->size == SS_MIN) {
                doStop = true;
                msg = "new instruction";
            } else if (!dvmAddressSetGet(
                    pCtrl->pAddressSet, pc - method->insns)) {
                doStop = true;
                msg = "new line";
            }
        } else if (pCtrl->depth == SD_OVER) {
            /*
             * Step over method calls.  We break when the line number is
             * different and the frame depth is <= the original frame
             * depth.  (We can't just compare on the method, because we
             * might get unrolled past it by an exception, and it's tricky
             * to identify recursion.)
             */
            frameDepth = dvmComputeVagueFrameDepth(self, fp);
            if (frameDepth < pCtrl->frameDepth) {
                /* popped up one or more frames, always trigger */
                doStop = true;
                msg = "method pop";
            } else if (frameDepth == pCtrl->frameDepth) {
                /* same depth, see if we moved */
                if (pCtrl->size == SS_MIN) {
                    doStop = true;
                    msg = "new instruction";
                } else if (!dvmAddressSetGet(pCtrl->pAddressSet,
                            pc - method->insns)) {
                    doStop = true;
                    msg = "new line";
                }
            }
        } else {
            assert(pCtrl->depth == SD_OUT);
            /*
             * Return from the current method.  We break when the frame
             * depth pops up.
             *
             * This differs from the "method exit" break in that it stops
             * with the PC at the next instruction in the returned-to
             * function, rather than the end of the returning function.
             */
            frameDepth = dvmComputeVagueFrameDepth(self, fp);
            if (frameDepth < pCtrl->frameDepth) {
                doStop = true;
                msg = "method pop";
            }
        }

        if (doStop) {
            LOGV("#####S %s\n", msg);
            eventFlags |= DBG_SINGLE_STEP;
        }
    }

    /*
     * Check to see if this is a "return" instruction.  JDWP says we should
     * send the event *after* the code has been executed, but it also says
     * the location we provide is the last instruction.  Since the "return"
     * instruction has no interesting side effects, we should be safe.
     * (We can't just move this down to the returnFromMethod label because
     * we potentially need to combine it with other events.)
     *
     * We're also not supposed to generate a method exit event if the method
     * terminates "with a thrown exception".
     */
    u2 inst = INST_INST(FETCH(0));
    if (inst == OP_RETURN_VOID || inst == OP_RETURN || inst == OP_RETURN_WIDE ||
        inst == OP_RETURN_OBJECT)
    {
        eventFlags |= DBG_METHOD_EXIT;
    }

    /*
     * If there's something interesting going on, see if it matches one
     * of the debugger filters.
     */
    if (eventFlags != 0) {
        Object* thisPtr = dvmGetThisPtr(method, fp);
        if (thisPtr != NULL && !dvmIsValidObject(thisPtr)) {
            /*
             * TODO: remove this check if we're confident that the "this"
             * pointer is where it should be -- slows us down, especially
             * during single-step.
             */
            char* desc = dexProtoCopyMethodDescriptor(&method->prototype);
            LOGE("HEY: invalid 'this' ptr %p (%s.%s %s)\n", thisPtr,
                method->clazz->descriptor, method->name, desc);
            free(desc);
            dvmAbort();
        }
        dvmDbgPostLocationEvent(method, pc - method->insns, thisPtr,
            eventFlags);
    }
}

/*
 * Perform some operations at the "top" of the interpreter loop.
 * This stuff is required to support debugging and profiling.
 *
 * Using" __attribute__((noinline))" seems to do more harm than good.  This
 * is best when inlined due to the large number of parameters, most of
 * which are local vars in the main interp loop.
 */
static void checkDebugAndProf(const u2* pc, const u4* fp, Thread* self,
    const Method* method, bool* pIsMethodEntry)
{
    /* check to see if we've run off end of method */
    assert(pc >= method->insns && pc <
            method->insns + dvmGetMethodInsnsSize(method));

#if 0
    /*
     * When we hit a specific method, enable verbose instruction logging.
     * Sometimes it's helpful to use the debugger attach as a trigger too.
     */
    if (*pIsMethodEntry) {
        static const char* cd = "Landroid/test/Arithmetic;";
        static const char* mn = "shiftTest2";
        static const char* sg = "()V";

        if (/*DEBUGGER_ACTIVE &&*/
            strcmp(method->clazz->descriptor, cd) == 0 &&
            strcmp(method->name, mn) == 0 &&
            strcmp(method->shorty, sg) == 0)
        {
            LOGW("Reached %s.%s, enabling verbose mode\n",
                method->clazz->descriptor, method->name);
            android_setMinPriority(LOG_TAG"i", ANDROID_LOG_VERBOSE);
            dumpRegs(method, fp, true);
        }

        if (!DEBUGGER_ACTIVE)
            *pIsMethodEntry = false;
    }
#endif

    /*
     * If the debugger is attached, check for events.  If the profiler is
     * enabled, update that too.
     *
     * This code is executed for every instruction we interpret, so for
     * performance we use a couple of #ifdef blocks instead of runtime tests.
     */
    bool isEntry = *pIsMethodEntry;
    if (isEntry) {
        *pIsMethodEntry = false;
        TRACE_METHOD_ENTER(self, method);
    }
    if (DEBUGGER_ACTIVE) {
        updateDebugger(method, pc, fp, isEntry, self);
    }
    if (gDvm.instructionCountEnableCount != 0) {
        /*
         * Count up the #of executed instructions.  This isn't synchronized
         * for thread-safety; if we need that we should make this
         * thread-local and merge counts into the global area when threads
         * exit (perhaps suspending all other threads GC-style and pulling
         * the data out of them).
         */
        int inst = *pc & 0xff;
        gDvm.executedInstrCounts[inst]++;
    }
}

/* File: portable/entry.c */
/*
 * Main interpreter loop.
 *
 * This was written with an ARM implementation in mind.
 */
bool INTERP_FUNC_NAME(Thread* self, InterpState* interpState)
{
#if defined(EASY_GDB)
    StackSaveArea* debugSaveArea = SAVEAREA_FROM_FP(self->curFrame);
#endif
#if INTERP_TYPE == INTERP_DBG
    bool debugIsMethodEntry = false;
    debugIsMethodEntry = interpState->debugIsMethodEntry;
#endif
#if defined(WITH_TRACKREF_CHECKS)
    int debugTrackedRefStart = interpState->debugTrackedRefStart;
#endif
    DvmDex* methodClassDex;     // curMethod->clazz->pDvmDex
    JValue retval;

    /* core state */
    const Method* curMethod;    // method we're interpreting
    const u2* pc;               // program counter
    u4* fp;                     // frame pointer
    u2 inst;                    // current instruction
    /* instruction decoding */
    u4 ref;                     // 16 or 32-bit quantity fetched directly
    u2 vsrc1, vsrc2, vdst;      // usually used for register indexes
    /* method call setup */
    const Method* methodToCall;
    bool methodCallRange;
    bool jumboFormat;


#if defined(THREADED_INTERP)
    /* static computed goto table */
    DEFINE_GOTO_TABLE(handlerTable);
#endif

#if defined(WITH_JIT)
#if 0
    LOGD("*DebugInterp - entrypoint is %d, tgt is 0x%x, %s\n",
         interpState->entryPoint,
         interpState->pc,
         interpState->method->name);
#endif
#if INTERP_TYPE == INTERP_DBG
    const ClassObject* callsiteClass = NULL;

#if defined(WITH_SELF_VERIFICATION)
    if (interpState->jitState != kJitSelfVerification) {
        interpState->self->shadowSpace->jitExitState = kSVSIdle;
    }
#endif

    /* Check to see if we've got a trace selection request. */
    if (
         /*
          * Only perform dvmJitCheckTraceRequest if the entry point is
          * EntryInstr and the jit state is either kJitTSelectRequest or
          * kJitTSelectRequestHot. If debugger/profiler happens to be attached,
          * dvmJitCheckTraceRequest will change the jitState to kJitDone but
          * but stay in the dbg interpreter.
          */
         (interpState->entryPoint == kInterpEntryInstr) &&
         (interpState->jitState == kJitTSelectRequest ||
          interpState->jitState == kJitTSelectRequestHot) &&
         dvmJitCheckTraceRequest(self, interpState)) {
        interpState->nextMode = INTERP_STD;
        //LOGD("Invalid trace request, exiting\n");
        return true;
    }
#endif /* INTERP_TYPE == INTERP_DBG */
#endif /* WITH_JIT */

    /* copy state in */
    curMethod = interpState->method;
    pc = interpState->pc;
    fp = interpState->fp;
    retval = interpState->retval;   /* only need for kInterpEntryReturn? */

    methodClassDex = curMethod->clazz->pDvmDex;

    LOGVV("threadid=%d: entry(%s) %s.%s pc=0x%x fp=%p ep=%d\n",
        self->threadId, (interpState->nextMode == INTERP_STD) ? "STD" : "DBG",
        curMethod->clazz->descriptor, curMethod->name, pc - curMethod->insns,
        fp, interpState->entryPoint);

    /*
     * DEBUG: scramble this to ensure we're not relying on it.
     */
    methodToCall = (const Method*) -1;

#if INTERP_TYPE == INTERP_DBG
    if (debugIsMethodEntry) {
        ILOGD("|-- Now interpreting %s.%s", curMethod->clazz->descriptor,
                curMethod->name);
        DUMP_REGS(curMethod, interpState->fp, false);
    }
#endif

    switch (interpState->entryPoint) {
    case kInterpEntryInstr:
        /* just fall through to instruction loop or threaded kickstart */
        break;
    case kInterpEntryReturn:
        CHECK_JIT_VOID();
        goto returnFromMethod;
    case kInterpEntryThrow:
        goto exceptionThrown;
    default:
        dvmAbort();
    }

#ifdef THREADED_INTERP
    FINISH(0);                  /* fetch and execute first instruction */
#else
    while (1) {
        CHECK_DEBUG_AND_PROF(); /* service debugger and profiling */
        CHECK_TRACKED_REFS();   /* check local reference tracking */

        /* fetch the next 16 bits from the instruction stream */
        inst = FETCH(0);

        switch (INST_INST(inst)) {
#endif

/*--- start of opcodes ---*/

/* File: c/OP_NOP.c */
HANDLE_OPCODE(OP_NOP)
    FINISH(1);
OP_END

/* File: c/OP_MOVE.c */
HANDLE_OPCODE(OP_MOVE /*vA, vB*/)
    vdst = INST_A(inst);
    vsrc1 = INST_B(inst);
    ILOGV("|move%s v%d,v%d %s(v%d=0x%08x)",
        (INST_INST(inst) == OP_MOVE) ? "" : "-object", vdst, vsrc1,
        kSpacing, vdst, GET_REGISTER(vsrc1));
    SET_REGISTER(vdst, GET_REGISTER(vsrc1));
    FINISH(1);
OP_END

/* File: c/OP_MOVE_FROM16.c */
HANDLE_OPCODE(OP_MOVE_FROM16 /*vAA, vBBBB*/)
    vdst = INST_AA(inst);
    vsrc1 = FETCH(1);
    ILOGV("|move%s/from16 v%d,v%d %s(v%d=0x%08x)",
        (INST_INST(inst) == OP_MOVE_FROM16) ? "" : "-object", vdst, vsrc1,
        kSpacing, vdst, GET_REGISTER(vsrc1));
    SET_REGISTER(vdst, GET_REGISTER(vsrc1));
    FINISH(2);
OP_END

/* File: c/OP_MOVE_16.c */
HANDLE_OPCODE(OP_MOVE_16 /*vAAAA, vBBBB*/)
    vdst = FETCH(1);
    vsrc1 = FETCH(2);
    ILOGV("|move%s/16 v%d,v%d %s(v%d=0x%08x)",
        (INST_INST(inst) == OP_MOVE_16) ? "" : "-object", vdst, vsrc1,
        kSpacing, vdst, GET_REGISTER(vsrc1));
    SET_REGISTER(vdst, GET_REGISTER(vsrc1));
    FINISH(3);
OP_END

/* File: c/OP_MOVE_WIDE.c */
HANDLE_OPCODE(OP_MOVE_WIDE /*vA, vB*/)
    /* IMPORTANT: must correctly handle overlapping registers, e.g. both
     * "move-wide v6, v7" and "move-wide v7, v6" */
    vdst = INST_A(inst);
    vsrc1 = INST_B(inst);
    ILOGV("|move-wide v%d,v%d %s(v%d=0x%08llx)", vdst, vsrc1,
        kSpacing+5, vdst, GET_REGISTER_WIDE(vsrc1));
    SET_REGISTER_WIDE(vdst, GET_REGISTER_WIDE(vsrc1));
    FINISH(1);
OP_END

/* File: c/OP_MOVE_WIDE_FROM16.c */
HANDLE_OPCODE(OP_MOVE_WIDE_FROM16 /*vAA, vBBBB*/)
    vdst = INST_AA(inst);
    vsrc1 = FETCH(1);
    ILOGV("|move-wide/from16 v%d,v%d  (v%d=0x%08llx)", vdst, vsrc1,
        vdst, GET_REGISTER_WIDE(vsrc1));
    SET_REGISTER_WIDE(vdst, GET_REGISTER_WIDE(vsrc1));
    FINISH(2);
OP_END

/* File: c/OP_MOVE_WIDE_16.c */
HANDLE_OPCODE(OP_MOVE_WIDE_16 /*vAAAA, vBBBB*/)
    vdst = FETCH(1);
    vsrc1 = FETCH(2);
    ILOGV("|move-wide/16 v%d,v%d %s(v%d=0x%08llx)", vdst, vsrc1,
        kSpacing+8, vdst, GET_REGISTER_WIDE(vsrc1));
    SET_REGISTER_WIDE(vdst, GET_REGISTER_WIDE(vsrc1));
    FINISH(3);
OP_END

/* File: c/OP_MOVE_OBJECT.c */
/* File: c/OP_MOVE.c */
HANDLE_OPCODE(OP_MOVE_OBJECT /*vA, vB*/)
    vdst = INST_A(inst);
    vsrc1 = INST_B(inst);
    ILOGV("|move%s v%d,v%d %s(v%d=0x%08x)",
        (INST_INST(inst) == OP_MOVE) ? "" : "-object", vdst, vsrc1,
        kSpacing, vdst, GET_REGISTER(vsrc1));
    SET_REGISTER(vdst, GET_REGISTER(vsrc1));
    FINISH(1);
OP_END


/* File: c/OP_MOVE_OBJECT_FROM16.c */
/* File: c/OP_MOVE_FROM16.c */
HANDLE_OPCODE(OP_MOVE_OBJECT_FROM16 /*vAA, vBBBB*/)
    vdst = INST_AA(inst);
    vsrc1 = FETCH(1);
    ILOGV("|move%s/from16 v%d,v%d %s(v%d=0x%08x)",
        (INST_INST(inst) == OP_MOVE_FROM16) ? "" : "-object", vdst, vsrc1,
        kSpacing, vdst, GET_REGISTER(vsrc1));
    SET_REGISTER(vdst, GET_REGISTER(vsrc1));
    FINISH(2);
OP_END


/* File: c/OP_MOVE_OBJECT_16.c */
/* File: c/OP_MOVE_16.c */
HANDLE_OPCODE(OP_MOVE_OBJECT_16 /*vAAAA, vBBBB*/)
    vdst = FETCH(1);
    vsrc1 = FETCH(2);
    ILOGV("|move%s/16 v%d,v%d %s(v%d=0x%08x)",
        (INST_INST(inst) == OP_MOVE_16) ? "" : "-object", vdst, vsrc1,
        kSpacing, vdst, GET_REGISTER(vsrc1));
    SET_REGISTER(vdst, GET_REGISTER(vsrc1));
    FINISH(3);
OP_END


/* File: c/OP_MOVE_RESULT.c */
HANDLE_OPCODE(OP_MOVE_RESULT /*vAA*/)
    vdst = INST_AA(inst);
    ILOGV("|move-result%s v%d %s(v%d=0x%08x)",
         (INST_INST(inst) == OP_MOVE_RESULT) ? "" : "-object",
         vdst, kSpacing+4, vdst,retval.i);
    SET_REGISTER(vdst, retval.i);
    FINISH(1);
OP_END

/* File: c/OP_MOVE_RESULT_WIDE.c */
HANDLE_OPCODE(OP_MOVE_RESULT_WIDE /*vAA*/)
    vdst = INST_AA(inst);
    ILOGV("|move-result-wide v%d %s(0x%08llx)", vdst, kSpacing, retval.j);
    SET_REGISTER_WIDE(vdst, retval.j);
    FINISH(1);
OP_END

/* File: c/OP_MOVE_RESULT_OBJECT.c */
/* File: c/OP_MOVE_RESULT.c */
HANDLE_OPCODE(OP_MOVE_RESULT_OBJECT /*vAA*/)
    vdst = INST_AA(inst);
    ILOGV("|move-result%s v%d %s(v%d=0x%08x)",
         (INST_INST(inst) == OP_MOVE_RESULT) ? "" : "-object",
         vdst, kSpacing+4, vdst,retval.i);
    SET_REGISTER(vdst, retval.i);
    FINISH(1);
OP_END


/* File: c/OP_MOVE_EXCEPTION.c */
HANDLE_OPCODE(OP_MOVE_EXCEPTION /*vAA*/)
    vdst = INST_AA(inst);
    ILOGV("|move-exception v%d", vdst);
    assert(self->exception != NULL);
    SET_REGISTER(vdst, (u4)self->exception);
    dvmClearException(self);
    FINISH(1);
OP_END

/* File: c/OP_RETURN_VOID.c */
HANDLE_OPCODE(OP_RETURN_VOID /**/)
    ILOGV("|return-void");
#ifndef NDEBUG
    retval.j = 0xababababULL;    // placate valgrind
#endif
    GOTO_returnFromMethod();
OP_END

/* File: c/OP_RETURN.c */
HANDLE_OPCODE(OP_RETURN /*vAA*/)
    vsrc1 = INST_AA(inst);
    ILOGV("|return%s v%d",
        (INST_INST(inst) == OP_RETURN) ? "" : "-object", vsrc1);
    retval.i = GET_REGISTER(vsrc1);
    GOTO_returnFromMethod();
OP_END

/* File: c/OP_RETURN_WIDE.c */
HANDLE_OPCODE(OP_RETURN_WIDE /*vAA*/)
    vsrc1 = INST_AA(inst);
    ILOGV("|return-wide v%d", vsrc1);
    retval.j = GET_REGISTER_WIDE(vsrc1);
    GOTO_returnFromMethod();
OP_END

/* File: c/OP_RETURN_OBJECT.c */
/* File: c/OP_RETURN.c */
HANDLE_OPCODE(OP_RETURN_OBJECT /*vAA*/)
    vsrc1 = INST_AA(inst);
    ILOGV("|return%s v%d",
        (INST_INST(inst) == OP_RETURN) ? "" : "-object", vsrc1);
    retval.i = GET_REGISTER(vsrc1);
    GOTO_returnFromMethod();
OP_END


/* File: c/OP_CONST_4.c */
HANDLE_OPCODE(OP_CONST_4 /*vA, #+B*/)
    {
        s4 tmp;

        vdst = INST_A(inst);
        tmp = (s4) (INST_B(inst) << 28) >> 28;  // sign extend 4-bit value
        ILOGV("|const/4 v%d,#0x%02x", vdst, (s4)tmp);
        SET_REGISTER(vdst, tmp);
    }
    FINISH(1);
OP_END

/* File: c/OP_CONST_16.c */
HANDLE_OPCODE(OP_CONST_16 /*vAA, #+BBBB*/)
    vdst = INST_AA(inst);
    vsrc1 = FETCH(1);
    ILOGV("|const/16 v%d,#0x%04x", vdst, (s2)vsrc1);
    SET_REGISTER(vdst, (s2) vsrc1);
    FINISH(2);
OP_END

/* File: c/OP_CONST.c */
HANDLE_OPCODE(OP_CONST /*vAA, #+BBBBBBBB*/)
    {
        u4 tmp;

        vdst = INST_AA(inst);
        tmp = FETCH(1);
        tmp |= (u4)FETCH(2) << 16;
        ILOGV("|const v%d,#0x%08x", vdst, tmp);
        SET_REGISTER(vdst, tmp);
    }
    FINISH(3);
OP_END

/* File: c/OP_CONST_HIGH16.c */
HANDLE_OPCODE(OP_CONST_HIGH16 /*vAA, #+BBBB0000*/)
    vdst = INST_AA(inst);
    vsrc1 = FETCH(1);
    ILOGV("|const/high16 v%d,#0x%04x0000", vdst, vsrc1);
    SET_REGISTER(vdst, vsrc1 << 16);
    FINISH(2);
OP_END

/* File: c/OP_CONST_WIDE_16.c */
HANDLE_OPCODE(OP_CONST_WIDE_16 /*vAA, #+BBBB*/)
    vdst = INST_AA(inst);
    vsrc1 = FETCH(1);
    ILOGV("|const-wide/16 v%d,#0x%04x", vdst, (s2)vsrc1);
    SET_REGISTER_WIDE(vdst, (s2)vsrc1);
    FINISH(2);
OP_END

/* File: c/OP_CONST_WIDE_32.c */
HANDLE_OPCODE(OP_CONST_WIDE_32 /*vAA, #+BBBBBBBB*/)
    {
        u4 tmp;

        vdst = INST_AA(inst);
        tmp = FETCH(1);
        tmp |= (u4)FETCH(2) << 16;
        ILOGV("|const-wide/32 v%d,#0x%08x", vdst, tmp);
        SET_REGISTER_WIDE(vdst, (s4) tmp);
    }
    FINISH(3);
OP_END

/* File: c/OP_CONST_WIDE.c */
HANDLE_OPCODE(OP_CONST_WIDE /*vAA, #+BBBBBBBBBBBBBBBB*/)
    {
        u8 tmp;

        vdst = INST_AA(inst);
        tmp = FETCH(1);
        tmp |= (u8)FETCH(2) << 16;
        tmp |= (u8)FETCH(3) << 32;
        tmp |= (u8)FETCH(4) << 48;
        ILOGV("|const-wide v%d,#0x%08llx", vdst, tmp);
        SET_REGISTER_WIDE(vdst, tmp);
    }
    FINISH(5);
OP_END

/* File: c/OP_CONST_WIDE_HIGH16.c */
HANDLE_OPCODE(OP_CONST_WIDE_HIGH16 /*vAA, #+BBBB000000000000*/)
    vdst = INST_AA(inst);
    vsrc1 = FETCH(1);
    ILOGV("|const-wide/high16 v%d,#0x%04x000000000000", vdst, vsrc1);
    SET_REGISTER_WIDE(vdst, ((u8) vsrc1) << 48);
    FINISH(2);
OP_END

/* File: c/OP_CONST_STRING.c */
HANDLE_OPCODE(OP_CONST_STRING /*vAA, string@BBBB*/)
    {
        StringObject* strObj;

        vdst = INST_AA(inst);
        ref = FETCH(1);
        ILOGV("|const-string v%d string@0x%04x", vdst, ref);
        strObj = dvmDexGetResolvedString(methodClassDex, ref);
        if (strObj == NULL) {
            EXPORT_PC();
            strObj = dvmResolveString(curMethod->clazz, ref);
            if (strObj == NULL)
                GOTO_exceptionThrown();
        }
        SET_REGISTER(vdst, (u4) strObj);
    }
    FINISH(2);
OP_END

/* File: c/OP_CONST_STRING_JUMBO.c */
HANDLE_OPCODE(OP_CONST_STRING_JUMBO /*vAA, string@BBBBBBBB*/)
    {
        StringObject* strObj;
        u4 tmp;

        vdst = INST_AA(inst);
        tmp = FETCH(1);
        tmp |= (u4)FETCH(2) << 16;
        ILOGV("|const-string/jumbo v%d string@0x%08x", vdst, tmp);
        strObj = dvmDexGetResolvedString(methodClassDex, tmp);
        if (strObj == NULL) {
            EXPORT_PC();
            strObj = dvmResolveString(curMethod->clazz, tmp);
            if (strObj == NULL)
                GOTO_exceptionThrown();
        }
        SET_REGISTER(vdst, (u4) strObj);
    }
    FINISH(3);
OP_END

/* File: c/OP_CONST_CLASS.c */
HANDLE_OPCODE(OP_CONST_CLASS /*vAA, class@BBBB*/)
    {
        ClassObject* clazz;

        vdst = INST_AA(inst);
        ref = FETCH(1);
        ILOGV("|const-class v%d class@0x%04x", vdst, ref);
        clazz = dvmDexGetResolvedClass(methodClassDex, ref);
        if (clazz == NULL) {
            EXPORT_PC();
            clazz = dvmResolveClass(curMethod->clazz, ref, true);
            if (clazz == NULL)
                GOTO_exceptionThrown();
        }
        SET_REGISTER(vdst, (u4) clazz);
    }
    FINISH(2);
OP_END

/* File: c/OP_MONITOR_ENTER.c */
HANDLE_OPCODE(OP_MONITOR_ENTER /*vAA*/)
    {
        Object* obj;

        vsrc1 = INST_AA(inst);
        ILOGV("|monitor-enter v%d %s(0x%08x)",
            vsrc1, kSpacing+6, GET_REGISTER(vsrc1));
        obj = (Object*)GET_REGISTER(vsrc1);
        if (!checkForNullExportPC(obj, fp, pc))
            GOTO_exceptionThrown();
        ILOGV("+ locking %p %s\n", obj, obj->clazz->descriptor);
        EXPORT_PC();    /* need for precise GC */
        dvmLockObject(self, obj);
    }
    FINISH(1);
OP_END

/* File: c/OP_MONITOR_EXIT.c */
HANDLE_OPCODE(OP_MONITOR_EXIT /*vAA*/)
    {
        Object* obj;

        EXPORT_PC();

        vsrc1 = INST_AA(inst);
        ILOGV("|monitor-exit v%d %s(0x%08x)",
            vsrc1, kSpacing+5, GET_REGISTER(vsrc1));
        obj = (Object*)GET_REGISTER(vsrc1);
        if (!checkForNull(obj)) {
            /*
             * The exception needs to be processed at the *following*
             * instruction, not the current instruction (see the Dalvik
             * spec).  Because we're jumping to an exception handler,
             * we're not actually at risk of skipping an instruction
             * by doing so.
             */
            ADJUST_PC(1);           /* monitor-exit width is 1 */
            GOTO_exceptionThrown();
        }
        ILOGV("+ unlocking %p %s\n", obj, obj->clazz->descriptor);
        if (!dvmUnlockObject(self, obj)) {
            assert(dvmCheckException(self));
            ADJUST_PC(1);
            GOTO_exceptionThrown();
        }
    }
    FINISH(1);
OP_END

/* File: c/OP_CHECK_CAST.c */
HANDLE_OPCODE(OP_CHECK_CAST /*vAA, class@BBBB*/)
    {
        ClassObject* clazz;
        Object* obj;

        EXPORT_PC();

        vsrc1 = INST_AA(inst);
        ref = FETCH(1);         /* class to check against */
        ILOGV("|check-cast v%d,class@0x%04x", vsrc1, ref);

        obj = (Object*)GET_REGISTER(vsrc1);
        if (obj != NULL) {
#if defined(WITH_EXTRA_OBJECT_VALIDATION)
            if (!checkForNull(obj))
                GOTO_exceptionThrown();
#endif
            clazz = dvmDexGetResolvedClass(methodClassDex, ref);
            if (clazz == NULL) {
                clazz = dvmResolveClass(curMethod->clazz, ref, false);
                if (clazz == NULL)
                    GOTO_exceptionThrown();
            }
            if (!dvmInstanceof(obj->clazz, clazz)) {
                dvmThrowClassCastException(obj->clazz, clazz);
                GOTO_exceptionThrown();
            }
        }
    }
    FINISH(2);
OP_END

/* File: c/OP_INSTANCE_OF.c */
HANDLE_OPCODE(OP_INSTANCE_OF /*vA, vB, class@CCCC*/)
    {
        ClassObject* clazz;
        Object* obj;

        vdst = INST_A(inst);
        vsrc1 = INST_B(inst);   /* object to check */
        ref = FETCH(1);         /* class to check against */
        ILOGV("|instance-of v%d,v%d,class@0x%04x", vdst, vsrc1, ref);

        obj = (Object*)GET_REGISTER(vsrc1);
        if (obj == NULL) {
            SET_REGISTER(vdst, 0);
        } else {
#if defined(WITH_EXTRA_OBJECT_VALIDATION)
            if (!checkForNullExportPC(obj, fp, pc))
                GOTO_exceptionThrown();
#endif
            clazz = dvmDexGetResolvedClass(methodClassDex, ref);
            if (clazz == NULL) {
                EXPORT_PC();
                clazz = dvmResolveClass(curMethod->clazz, ref, true);
                if (clazz == NULL)
                    GOTO_exceptionThrown();
            }
            SET_REGISTER(vdst, dvmInstanceof(obj->clazz, clazz));
        }
    }
    FINISH(2);
OP_END

/* File: c/OP_ARRAY_LENGTH.c */
HANDLE_OPCODE(OP_ARRAY_LENGTH /*vA, vB*/)
    {
        ArrayObject* arrayObj;

        vdst = INST_A(inst);
        vsrc1 = INST_B(inst);
        arrayObj = (ArrayObject*) GET_REGISTER(vsrc1);
        ILOGV("|array-length v%d,v%d  (%p)", vdst, vsrc1, arrayObj);
        if (!checkForNullExportPC((Object*) arrayObj, fp, pc))
            GOTO_exceptionThrown();
        /* verifier guarantees this is an array reference */
        SET_REGISTER(vdst, arrayObj->length);
    }
    FINISH(1);
OP_END

/* File: c/OP_NEW_INSTANCE.c */
HANDLE_OPCODE(OP_NEW_INSTANCE /*vAA, class@BBBB*/)
    {
        ClassObject* clazz;
        Object* newObj;

        EXPORT_PC();

        vdst = INST_AA(inst);
        ref = FETCH(1);
        ILOGV("|new-instance v%d,class@0x%04x", vdst, ref);
        clazz = dvmDexGetResolvedClass(methodClassDex, ref);
        if (clazz == NULL) {
            clazz = dvmResolveClass(curMethod->clazz, ref, false);
            if (clazz == NULL)
                GOTO_exceptionThrown();
        }

        if (!dvmIsClassInitialized(clazz) && !dvmInitClass(clazz))
            GOTO_exceptionThrown();

        /*
         * The JIT needs dvmDexGetResolvedClass() to return non-null.
         * Since we use the portable interpreter to build the trace, this extra
         * check is not needed for mterp.
         */
        if (!dvmDexGetResolvedClass(methodClassDex, ref)) {
            /* Class initialization is still ongoing - end the trace */
            END_JIT_TSELECT();
        }

        /*
         * Verifier now tests for interface/abstract class.
         */
        //if (dvmIsInterfaceClass(clazz) || dvmIsAbstractClass(clazz)) {
        //    dvmThrowExceptionWithClassMessage("Ljava/lang/InstantiationError;",
        //        clazz->descriptor);
        //    GOTO_exceptionThrown();
        //}
        newObj = dvmAllocObject(clazz, ALLOC_DONT_TRACK);
        if (newObj == NULL)
            GOTO_exceptionThrown();
        SET_REGISTER(vdst, (u4) newObj);
    }
    FINISH(2);
OP_END

/* File: c/OP_NEW_ARRAY.c */
HANDLE_OPCODE(OP_NEW_ARRAY /*vA, vB, class@CCCC*/)
    {
        ClassObject* arrayClass;
        ArrayObject* newArray;
        s4 length;

        EXPORT_PC();

        vdst = INST_A(inst);
        vsrc1 = INST_B(inst);       /* length reg */
        ref = FETCH(1);
        ILOGV("|new-array v%d,v%d,class@0x%04x  (%d elements)",
            vdst, vsrc1, ref, (s4) GET_REGISTER(vsrc1));
        length = (s4) GET_REGISTER(vsrc1);
        if (length < 0) {
            dvmThrowException("Ljava/lang/NegativeArraySizeException;", NULL);
            GOTO_exceptionThrown();
        }
        arrayClass = dvmDexGetResolvedClass(methodClassDex, ref);
        if (arrayClass == NULL) {
            arrayClass = dvmResolveClass(curMethod->clazz, ref, false);
            if (arrayClass == NULL)
                GOTO_exceptionThrown();
        }
        /* verifier guarantees this is an array class */
        assert(dvmIsArrayClass(arrayClass));
        assert(dvmIsClassInitialized(arrayClass));

        newArray = dvmAllocArrayByClass(arrayClass, length, ALLOC_DONT_TRACK);
        if (newArray == NULL)
            GOTO_exceptionThrown();
        SET_REGISTER(vdst, (u4) newArray);
    }
    FINISH(2);
OP_END

/* File: c/OP_FILLED_NEW_ARRAY.c */
HANDLE_OPCODE(OP_FILLED_NEW_ARRAY /*vB, {vD, vE, vF, vG, vA}, class@CCCC*/)
    GOTO_invoke(filledNewArray, false, false);
OP_END

/* File: c/OP_FILLED_NEW_ARRAY_RANGE.c */
HANDLE_OPCODE(OP_FILLED_NEW_ARRAY_RANGE /*{vCCCC..v(CCCC+AA-1)}, class@BBBB*/)
    GOTO_invoke(filledNewArray, true, false);
OP_END

/* File: c/OP_FILL_ARRAY_DATA.c */
HANDLE_OPCODE(OP_FILL_ARRAY_DATA)   /*vAA, +BBBBBBBB*/
    {
        const u2* arrayData;
        s4 offset;
        ArrayObject* arrayObj;

        EXPORT_PC();
        vsrc1 = INST_AA(inst);
        offset = FETCH(1) | (((s4) FETCH(2)) << 16);
        ILOGV("|fill-array-data v%d +0x%04x", vsrc1, offset);
        arrayData = pc + offset;       // offset in 16-bit units
#ifndef NDEBUG
        if (arrayData < curMethod->insns ||
            arrayData >= curMethod->insns + dvmGetMethodInsnsSize(curMethod))
        {
            /* should have been caught in verifier */
            dvmThrowException("Ljava/lang/InternalError;",
                              "bad fill array data");
            GOTO_exceptionThrown();
        }
#endif
        arrayObj = (ArrayObject*) GET_REGISTER(vsrc1);
        if (!dvmInterpHandleFillArrayData(arrayObj, arrayData)) {
            GOTO_exceptionThrown();
        }
        FINISH(3);
    }
OP_END

/* File: c/OP_THROW.c */
HANDLE_OPCODE(OP_THROW /*vAA*/)
    {
        Object* obj;

        /*
         * We don't create an exception here, but the process of searching
         * for a catch block can do class lookups and throw exceptions.
         * We need to update the saved PC.
         */
        EXPORT_PC();

        vsrc1 = INST_AA(inst);
        ILOGV("|throw v%d  (%p)", vsrc1, (void*)GET_REGISTER(vsrc1));
        obj = (Object*) GET_REGISTER(vsrc1);
        if (!checkForNull(obj)) {
            /* will throw a null pointer exception */
            LOGVV("Bad exception\n");
        } else {
            /* use the requested exception */
            dvmSetException(self, obj);
        }
        GOTO_exceptionThrown();
    }
OP_END

/* File: c/OP_GOTO.c */
HANDLE_OPCODE(OP_GOTO /*+AA*/)
    vdst = INST_AA(inst);
    if ((s1)vdst < 0)
        ILOGV("|goto -0x%02x", -((s1)vdst));
    else
        ILOGV("|goto +0x%02x", ((s1)vdst));
    ILOGV("> branch taken");
    if ((s1)vdst < 0)
        PERIODIC_CHECKS(kInterpEntryInstr, (s1)vdst);
    FINISH((s1)vdst);
OP_END

/* File: c/OP_GOTO_16.c */
HANDLE_OPCODE(OP_GOTO_16 /*+AAAA*/)
    {
        s4 offset = (s2) FETCH(1);          /* sign-extend next code unit */

        if (offset < 0)
            ILOGV("|goto/16 -0x%04x", -offset);
        else
            ILOGV("|goto/16 +0x%04x", offset);
        ILOGV("> branch taken");
        if (offset < 0)
            PERIODIC_CHECKS(kInterpEntryInstr, offset);
        FINISH(offset);
    }
OP_END

/* File: c/OP_GOTO_32.c */
HANDLE_OPCODE(OP_GOTO_32 /*+AAAAAAAA*/)
    {
        s4 offset = FETCH(1);               /* low-order 16 bits */
        offset |= ((s4) FETCH(2)) << 16;    /* high-order 16 bits */

        if (offset < 0)
            ILOGV("|goto/32 -0x%08x", -offset);
        else
            ILOGV("|goto/32 +0x%08x", offset);
        ILOGV("> branch taken");
        if (offset <= 0)    /* allowed to branch to self */
            PERIODIC_CHECKS(kInterpEntryInstr, offset);
        FINISH(offset);
    }
OP_END

/* File: c/OP_PACKED_SWITCH.c */
HANDLE_OPCODE(OP_PACKED_SWITCH /*vAA, +BBBB*/)
    {
        const u2* switchData;
        u4 testVal;
        s4 offset;

        vsrc1 = INST_AA(inst);
        offset = FETCH(1) | (((s4) FETCH(2)) << 16);
        ILOGV("|packed-switch v%d +0x%04x", vsrc1, vsrc2);
        switchData = pc + offset;       // offset in 16-bit units
#ifndef NDEBUG
        if (switchData < curMethod->insns ||
            switchData >= curMethod->insns + dvmGetMethodInsnsSize(curMethod))
        {
            /* should have been caught in verifier */
            EXPORT_PC();
            dvmThrowException("Ljava/lang/InternalError;", "bad packed switch");
            GOTO_exceptionThrown();
        }
#endif
        testVal = GET_REGISTER(vsrc1);

        offset = dvmInterpHandlePackedSwitch(switchData, testVal);
        ILOGV("> branch taken (0x%04x)\n", offset);
        if (offset <= 0)  /* uncommon */
            PERIODIC_CHECKS(kInterpEntryInstr, offset);
        FINISH(offset);
    }
OP_END

/* File: c/OP_SPARSE_SWITCH.c */
HANDLE_OPCODE(OP_SPARSE_SWITCH /*vAA, +BBBB*/)
    {
        const u2* switchData;
        u4 testVal;
        s4 offset;

        vsrc1 = INST_AA(inst);
        offset = FETCH(1) | (((s4) FETCH(2)) << 16);
        ILOGV("|sparse-switch v%d +0x%04x", vsrc1, vsrc2);
        switchData = pc + offset;       // offset in 16-bit units
#ifndef NDEBUG
        if (switchData < curMethod->insns ||
            switchData >= curMethod->insns + dvmGetMethodInsnsSize(curMethod))
        {
            /* should have been caught in verifier */
            EXPORT_PC();
            dvmThrowException("Ljava/lang/InternalError;", "bad sparse switch");
            GOTO_exceptionThrown();
        }
#endif
        testVal = GET_REGISTER(vsrc1);

        offset = dvmInterpHandleSparseSwitch(switchData, testVal);
        ILOGV("> branch taken (0x%04x)\n", offset);
        if (offset <= 0)  /* uncommon */
            PERIODIC_CHECKS(kInterpEntryInstr, offset);
        FINISH(offset);
    }
OP_END

/* File: c/OP_CMPL_FLOAT.c */
HANDLE_OP_CMPX(OP_CMPL_FLOAT, "l-float", float, _FLOAT, -1)
OP_END

/* File: c/OP_CMPG_FLOAT.c */
HANDLE_OP_CMPX(OP_CMPG_FLOAT, "g-float", float, _FLOAT, 1)
OP_END

/* File: c/OP_CMPL_DOUBLE.c */
HANDLE_OP_CMPX(OP_CMPL_DOUBLE, "l-double", double, _DOUBLE, -1)
OP_END

/* File: c/OP_CMPG_DOUBLE.c */
HANDLE_OP_CMPX(OP_CMPG_DOUBLE, "g-double", double, _DOUBLE, 1)
OP_END

/* File: c/OP_CMP_LONG.c */
HANDLE_OP_CMPX(OP_CMP_LONG, "-long", s8, _WIDE, 0)
OP_END

/* File: c/OP_IF_EQ.c */
HANDLE_OP_IF_XX(OP_IF_EQ, "eq", ==)
OP_END

/* File: c/OP_IF_NE.c */
HANDLE_OP_IF_XX(OP_IF_NE, "ne", !=)
OP_END

/* File: c/OP_IF_LT.c */
HANDLE_OP_IF_XX(OP_IF_LT, "lt", <)
OP_END

/* File: c/OP_IF_GE.c */
HANDLE_OP_IF_XX(OP_IF_GE, "ge", >=)
OP_END

/* File: c/OP_IF_GT.c */
HANDLE_OP_IF_XX(OP_IF_GT, "gt", >)
OP_END

/* File: c/OP_IF_LE.c */
HANDLE_OP_IF_XX(OP_IF_LE, "le", <=)
OP_END

/* File: c/OP_IF_EQZ.c */
HANDLE_OP_IF_XXZ(OP_IF_EQZ, "eqz", ==)
OP_END

/* File: c/OP_IF_NEZ.c */
HANDLE_OP_IF_XXZ(OP_IF_NEZ, "nez", !=)
OP_END

/* File: c/OP_IF_LTZ.c */
HANDLE_OP_IF_XXZ(OP_IF_LTZ, "ltz", <)
OP_END

/* File: c/OP_IF_GEZ.c */
HANDLE_OP_IF_XXZ(OP_IF_GEZ, "gez", >=)
OP_END

/* File: c/OP_IF_GTZ.c */
HANDLE_OP_IF_XXZ(OP_IF_GTZ, "gtz", >)
OP_END

/* File: c/OP_IF_LEZ.c */
HANDLE_OP_IF_XXZ(OP_IF_LEZ, "lez", <=)
OP_END

/* File: c/OP_UNUSED_3E.c */
HANDLE_OPCODE(OP_UNUSED_3E)
OP_END

/* File: c/OP_UNUSED_3F.c */
HANDLE_OPCODE(OP_UNUSED_3F)
OP_END

/* File: c/OP_UNUSED_40.c */
HANDLE_OPCODE(OP_UNUSED_40)
OP_END

/* File: c/OP_UNUSED_41.c */
HANDLE_OPCODE(OP_UNUSED_41)
OP_END

/* File: c/OP_UNUSED_42.c */
HANDLE_OPCODE(OP_UNUSED_42)
OP_END

/* File: c/OP_UNUSED_43.c */
HANDLE_OPCODE(OP_UNUSED_43)
OP_END

/* File: c/OP_AGET.c */
HANDLE_OP_AGET(OP_AGET, "", u4, )
OP_END

/* File: c/OP_AGET_WIDE.c */
HANDLE_OP_AGET(OP_AGET_WIDE, "-wide", s8, _WIDE)
OP_END

/* File: c/OP_AGET_OBJECT.c */
HANDLE_OP_AGET(OP_AGET_OBJECT, "-object", u4, )
OP_END

/* File: c/OP_AGET_BOOLEAN.c */
HANDLE_OP_AGET(OP_AGET_BOOLEAN, "-boolean", u1, )
OP_END

/* File: c/OP_AGET_BYTE.c */
HANDLE_OP_AGET(OP_AGET_BYTE, "-byte", s1, )
OP_END

/* File: c/OP_AGET_CHAR.c */
HANDLE_OP_AGET(OP_AGET_CHAR, "-char", u2, )
OP_END

/* File: c/OP_AGET_SHORT.c */
HANDLE_OP_AGET(OP_AGET_SHORT, "-short", s2, )
OP_END

/* File: c/OP_APUT.c */
HANDLE_OP_APUT(OP_APUT, "", u4, )
OP_END

/* File: c/OP_APUT_WIDE.c */
HANDLE_OP_APUT(OP_APUT_WIDE, "-wide", s8, _WIDE)
OP_END

/* File: c/OP_APUT_OBJECT.c */
HANDLE_OPCODE(OP_APUT_OBJECT /*vAA, vBB, vCC*/)
    {
        ArrayObject* arrayObj;
        Object* obj;
        u2 arrayInfo;
        EXPORT_PC();
        vdst = INST_AA(inst);       /* AA: source value */
        arrayInfo = FETCH(1);
        vsrc1 = arrayInfo & 0xff;   /* BB: array ptr */
        vsrc2 = arrayInfo >> 8;     /* CC: index */
        ILOGV("|aput%s v%d,v%d,v%d", "-object", vdst, vsrc1, vsrc2);
        arrayObj = (ArrayObject*) GET_REGISTER(vsrc1);
        if (!checkForNull((Object*) arrayObj))
            GOTO_exceptionThrown();
        if (GET_REGISTER(vsrc2) >= arrayObj->length) {
            dvmThrowAIOOBE(GET_REGISTER(vsrc2), arrayObj->length);
            GOTO_exceptionThrown();
        }
        obj = (Object*) GET_REGISTER(vdst);
        if (obj != NULL) {
            if (!checkForNull(obj))
                GOTO_exceptionThrown();
            if (!dvmCanPutArrayElement(obj->clazz, arrayObj->obj.clazz)) {
                LOGV("Can't put a '%s'(%p) into array type='%s'(%p)\n",
                    obj->clazz->descriptor, obj,
                    arrayObj->obj.clazz->descriptor, arrayObj);
                dvmThrowArrayStoreException(obj->clazz, arrayObj->obj.clazz);
                GOTO_exceptionThrown();
            }
        }
        ILOGV("+ APUT[%d]=0x%08x", GET_REGISTER(vsrc2), GET_REGISTER(vdst));
        dvmSetObjectArrayElement(arrayObj,
                                 GET_REGISTER(vsrc2),
                                 (Object *)GET_REGISTER(vdst));
    }
    FINISH(2);
OP_END

/* File: c/OP_APUT_BOOLEAN.c */
HANDLE_OP_APUT(OP_APUT_BOOLEAN, "-boolean", u1, )
OP_END

/* File: c/OP_APUT_BYTE.c */
HANDLE_OP_APUT(OP_APUT_BYTE, "-byte", s1, )
OP_END

/* File: c/OP_APUT_CHAR.c */
HANDLE_OP_APUT(OP_APUT_CHAR, "-char", u2, )
OP_END

/* File: c/OP_APUT_SHORT.c */
HANDLE_OP_APUT(OP_APUT_SHORT, "-short", s2, )
OP_END

/* File: c/OP_IGET.c */
HANDLE_IGET_X(OP_IGET,                  "", Int, )
OP_END

/* File: c/OP_IGET_WIDE.c */
HANDLE_IGET_X(OP_IGET_WIDE,             "-wide", Long, _WIDE)
OP_END

/* File: c/OP_IGET_OBJECT.c */
HANDLE_IGET_X(OP_IGET_OBJECT,           "-object", Object, _AS_OBJECT)
OP_END

/* File: c/OP_IGET_BOOLEAN.c */
HANDLE_IGET_X(OP_IGET_BOOLEAN,          "", Int, )
OP_END

/* File: c/OP_IGET_BYTE.c */
HANDLE_IGET_X(OP_IGET_BYTE,             "", Int, )
OP_END

/* File: c/OP_IGET_CHAR.c */
HANDLE_IGET_X(OP_IGET_CHAR,             "", Int, )
OP_END

/* File: c/OP_IGET_SHORT.c */
HANDLE_IGET_X(OP_IGET_SHORT,            "", Int, )
OP_END

/* File: c/OP_IPUT.c */
HANDLE_IPUT_X(OP_IPUT,                  "", Int, )
OP_END

/* File: c/OP_IPUT_WIDE.c */
HANDLE_IPUT_X(OP_IPUT_WIDE,             "-wide", Long, _WIDE)
OP_END

/* File: c/OP_IPUT_OBJECT.c */
/*
 * The VM spec says we should verify that the reference being stored into
 * the field is assignment compatible.  In practice, many popular VMs don't
 * do this because it slows down a very common operation.  It's not so bad
 * for us, since "dexopt" quickens it whenever possible, but it's still an
 * issue.
 *
 * To make this spec-complaint, we'd need to add a ClassObject pointer to
 * the Field struct, resolve the field's type descriptor at link or class
 * init time, and then verify the type here.
 */
HANDLE_IPUT_X(OP_IPUT_OBJECT,           "-object", Object, _AS_OBJECT)
OP_END

/* File: c/OP_IPUT_BOOLEAN.c */
HANDLE_IPUT_X(OP_IPUT_BOOLEAN,          "", Int, )
OP_END

/* File: c/OP_IPUT_BYTE.c */
HANDLE_IPUT_X(OP_IPUT_BYTE,             "", Int, )
OP_END

/* File: c/OP_IPUT_CHAR.c */
HANDLE_IPUT_X(OP_IPUT_CHAR,             "", Int, )
OP_END

/* File: c/OP_IPUT_SHORT.c */
HANDLE_IPUT_X(OP_IPUT_SHORT,            "", Int, )
OP_END

/* File: c/OP_SGET.c */
HANDLE_SGET_X(OP_SGET,                  "", Int, )
OP_END

/* File: c/OP_SGET_WIDE.c */
HANDLE_SGET_X(OP_SGET_WIDE,             "-wide", Long, _WIDE)
OP_END

/* File: c/OP_SGET_OBJECT.c */
HANDLE_SGET_X(OP_SGET_OBJECT,           "-object", Object, _AS_OBJECT)
OP_END

/* File: c/OP_SGET_BOOLEAN.c */
HANDLE_SGET_X(OP_SGET_BOOLEAN,          "", Int, )
OP_END

/* File: c/OP_SGET_BYTE.c */
HANDLE_SGET_X(OP_SGET_BYTE,             "", Int, )
OP_END

/* File: c/OP_SGET_CHAR.c */
HANDLE_SGET_X(OP_SGET_CHAR,             "", Int, )
OP_END

/* File: c/OP_SGET_SHORT.c */
HANDLE_SGET_X(OP_SGET_SHORT,            "", Int, )
OP_END

/* File: c/OP_SPUT.c */
HANDLE_SPUT_X(OP_SPUT,                  "", Int, )
OP_END

/* File: c/OP_SPUT_WIDE.c */
HANDLE_SPUT_X(OP_SPUT_WIDE,             "-wide", Long, _WIDE)
OP_END

/* File: c/OP_SPUT_OBJECT.c */
HANDLE_SPUT_X(OP_SPUT_OBJECT,           "-object", Object, _AS_OBJECT)
OP_END

/* File: c/OP_SPUT_BOOLEAN.c */
HANDLE_SPUT_X(OP_SPUT_BOOLEAN,          "", Int, )
OP_END

/* File: c/OP_SPUT_BYTE.c */
HANDLE_SPUT_X(OP_SPUT_BYTE,             "", Int, )
OP_END

/* File: c/OP_SPUT_CHAR.c */
HANDLE_SPUT_X(OP_SPUT_CHAR,             "", Int, )
OP_END

/* File: c/OP_SPUT_SHORT.c */
HANDLE_SPUT_X(OP_SPUT_SHORT,            "", Int, )
OP_END

/* File: c/OP_INVOKE_VIRTUAL.c */
HANDLE_OPCODE(OP_INVOKE_VIRTUAL /*vB, {vD, vE, vF, vG, vA}, meth@CCCC*/)
    GOTO_invoke(invokeVirtual, false, false);
OP_END

/* File: c/OP_INVOKE_SUPER.c */
HANDLE_OPCODE(OP_INVOKE_SUPER /*vB, {vD, vE, vF, vG, vA}, meth@CCCC*/)
    GOTO_invoke(invokeSuper, false, false);
OP_END

/* File: c/OP_INVOKE_DIRECT.c */
HANDLE_OPCODE(OP_INVOKE_DIRECT /*vB, {vD, vE, vF, vG, vA}, meth@CCCC*/)
    GOTO_invoke(invokeDirect, false, false);
OP_END

/* File: c/OP_INVOKE_STATIC.c */
HANDLE_OPCODE(OP_INVOKE_STATIC /*vB, {vD, vE, vF, vG, vA}, meth@CCCC*/)
    GOTO_invoke(invokeStatic, false, false);
OP_END

/* File: c/OP_INVOKE_INTERFACE.c */
HANDLE_OPCODE(OP_INVOKE_INTERFACE /*vB, {vD, vE, vF, vG, vA}, meth@CCCC*/)
    GOTO_invoke(invokeInterface, false, false);
OP_END

/* File: c/OP_UNUSED_73.c */
HANDLE_OPCODE(OP_UNUSED_73)
OP_END

/* File: c/OP_INVOKE_VIRTUAL_RANGE.c */
HANDLE_OPCODE(OP_INVOKE_VIRTUAL_RANGE /*{vCCCC..v(CCCC+AA-1)}, meth@BBBB*/)
    GOTO_invoke(invokeVirtual, true, false);
OP_END

/* File: c/OP_INVOKE_SUPER_RANGE.c */
HANDLE_OPCODE(OP_INVOKE_SUPER_RANGE /*{vCCCC..v(CCCC+AA-1)}, meth@BBBB*/)
    GOTO_invoke(invokeSuper, true, false);
OP_END

/* File: c/OP_INVOKE_DIRECT_RANGE.c */
HANDLE_OPCODE(OP_INVOKE_DIRECT_RANGE /*{vCCCC..v(CCCC+AA-1)}, meth@BBBB*/)
    GOTO_invoke(invokeDirect, true, false);
OP_END

/* File: c/OP_INVOKE_STATIC_RANGE.c */
HANDLE_OPCODE(OP_INVOKE_STATIC_RANGE /*{vCCCC..v(CCCC+AA-1)}, meth@BBBB*/)
    GOTO_invoke(invokeStatic, true, false);
OP_END

/* File: c/OP_INVOKE_INTERFACE_RANGE.c */
HANDLE_OPCODE(OP_INVOKE_INTERFACE_RANGE /*{vCCCC..v(CCCC+AA-1)}, meth@BBBB*/)
    GOTO_invoke(invokeInterface, true, false);
OP_END

/* File: c/OP_UNUSED_79.c */
HANDLE_OPCODE(OP_UNUSED_79)
OP_END

/* File: c/OP_UNUSED_7A.c */
HANDLE_OPCODE(OP_UNUSED_7A)
OP_END

/* File: c/OP_NEG_INT.c */
HANDLE_UNOP(OP_NEG_INT, "neg-int", -, , )
OP_END

/* File: c/OP_NOT_INT.c */
HANDLE_UNOP(OP_NOT_INT, "not-int", , ^ 0xffffffff, )
OP_END

/* File: c/OP_NEG_LONG.c */
HANDLE_UNOP(OP_NEG_LONG, "neg-long", -, , _WIDE)
OP_END

/* File: c/OP_NOT_LONG.c */
HANDLE_UNOP(OP_NOT_LONG, "not-long", , ^ 0xffffffffffffffffULL, _WIDE)
OP_END

/* File: c/OP_NEG_FLOAT.c */
HANDLE_UNOP(OP_NEG_FLOAT, "neg-float", -, , _FLOAT)
OP_END

/* File: c/OP_NEG_DOUBLE.c */
HANDLE_UNOP(OP_NEG_DOUBLE, "neg-double", -, , _DOUBLE)
OP_END

/* File: c/OP_INT_TO_LONG.c */
HANDLE_NUMCONV(OP_INT_TO_LONG,          "int-to-long", _INT, _WIDE)
OP_END

/* File: c/OP_INT_TO_FLOAT.c */
HANDLE_NUMCONV(OP_INT_TO_FLOAT,         "int-to-float", _INT, _FLOAT)
OP_END

/* File: c/OP_INT_TO_DOUBLE.c */
HANDLE_NUMCONV(OP_INT_TO_DOUBLE,        "int-to-double", _INT, _DOUBLE)
OP_END

/* File: c/OP_LONG_TO_INT.c */
HANDLE_NUMCONV(OP_LONG_TO_INT,          "long-to-int", _WIDE, _INT)
OP_END

/* File: c/OP_LONG_TO_FLOAT.c */
HANDLE_NUMCONV(OP_LONG_TO_FLOAT,        "long-to-float", _WIDE, _FLOAT)
OP_END

/* File: c/OP_LONG_TO_DOUBLE.c */
HANDLE_NUMCONV(OP_LONG_TO_DOUBLE,       "long-to-double", _WIDE, _DOUBLE)
OP_END

/* File: c/OP_FLOAT_TO_INT.c */
HANDLE_FLOAT_TO_INT(OP_FLOAT_TO_INT,    "float-to-int",
    float, _FLOAT, s4, _INT)
OP_END

/* File: c/OP_FLOAT_TO_LONG.c */
HANDLE_FLOAT_TO_INT(OP_FLOAT_TO_LONG,   "float-to-long",
    float, _FLOAT, s8, _WIDE)
OP_END

/* File: c/OP_FLOAT_TO_DOUBLE.c */
HANDLE_NUMCONV(OP_FLOAT_TO_DOUBLE,      "float-to-double", _FLOAT, _DOUBLE)
OP_END

/* File: c/OP_DOUBLE_TO_INT.c */
HANDLE_FLOAT_TO_INT(OP_DOUBLE_TO_INT,   "double-to-int",
    double, _DOUBLE, s4, _INT)
OP_END

/* File: c/OP_DOUBLE_TO_LONG.c */
HANDLE_FLOAT_TO_INT(OP_DOUBLE_TO_LONG,  "double-to-long",
    double, _DOUBLE, s8, _WIDE)
OP_END

/* File: c/OP_DOUBLE_TO_FLOAT.c */
HANDLE_NUMCONV(OP_DOUBLE_TO_FLOAT,      "double-to-float", _DOUBLE, _FLOAT)
OP_END

/* File: c/OP_INT_TO_BYTE.c */
HANDLE_INT_TO_SMALL(OP_INT_TO_BYTE,     "byte", s1)
OP_END

/* File: c/OP_INT_TO_CHAR.c */
HANDLE_INT_TO_SMALL(OP_INT_TO_CHAR,     "char", u2)
OP_END

/* File: c/OP_INT_TO_SHORT.c */
HANDLE_INT_TO_SMALL(OP_INT_TO_SHORT,    "short", s2)    /* want sign bit */
OP_END

/* File: c/OP_ADD_INT.c */
HANDLE_OP_X_INT(OP_ADD_INT, "add", +, 0)
OP_END

/* File: c/OP_SUB_INT.c */
HANDLE_OP_X_INT(OP_SUB_INT, "sub", -, 0)
OP_END

/* File: c/OP_MUL_INT.c */
HANDLE_OP_X_INT(OP_MUL_INT, "mul", *, 0)
OP_END

/* File: c/OP_DIV_INT.c */
HANDLE_OP_X_INT(OP_DIV_INT, "div", /, 1)
OP_END

/* File: c/OP_REM_INT.c */
HANDLE_OP_X_INT(OP_REM_INT, "rem", %, 2)
OP_END

/* File: c/OP_AND_INT.c */
HANDLE_OP_X_INT(OP_AND_INT, "and", &, 0)
OP_END

/* File: c/OP_OR_INT.c */
HANDLE_OP_X_INT(OP_OR_INT,  "or",  |, 0)
OP_END

/* File: c/OP_XOR_INT.c */
HANDLE_OP_X_INT(OP_XOR_INT, "xor", ^, 0)
OP_END

/* File: c/OP_SHL_INT.c */
HANDLE_OP_SHX_INT(OP_SHL_INT, "shl", (s4), <<)
OP_END

/* File: c/OP_SHR_INT.c */
HANDLE_OP_SHX_INT(OP_SHR_INT, "shr", (s4), >>)
OP_END

/* File: c/OP_USHR_INT.c */
HANDLE_OP_SHX_INT(OP_USHR_INT, "ushr", (u4), >>)
OP_END

/* File: c/OP_ADD_LONG.c */
HANDLE_OP_X_LONG(OP_ADD_LONG, "add", +, 0)
OP_END

/* File: c/OP_SUB_LONG.c */
HANDLE_OP_X_LONG(OP_SUB_LONG, "sub", -, 0)
OP_END

/* File: c/OP_MUL_LONG.c */
HANDLE_OP_X_LONG(OP_MUL_LONG, "mul", *, 0)
OP_END

/* File: c/OP_DIV_LONG.c */
HANDLE_OP_X_LONG(OP_DIV_LONG, "div", /, 1)
OP_END

/* File: c/OP_REM_LONG.c */
HANDLE_OP_X_LONG(OP_REM_LONG, "rem", %, 2)
OP_END

/* File: c/OP_AND_LONG.c */
HANDLE_OP_X_LONG(OP_AND_LONG, "and", &, 0)
OP_END

/* File: c/OP_OR_LONG.c */
HANDLE_OP_X_LONG(OP_OR_LONG,  "or", |, 0)
OP_END

/* File: c/OP_XOR_LONG.c */
HANDLE_OP_X_LONG(OP_XOR_LONG, "xor", ^, 0)
OP_END

/* File: c/OP_SHL_LONG.c */
HANDLE_OP_SHX_LONG(OP_SHL_LONG, "shl", (s8), <<)
OP_END

/* File: c/OP_SHR_LONG.c */
HANDLE_OP_SHX_LONG(OP_SHR_LONG, "shr", (s8), >>)
OP_END

/* File: c/OP_USHR_LONG.c */
HANDLE_OP_SHX_LONG(OP_USHR_LONG, "ushr", (u8), >>)
OP_END

/* File: c/OP_ADD_FLOAT.c */
HANDLE_OP_X_FLOAT(OP_ADD_FLOAT, "add", +)
OP_END

/* File: c/OP_SUB_FLOAT.c */
HANDLE_OP_X_FLOAT(OP_SUB_FLOAT, "sub", -)
OP_END

/* File: c/OP_MUL_FLOAT.c */
HANDLE_OP_X_FLOAT(OP_MUL_FLOAT, "mul", *)
OP_END

/* File: c/OP_DIV_FLOAT.c */
HANDLE_OP_X_FLOAT(OP_DIV_FLOAT, "div", /)
OP_END

/* File: c/OP_REM_FLOAT.c */
HANDLE_OPCODE(OP_REM_FLOAT /*vAA, vBB, vCC*/)
    {
        u2 srcRegs;
        vdst = INST_AA(inst);
        srcRegs = FETCH(1);
        vsrc1 = srcRegs & 0xff;
        vsrc2 = srcRegs >> 8;
        ILOGV("|%s-float v%d,v%d,v%d", "mod", vdst, vsrc1, vsrc2);
        SET_REGISTER_FLOAT(vdst,
            fmodf(GET_REGISTER_FLOAT(vsrc1), GET_REGISTER_FLOAT(vsrc2)));
    }
    FINISH(2);
OP_END

/* File: c/OP_ADD_DOUBLE.c */
HANDLE_OP_X_DOUBLE(OP_ADD_DOUBLE, "add", +)
OP_END

/* File: c/OP_SUB_DOUBLE.c */
HANDLE_OP_X_DOUBLE(OP_SUB_DOUBLE, "sub", -)
OP_END

/* File: c/OP_MUL_DOUBLE.c */
HANDLE_OP_X_DOUBLE(OP_MUL_DOUBLE, "mul", *)
OP_END

/* File: c/OP_DIV_DOUBLE.c */
HANDLE_OP_X_DOUBLE(OP_DIV_DOUBLE, "div", /)
OP_END

/* File: c/OP_REM_DOUBLE.c */
HANDLE_OPCODE(OP_REM_DOUBLE /*vAA, vBB, vCC*/)
    {
        u2 srcRegs;
        vdst = INST_AA(inst);
        srcRegs = FETCH(1);
        vsrc1 = srcRegs & 0xff;
        vsrc2 = srcRegs >> 8;
        ILOGV("|%s-double v%d,v%d,v%d", "mod", vdst, vsrc1, vsrc2);
        SET_REGISTER_DOUBLE(vdst,
            fmod(GET_REGISTER_DOUBLE(vsrc1), GET_REGISTER_DOUBLE(vsrc2)));
    }
    FINISH(2);
OP_END

/* File: c/OP_ADD_INT_2ADDR.c */
HANDLE_OP_X_INT_2ADDR(OP_ADD_INT_2ADDR, "add", +, 0)
OP_END

/* File: c/OP_SUB_INT_2ADDR.c */
HANDLE_OP_X_INT_2ADDR(OP_SUB_INT_2ADDR, "sub", -, 0)
OP_END

/* File: c/OP_MUL_INT_2ADDR.c */
HANDLE_OP_X_INT_2ADDR(OP_MUL_INT_2ADDR, "mul", *, 0)
OP_END

/* File: c/OP_DIV_INT_2ADDR.c */
HANDLE_OP_X_INT_2ADDR(OP_DIV_INT_2ADDR, "div", /, 1)
OP_END

/* File: c/OP_REM_INT_2ADDR.c */
HANDLE_OP_X_INT_2ADDR(OP_REM_INT_2ADDR, "rem", %, 2)
OP_END

/* File: c/OP_AND_INT_2ADDR.c */
HANDLE_OP_X_INT_2ADDR(OP_AND_INT_2ADDR, "and", &, 0)
OP_END

/* File: c/OP_OR_INT_2ADDR.c */
HANDLE_OP_X_INT_2ADDR(OP_OR_INT_2ADDR,  "or", |, 0)
OP_END

/* File: c/OP_XOR_INT_2ADDR.c */
HANDLE_OP_X_INT_2ADDR(OP_XOR_INT_2ADDR, "xor", ^, 0)
OP_END

/* File: c/OP_SHL_INT_2ADDR.c */
HANDLE_OP_SHX_INT_2ADDR(OP_SHL_INT_2ADDR, "shl", (s4), <<)
OP_END

/* File: c/OP_SHR_INT_2ADDR.c */
HANDLE_OP_SHX_INT_2ADDR(OP_SHR_INT_2ADDR, "shr", (s4), >>)
OP_END

/* File: c/OP_USHR_INT_2ADDR.c */
HANDLE_OP_SHX_INT_2ADDR(OP_USHR_INT_2ADDR, "ushr", (u4), >>)
OP_END

/* File: c/OP_ADD_LONG_2ADDR.c */
HANDLE_OP_X_LONG_2ADDR(OP_ADD_LONG_2ADDR, "add", +, 0)
OP_END

/* File: c/OP_SUB_LONG_2ADDR.c */
HANDLE_OP_X_LONG_2ADDR(OP_SUB_LONG_2ADDR, "sub", -, 0)
OP_END

/* File: c/OP_MUL_LONG_2ADDR.c */
HANDLE_OP_X_LONG_2ADDR(OP_MUL_LONG_2ADDR, "mul", *, 0)
OP_END

/* File: c/OP_DIV_LONG_2ADDR.c */
HANDLE_OP_X_LONG_2ADDR(OP_DIV_LONG_2ADDR, "div", /, 1)
OP_END

/* File: c/OP_REM_LONG_2ADDR.c */
HANDLE_OP_X_LONG_2ADDR(OP_REM_LONG_2ADDR, "rem", %, 2)
OP_END

/* File: c/OP_AND_LONG_2ADDR.c */
HANDLE_OP_X_LONG_2ADDR(OP_AND_LONG_2ADDR, "and", &, 0)
OP_END

/* File: c/OP_OR_LONG_2ADDR.c */
HANDLE_OP_X_LONG_2ADDR(OP_OR_LONG_2ADDR,  "or", |, 0)
OP_END

/* File: c/OP_XOR_LONG_2ADDR.c */
HANDLE_OP_X_LONG_2ADDR(OP_XOR_LONG_2ADDR, "xor", ^, 0)
OP_END

/* File: c/OP_SHL_LONG_2ADDR.c */
HANDLE_OP_SHX_LONG_2ADDR(OP_SHL_LONG_2ADDR, "shl", (s8), <<)
OP_END

/* File: c/OP_SHR_LONG_2ADDR.c */
HANDLE_OP_SHX_LONG_2ADDR(OP_SHR_LONG_2ADDR, "shr", (s8), >>)
OP_END

/* File: c/OP_USHR_LONG_2ADDR.c */
HANDLE_OP_SHX_LONG_2ADDR(OP_USHR_LONG_2ADDR, "ushr", (u8), >>)
OP_END

/* File: c/OP_ADD_FLOAT_2ADDR.c */
HANDLE_OP_X_FLOAT_2ADDR(OP_ADD_FLOAT_2ADDR, "add", +)
OP_END

/* File: c/OP_SUB_FLOAT_2ADDR.c */
HANDLE_OP_X_FLOAT_2ADDR(OP_SUB_FLOAT_2ADDR, "sub", -)
OP_END

/* File: c/OP_MUL_FLOAT_2ADDR.c */
HANDLE_OP_X_FLOAT_2ADDR(OP_MUL_FLOAT_2ADDR, "mul", *)
OP_END

/* File: c/OP_DIV_FLOAT_2ADDR.c */
HANDLE_OP_X_FLOAT_2ADDR(OP_DIV_FLOAT_2ADDR, "div", /)
OP_END

/* File: c/OP_REM_FLOAT_2ADDR.c */
HANDLE_OPCODE(OP_REM_FLOAT_2ADDR /*vA, vB*/)
    vdst = INST_A(inst);
    vsrc1 = INST_B(inst);
    ILOGV("|%s-float-2addr v%d,v%d", "mod", vdst, vsrc1);
    SET_REGISTER_FLOAT(vdst,
        fmodf(GET_REGISTER_FLOAT(vdst), GET_REGISTER_FLOAT(vsrc1)));
    FINISH(1);
OP_END

/* File: c/OP_ADD_DOUBLE_2ADDR.c */
HANDLE_OP_X_DOUBLE_2ADDR(OP_ADD_DOUBLE_2ADDR, "add", +)
OP_END

/* File: c/OP_SUB_DOUBLE_2ADDR.c */
HANDLE_OP_X_DOUBLE_2ADDR(OP_SUB_DOUBLE_2ADDR, "sub", -)
OP_END

/* File: c/OP_MUL_DOUBLE_2ADDR.c */
HANDLE_OP_X_DOUBLE_2ADDR(OP_MUL_DOUBLE_2ADDR, "mul", *)
OP_END

/* File: c/OP_DIV_DOUBLE_2ADDR.c */
HANDLE_OP_X_DOUBLE_2ADDR(OP_DIV_DOUBLE_2ADDR, "div", /)
OP_END

/* File: c/OP_REM_DOUBLE_2ADDR.c */
HANDLE_OPCODE(OP_REM_DOUBLE_2ADDR /*vA, vB*/)
    vdst = INST_A(inst);
    vsrc1 = INST_B(inst);
    ILOGV("|%s-double-2addr v%d,v%d", "mod", vdst, vsrc1);
    SET_REGISTER_DOUBLE(vdst,
        fmod(GET_REGISTER_DOUBLE(vdst), GET_REGISTER_DOUBLE(vsrc1)));
    FINISH(1);
OP_END

/* File: c/OP_ADD_INT_LIT16.c */
HANDLE_OP_X_INT_LIT16(OP_ADD_INT_LIT16, "add", +, 0)
OP_END

/* File: c/OP_RSUB_INT.c */
HANDLE_OPCODE(OP_RSUB_INT /*vA, vB, #+CCCC*/)
    {
        vdst = INST_A(inst);
        vsrc1 = INST_B(inst);
        vsrc2 = FETCH(1);
        ILOGV("|rsub-int v%d,v%d,#+0x%04x", vdst, vsrc1, vsrc2);
        SET_REGISTER(vdst, (s2) vsrc2 - (s4) GET_REGISTER(vsrc1));
    }
    FINISH(2);
OP_END

/* File: c/OP_MUL_INT_LIT16.c */
HANDLE_OP_X_INT_LIT16(OP_MUL_INT_LIT16, "mul", *, 0)
OP_END

/* File: c/OP_DIV_INT_LIT16.c */
HANDLE_OP_X_INT_LIT16(OP_DIV_INT_LIT16, "div", /, 1)
OP_END

/* File: c/OP_REM_INT_LIT16.c */
HANDLE_OP_X_INT_LIT16(OP_REM_INT_LIT16, "rem", %, 2)
OP_END

/* File: c/OP_AND_INT_LIT16.c */
HANDLE_OP_X_INT_LIT16(OP_AND_INT_LIT16, "and", &, 0)
OP_END

/* File: c/OP_OR_INT_LIT16.c */
HANDLE_OP_X_INT_LIT16(OP_OR_INT_LIT16,  "or",  |, 0)
OP_END

/* File: c/OP_XOR_INT_LIT16.c */
HANDLE_OP_X_INT_LIT16(OP_XOR_INT_LIT16, "xor", ^, 0)
OP_END

/* File: c/OP_ADD_INT_LIT8.c */
HANDLE_OP_X_INT_LIT8(OP_ADD_INT_LIT8,   "add", +, 0)
OP_END

/* File: c/OP_RSUB_INT_LIT8.c */
HANDLE_OPCODE(OP_RSUB_INT_LIT8 /*vAA, vBB, #+CC*/)
    {
        u2 litInfo;
        vdst = INST_AA(inst);
        litInfo = FETCH(1);
        vsrc1 = litInfo & 0xff;
        vsrc2 = litInfo >> 8;
        ILOGV("|%s-int/lit8 v%d,v%d,#+0x%02x", "rsub", vdst, vsrc1, vsrc2);
        SET_REGISTER(vdst, (s1) vsrc2 - (s4) GET_REGISTER(vsrc1));
    }
    FINISH(2);
OP_END

/* File: c/OP_MUL_INT_LIT8.c */
HANDLE_OP_X_INT_LIT8(OP_MUL_INT_LIT8,   "mul", *, 0)
OP_END

/* File: c/OP_DIV_INT_LIT8.c */
HANDLE_OP_X_INT_LIT8(OP_DIV_INT_LIT8,   "div", /, 1)
OP_END

/* File: c/OP_REM_INT_LIT8.c */
HANDLE_OP_X_INT_LIT8(OP_REM_INT_LIT8,   "rem", %, 2)
OP_END

/* File: c/OP_AND_INT_LIT8.c */
HANDLE_OP_X_INT_LIT8(OP_AND_INT_LIT8,   "and", &, 0)
OP_END

/* File: c/OP_OR_INT_LIT8.c */
HANDLE_OP_X_INT_LIT8(OP_OR_INT_LIT8,    "or",  |, 0)
OP_END

/* File: c/OP_XOR_INT_LIT8.c */
HANDLE_OP_X_INT_LIT8(OP_XOR_INT_LIT8,   "xor", ^, 0)
OP_END

/* File: c/OP_SHL_INT_LIT8.c */
HANDLE_OP_SHX_INT_LIT8(OP_SHL_INT_LIT8,   "shl", (s4), <<)
OP_END

/* File: c/OP_SHR_INT_LIT8.c */
HANDLE_OP_SHX_INT_LIT8(OP_SHR_INT_LIT8,   "shr", (s4), >>)
OP_END

/* File: c/OP_USHR_INT_LIT8.c */
HANDLE_OP_SHX_INT_LIT8(OP_USHR_INT_LIT8,  "ushr", (u4), >>)
OP_END

/* File: c/OP_IGET_VOLATILE.c */
HANDLE_IGET_X(OP_IGET_VOLATILE,         "-volatile", IntVolatile, )
OP_END

/* File: c/OP_IPUT_VOLATILE.c */
HANDLE_IPUT_X(OP_IPUT_VOLATILE,         "-volatile", IntVolatile, )
OP_END

/* File: c/OP_SGET_VOLATILE.c */
HANDLE_SGET_X(OP_SGET_VOLATILE,         "-volatile", IntVolatile, )
OP_END

/* File: c/OP_SPUT_VOLATILE.c */
HANDLE_SPUT_X(OP_SPUT_VOLATILE,         "-volatile", IntVolatile, )
OP_END

/* File: c/OP_IGET_OBJECT_VOLATILE.c */
HANDLE_IGET_X(OP_IGET_OBJECT_VOLATILE,  "-object-volatile", ObjectVolatile, _AS_OBJECT)
OP_END

/* File: c/OP_IGET_WIDE_VOLATILE.c */
HANDLE_IGET_X(OP_IGET_WIDE_VOLATILE,    "-wide-volatile", LongVolatile, _WIDE)
OP_END

/* File: c/OP_IPUT_WIDE_VOLATILE.c */
HANDLE_IPUT_X(OP_IPUT_WIDE_VOLATILE,    "-wide-volatile", LongVolatile, _WIDE)
OP_END

/* File: c/OP_SGET_WIDE_VOLATILE.c */
HANDLE_SGET_X(OP_SGET_WIDE_VOLATILE,    "-wide-volatile", LongVolatile, _WIDE)
OP_END

/* File: c/OP_SPUT_WIDE_VOLATILE.c */
HANDLE_SPUT_X(OP_SPUT_WIDE_VOLATILE,    "-wide-volatile", LongVolatile, _WIDE)
OP_END

/* File: c/OP_BREAKPOINT.c */
HANDLE_OPCODE(OP_BREAKPOINT)
#if (INTERP_TYPE == INTERP_DBG)
    {
        /*
         * Restart this instruction with the original opcode.  We do
         * this by simply jumping to the handler.
         *
         * It's probably not necessary to update "inst", but we do it
         * for the sake of anything that needs to do disambiguation in a
         * common handler with INST_INST.
         *
         * The breakpoint itself is handled over in updateDebugger(),
         * because we need to detect other events (method entry, single
         * step) and report them in the same event packet, and we're not
         * yet handling those through breakpoint instructions.  By the
         * time we get here, the breakpoint has already been handled and
         * the thread resumed.
         */
        u1 originalOpcode = dvmGetOriginalOpcode(pc);
        LOGV("+++ break 0x%02x (0x%04x -> 0x%04x)\n", originalOpcode, inst,
            INST_REPLACE_OP(inst, originalOpcode));
        inst = INST_REPLACE_OP(inst, originalOpcode);
        FINISH_BKPT(originalOpcode);
    }
#else
    LOGE("Breakpoint hit in non-debug interpreter\n");
    dvmAbort();
#endif
OP_END

/* File: c/OP_THROW_VERIFICATION_ERROR.c */
HANDLE_OPCODE(OP_THROW_VERIFICATION_ERROR)
    EXPORT_PC();
    vsrc1 = INST_AA(inst);
    ref = FETCH(1);             /* class/field/method ref */
    dvmThrowVerificationError(curMethod, vsrc1, ref);
    GOTO_exceptionThrown();
OP_END

/* File: c/OP_EXECUTE_INLINE.c */
HANDLE_OPCODE(OP_EXECUTE_INLINE /*vB, {vD, vE, vF, vG}, inline@CCCC*/)
    {
        /*
         * This has the same form as other method calls, but we ignore
         * the 5th argument (vA).  This is chiefly because the first four
         * arguments to a function on ARM are in registers.
         *
         * We only set the arguments that are actually used, leaving
         * the rest uninitialized.  We're assuming that, if the method
         * needs them, they'll be specified in the call.
         *
         * However, this annoys gcc when optimizations are enabled,
         * causing a "may be used uninitialized" warning.  Quieting
         * the warnings incurs a slight penalty (5%: 373ns vs. 393ns
         * on empty method).  Note that valgrind is perfectly happy
         * either way as the uninitialiezd values are never actually
         * used.
         */
        u4 arg0, arg1, arg2, arg3;
        arg0 = arg1 = arg2 = arg3 = 0;

        EXPORT_PC();

        vsrc1 = INST_B(inst);       /* #of args */
        ref = FETCH(1);             /* inline call "ref" */
        vdst = FETCH(2);            /* 0-4 register indices */
        ILOGV("|execute-inline args=%d @%d {regs=0x%04x}",
            vsrc1, ref, vdst);

        assert((vdst >> 16) == 0);  // 16-bit type -or- high 16 bits clear
        assert(vsrc1 <= 4);

        switch (vsrc1) {
        case 4:
            arg3 = GET_REGISTER(vdst >> 12);
            /* fall through */
        case 3:
            arg2 = GET_REGISTER((vdst & 0x0f00) >> 8);
            /* fall through */
        case 2:
            arg1 = GET_REGISTER((vdst & 0x00f0) >> 4);
            /* fall through */
        case 1:
            arg0 = GET_REGISTER(vdst & 0x0f);
            /* fall through */
        default:        // case 0
            ;
        }

#if INTERP_TYPE == INTERP_DBG
        if (!dvmPerformInlineOp4Dbg(arg0, arg1, arg2, arg3, &retval, ref))
            GOTO_exceptionThrown();
#else
        if (!dvmPerformInlineOp4Std(arg0, arg1, arg2, arg3, &retval, ref))
            GOTO_exceptionThrown();
#endif
    }
    FINISH(3);
OP_END

/* File: c/OP_EXECUTE_INLINE_RANGE.c */
HANDLE_OPCODE(OP_EXECUTE_INLINE_RANGE /*{vCCCC..v(CCCC+AA-1)}, inline@BBBB*/)
    {
        u4 arg0, arg1, arg2, arg3;
        arg0 = arg1 = arg2 = arg3 = 0;      /* placate gcc */

        EXPORT_PC();

        vsrc1 = INST_AA(inst);      /* #of args */
        ref = FETCH(1);             /* inline call "ref" */
        vdst = FETCH(2);            /* range base */
        ILOGV("|execute-inline-range args=%d @%d {regs=v%d-v%d}",
            vsrc1, ref, vdst, vdst+vsrc1-1);

        assert((vdst >> 16) == 0);  // 16-bit type -or- high 16 bits clear
        assert(vsrc1 <= 4);

        switch (vsrc1) {
        case 4:
            arg3 = GET_REGISTER(vdst+3);
            /* fall through */
        case 3:
            arg2 = GET_REGISTER(vdst+2);
            /* fall through */
        case 2:
            arg1 = GET_REGISTER(vdst+1);
            /* fall through */
        case 1:
            arg0 = GET_REGISTER(vdst+0);
            /* fall through */
        default:        // case 0
            ;
        }

#if INTERP_TYPE == INTERP_DBG
        if (!dvmPerformInlineOp4Dbg(arg0, arg1, arg2, arg3, &retval, ref))
            GOTO_exceptionThrown();
#else
        if (!dvmPerformInlineOp4Std(arg0, arg1, arg2, arg3, &retval, ref))
            GOTO_exceptionThrown();
#endif
    }
    FINISH(3);
OP_END

/* File: c/OP_INVOKE_DIRECT_EMPTY.c */
HANDLE_OPCODE(OP_INVOKE_DIRECT_EMPTY /*vB, {vD, vE, vF, vG, vA}, meth@CCCC*/)
#if INTERP_TYPE != INTERP_DBG
    //LOGI("Ignoring empty\n");
    FINISH(3);
#else
    if (!DEBUGGER_ACTIVE) {
        //LOGI("Skipping empty\n");
        FINISH(3);      // don't want it to show up in profiler output
    } else {
        //LOGI("Running empty\n");
        /* fall through to OP_INVOKE_DIRECT */
        GOTO_invoke(invokeDirect, false, false);
    }
#endif
OP_END

/* File: c/OP_RETURN_VOID_BARRIER.c */
HANDLE_OPCODE(OP_RETURN_VOID_BARRIER /**/)
    ILOGV("|return-void");
#ifndef NDEBUG
    retval.j = 0xababababULL;   /* placate valgrind */
#endif
    ANDROID_MEMBAR_STORE();
    GOTO_returnFromMethod();
OP_END

/* File: c/OP_IGET_QUICK.c */
HANDLE_IGET_X_QUICK(OP_IGET_QUICK,          "", Int, )
OP_END

/* File: c/OP_IGET_WIDE_QUICK.c */
HANDLE_IGET_X_QUICK(OP_IGET_WIDE_QUICK,     "-wide", Long, _WIDE)
OP_END

/* File: c/OP_IGET_OBJECT_QUICK.c */
HANDLE_IGET_X_QUICK(OP_IGET_OBJECT_QUICK,   "-object", Object, _AS_OBJECT)
OP_END

/* File: c/OP_IPUT_QUICK.c */
HANDLE_IPUT_X_QUICK(OP_IPUT_QUICK,          "", Int, )
OP_END

/* File: c/OP_IPUT_WIDE_QUICK.c */
HANDLE_IPUT_X_QUICK(OP_IPUT_WIDE_QUICK,     "-wide", Long, _WIDE)
OP_END

/* File: c/OP_IPUT_OBJECT_QUICK.c */
HANDLE_IPUT_X_QUICK(OP_IPUT_OBJECT_QUICK,   "-object", Object, _AS_OBJECT)
OP_END

/* File: c/OP_INVOKE_VIRTUAL_QUICK.c */
HANDLE_OPCODE(OP_INVOKE_VIRTUAL_QUICK /*vB, {vD, vE, vF, vG, vA}, meth@CCCC*/)
    GOTO_invoke(invokeVirtualQuick, false, false);
OP_END

/* File: c/OP_INVOKE_VIRTUAL_QUICK_RANGE.c */
HANDLE_OPCODE(OP_INVOKE_VIRTUAL_QUICK_RANGE/*{vCCCC..v(CCCC+AA-1)}, meth@BBBB*/)
    GOTO_invoke(invokeVirtualQuick, true, false);
OP_END

/* File: c/OP_INVOKE_SUPER_QUICK.c */
HANDLE_OPCODE(OP_INVOKE_SUPER_QUICK /*vB, {vD, vE, vF, vG, vA}, meth@CCCC*/)
    GOTO_invoke(invokeSuperQuick, false, false);
OP_END

/* File: c/OP_INVOKE_SUPER_QUICK_RANGE.c */
HANDLE_OPCODE(OP_INVOKE_SUPER_QUICK_RANGE /*{vCCCC..v(CCCC+AA-1)}, meth@BBBB*/)
    GOTO_invoke(invokeSuperQuick, true, false);
OP_END

/* File: c/OP_IPUT_OBJECT_VOLATILE.c */
HANDLE_IPUT_X(OP_IPUT_OBJECT_VOLATILE,  "-object-volatile", ObjectVolatile, _AS_OBJECT)
OP_END

/* File: c/OP_SGET_OBJECT_VOLATILE.c */
HANDLE_SGET_X(OP_SGET_OBJECT_VOLATILE,  "-object-volatile", ObjectVolatile, _AS_OBJECT)
OP_END

/* File: c/OP_SPUT_OBJECT_VOLATILE.c */
HANDLE_SPUT_X(OP_SPUT_OBJECT_VOLATILE,  "-object-volatile", ObjectVolatile, _AS_OBJECT)
OP_END

/* File: c/OP_DISPATCH_FF.c */
HANDLE_OPCODE(OP_DISPATCH_FF)
    /*
     * Indicates extended opcode.  Use next 8 bits to choose where to branch.
     */
    DISPATCH_EXTENDED(INST_AA(inst));
OP_END

/* File: c/OP_CONST_CLASS_JUMBO.c */
HANDLE_OPCODE(OP_CONST_CLASS_JUMBO /*vBBBB, class@AAAAAAAA*/)
    {
        ClassObject* clazz;

        ref = FETCH(1) | (u4)FETCH(2) << 16;
        vdst = FETCH(3);
        ILOGV("|const-class/jumbo v%d class@0x%08x", vdst, ref);
        clazz = dvmDexGetResolvedClass(methodClassDex, ref);
        if (clazz == NULL) {
            EXPORT_PC();
            clazz = dvmResolveClass(curMethod->clazz, ref, true);
            if (clazz == NULL)
                GOTO_exceptionThrown();
        }
        SET_REGISTER(vdst, (u4) clazz);
    }
    FINISH(4);
OP_END

/* File: c/OP_CHECK_CAST_JUMBO.c */
HANDLE_OPCODE(OP_CHECK_CAST_JUMBO /*vBBBB, class@AAAAAAAA*/)
    {
        ClassObject* clazz;
        Object* obj;

        EXPORT_PC();

        ref = FETCH(1) | (u4)FETCH(2) << 16;     /* class to check against */
        vsrc1 = FETCH(3);
        ILOGV("|check-cast/jumbo v%d,class@0x%08x", vsrc1, ref);

        obj = (Object*)GET_REGISTER(vsrc1);
        if (obj != NULL) {
#if defined(WITH_EXTRA_OBJECT_VALIDATION)
            if (!checkForNull(obj))
                GOTO_exceptionThrown();
#endif
            clazz = dvmDexGetResolvedClass(methodClassDex, ref);
            if (clazz == NULL) {
                clazz = dvmResolveClass(curMethod->clazz, ref, false);
                if (clazz == NULL)
                    GOTO_exceptionThrown();
            }
            if (!dvmInstanceof(obj->clazz, clazz)) {
                dvmThrowClassCastException(obj->clazz, clazz);
                GOTO_exceptionThrown();
            }
        }
    }
    FINISH(4);
OP_END

/* File: c/OP_INSTANCE_OF_JUMBO.c */
HANDLE_OPCODE(OP_INSTANCE_OF_JUMBO /*vBBBB, vCCCC, class@AAAAAAAA*/)
    {
        ClassObject* clazz;
        Object* obj;

        ref = FETCH(1) | (u4)FETCH(2) << 16;     /* class to check against */
        vdst = FETCH(3);
        vsrc1 = FETCH(4);   /* object to check */
        ILOGV("|instance-of/jumbo v%d,v%d,class@0x%08x", vdst, vsrc1, ref);

        obj = (Object*)GET_REGISTER(vsrc1);
        if (obj == NULL) {
            SET_REGISTER(vdst, 0);
        } else {
#if defined(WITH_EXTRA_OBJECT_VALIDATION)
            if (!checkForNullExportPC(obj, fp, pc))
                GOTO_exceptionThrown();
#endif
            clazz = dvmDexGetResolvedClass(methodClassDex, ref);
            if (clazz == NULL) {
                EXPORT_PC();
                clazz = dvmResolveClass(curMethod->clazz, ref, true);
                if (clazz == NULL)
                    GOTO_exceptionThrown();
            }
            SET_REGISTER(vdst, dvmInstanceof(obj->clazz, clazz));
        }
    }
    FINISH(5);
OP_END

/* File: c/OP_NEW_INSTANCE_JUMBO.c */
HANDLE_OPCODE(OP_NEW_INSTANCE_JUMBO /*vBBBB, class@AAAAAAAA*/)
    {
        ClassObject* clazz;
        Object* newObj;

        EXPORT_PC();

        ref = FETCH(1) | (u4)FETCH(2) << 16;
        vdst = FETCH(3);
        ILOGV("|new-instance/jumbo v%d,class@0x%08x", vdst, ref);
        clazz = dvmDexGetResolvedClass(methodClassDex, ref);
        if (clazz == NULL) {
            clazz = dvmResolveClass(curMethod->clazz, ref, false);
            if (clazz == NULL)
                GOTO_exceptionThrown();
        }

        if (!dvmIsClassInitialized(clazz) && !dvmInitClass(clazz))
            GOTO_exceptionThrown();

        /*
         * The JIT needs dvmDexGetResolvedClass() to return non-null.
         * Since we use the portable interpreter to build the trace, this extra
         * check is not needed for mterp.
         */
        if (!dvmDexGetResolvedClass(methodClassDex, ref)) {
            /* Class initialization is still ongoing - end the trace */
            END_JIT_TSELECT();
        }

        /*
         * Verifier now tests for interface/abstract class.
         */
        //if (dvmIsInterfaceClass(clazz) || dvmIsAbstractClass(clazz)) {
        //    dvmThrowExceptionWithClassMessage("Ljava/lang/InstantiationError;",
        //        clazz->descriptor);
        //    GOTO_exceptionThrown();
        //}
        newObj = dvmAllocObject(clazz, ALLOC_DONT_TRACK);
        if (newObj == NULL)
            GOTO_exceptionThrown();
        SET_REGISTER(vdst, (u4) newObj);
    }
    FINISH(4);
OP_END

/* File: c/OP_NEW_ARRAY_JUMBO.c */
HANDLE_OPCODE(OP_NEW_ARRAY_JUMBO /*vBBBB, vCCCC, class@AAAAAAAA*/)
    {
        ClassObject* arrayClass;
        ArrayObject* newArray;
        s4 length;

        EXPORT_PC();

        ref = FETCH(1) | (u4)FETCH(2) << 16;
        vdst = FETCH(3);
        vsrc1 = FETCH(4);       /* length reg */
        ILOGV("|new-array/jumbo v%d,v%d,class@0x%08x  (%d elements)",
            vdst, vsrc1, ref, (s4) GET_REGISTER(vsrc1));
        length = (s4) GET_REGISTER(vsrc1);
        if (length < 0) {
            dvmThrowException("Ljava/lang/NegativeArraySizeException;", NULL);
            GOTO_exceptionThrown();
        }
        arrayClass = dvmDexGetResolvedClass(methodClassDex, ref);
        if (arrayClass == NULL) {
            arrayClass = dvmResolveClass(curMethod->clazz, ref, false);
            if (arrayClass == NULL)
                GOTO_exceptionThrown();
        }
        /* verifier guarantees this is an array class */
        assert(dvmIsArrayClass(arrayClass));
        assert(dvmIsClassInitialized(arrayClass));

        newArray = dvmAllocArrayByClass(arrayClass, length, ALLOC_DONT_TRACK);
        if (newArray == NULL)
            GOTO_exceptionThrown();
        SET_REGISTER(vdst, (u4) newArray);
    }
    FINISH(5);
OP_END

/* File: c/OP_FILLED_NEW_ARRAY_JUMBO.c */
HANDLE_OPCODE(OP_FILLED_NEW_ARRAY_JUMBO /*{vCCCC..v(CCCC+BBBB-1)}, class@AAAAAAAA*/)
    GOTO_invoke(filledNewArray, true, true);
OP_END

/* File: c/OP_IGET_JUMBO.c */
HANDLE_IGET_X_JUMBO(OP_IGET_JUMBO,          "", Int, )
OP_END

/* File: c/OP_IGET_WIDE_JUMBO.c */
HANDLE_IGET_X_JUMBO(OP_IGET_WIDE_JUMBO,     "-wide", Long, _WIDE)
OP_END

/* File: c/OP_IGET_OBJECT_JUMBO.c */
HANDLE_IGET_X_JUMBO(OP_IGET_OBJECT_JUMBO,   "-object", Object, _AS_OBJECT)
OP_END

/* File: c/OP_IGET_BOOLEAN_JUMBO.c */
HANDLE_IGET_X_JUMBO(OP_IGET_BOOLEAN_JUMBO,  "", Int, )
OP_END

/* File: c/OP_IGET_BYTE_JUMBO.c */
HANDLE_IGET_X_JUMBO(OP_IGET_BYTE_JUMBO,     "", Int, )
OP_END

/* File: c/OP_IGET_CHAR_JUMBO.c */
HANDLE_IGET_X_JUMBO(OP_IGET_CHAR_JUMBO,     "", Int, )
OP_END

/* File: c/OP_IGET_SHORT_JUMBO.c */
HANDLE_IGET_X_JUMBO(OP_IGET_SHORT_JUMBO,    "", Int, )
OP_END

/* File: c/OP_IPUT_JUMBO.c */
HANDLE_IPUT_X_JUMBO(OP_IPUT_JUMBO,          "", Int, )
OP_END

/* File: c/OP_IPUT_WIDE_JUMBO.c */
HANDLE_IPUT_X_JUMBO(OP_IPUT_WIDE_JUMBO,     "-wide", Long, _WIDE)
OP_END

/* File: c/OP_IPUT_OBJECT_JUMBO.c */
/*
 * The VM spec says we should verify that the reference being stored into
 * the field is assignment compatible.  In practice, many popular VMs don't
 * do this because it slows down a very common operation.  It's not so bad
 * for us, since "dexopt" quickens it whenever possible, but it's still an
 * issue.
 *
 * To make this spec-complaint, we'd need to add a ClassObject pointer to
 * the Field struct, resolve the field's type descriptor at link or class
 * init time, and then verify the type here.
 */
HANDLE_IPUT_X_JUMBO(OP_IPUT_OBJECT_JUMBO,   "-object", Object, _AS_OBJECT)
OP_END

/* File: c/OP_IPUT_BOOLEAN_JUMBO.c */
HANDLE_IPUT_X_JUMBO(OP_IPUT_BOOLEAN_JUMBO,  "", Int, )
OP_END

/* File: c/OP_IPUT_BYTE_JUMBO.c */
HANDLE_IPUT_X_JUMBO(OP_IPUT_BYTE_JUMBO,     "", Int, )
OP_END

/* File: c/OP_IPUT_CHAR_JUMBO.c */
HANDLE_IPUT_X_JUMBO(OP_IPUT_CHAR_JUMBO,     "", Int, )
OP_END

/* File: c/OP_IPUT_SHORT_JUMBO.c */
HANDLE_IPUT_X_JUMBO(OP_IPUT_SHORT_JUMBO,    "", Int, )
OP_END

/* File: c/OP_SGET_JUMBO.c */
HANDLE_SGET_X_JUMBO(OP_SGET_JUMBO,          "", Int, )
OP_END

/* File: c/OP_SGET_WIDE_JUMBO.c */
HANDLE_SGET_X_JUMBO(OP_SGET_WIDE_JUMBO,     "-wide", Long, _WIDE)
OP_END

/* File: c/OP_SGET_OBJECT_JUMBO.c */
HANDLE_SGET_X_JUMBO(OP_SGET_OBJECT_JUMBO,   "-object", Object, _AS_OBJECT)
OP_END

/* File: c/OP_SGET_BOOLEAN_JUMBO.c */
HANDLE_SGET_X_JUMBO(OP_SGET_BOOLEAN_JUMBO,  "", Int, )
OP_END

/* File: c/OP_SGET_BYTE_JUMBO.c */
HANDLE_SGET_X_JUMBO(OP_SGET_BYTE_JUMBO,     "", Int, )
OP_END

/* File: c/OP_SGET_CHAR_JUMBO.c */
HANDLE_SGET_X_JUMBO(OP_SGET_CHAR_JUMBO,     "", Int, )
OP_END

/* File: c/OP_SGET_SHORT_JUMBO.c */
HANDLE_SGET_X_JUMBO(OP_SGET_SHORT_JUMBO,    "", Int, )
OP_END

/* File: c/OP_SPUT_JUMBO.c */
HANDLE_SPUT_X_JUMBO(OP_SPUT_JUMBO,          "", Int, )
OP_END

/* File: c/OP_SPUT_WIDE_JUMBO.c */
HANDLE_SPUT_X_JUMBO(OP_SPUT_WIDE_JUMBO,     "-wide", Long, _WIDE)
OP_END

/* File: c/OP_SPUT_OBJECT_JUMBO.c */
HANDLE_SPUT_X_JUMBO(OP_SPUT_OBJECT_JUMBO,   "-object", Object, _AS_OBJECT)
OP_END

/* File: c/OP_SPUT_BOOLEAN_JUMBO.c */
HANDLE_SPUT_X_JUMBO(OP_SPUT_BOOLEAN_JUMBO,          "", Int, )
OP_END

/* File: c/OP_SPUT_BYTE_JUMBO.c */
HANDLE_SPUT_X_JUMBO(OP_SPUT_BYTE_JUMBO,     "", Int, )
OP_END

/* File: c/OP_SPUT_CHAR_JUMBO.c */
HANDLE_SPUT_X_JUMBO(OP_SPUT_CHAR_JUMBO,     "", Int, )
OP_END

/* File: c/OP_SPUT_SHORT_JUMBO.c */
HANDLE_SPUT_X_JUMBO(OP_SPUT_SHORT_JUMBO,    "", Int, )
OP_END

/* File: c/OP_INVOKE_VIRTUAL_JUMBO.c */
HANDLE_OPCODE(OP_INVOKE_VIRTUAL_JUMBO /*{vCCCC..v(CCCC+BBBB-1)}, meth@AAAAAAAA*/)
    GOTO_invoke(invokeVirtual, true, true);
OP_END

/* File: c/OP_INVOKE_SUPER_JUMBO.c */
HANDLE_OPCODE(OP_INVOKE_SUPER_JUMBO /*{vCCCC..v(CCCC+BBBB-1)}, meth@AAAAAAAA*/)
    GOTO_invoke(invokeSuper, true, true);
OP_END

/* File: c/OP_INVOKE_DIRECT_JUMBO.c */
HANDLE_OPCODE(OP_INVOKE_DIRECT_JUMBO /*{vCCCC..v(CCCC+BBBB-1)}, meth@AAAAAAAA*/)
    GOTO_invoke(invokeDirect, true, true);
OP_END

/* File: c/OP_INVOKE_STATIC_JUMBO.c */
HANDLE_OPCODE(OP_INVOKE_STATIC_JUMBO /*{vCCCC..v(CCCC+BBBB-1)}, meth@AAAAAAAA*/)
    GOTO_invoke(invokeStatic, true, true);
OP_END

/* File: c/OP_INVOKE_INTERFACE_JUMBO.c */
HANDLE_OPCODE(OP_INVOKE_INTERFACE_JUMBO /*{vCCCC..v(CCCC+BBBB-1)}, meth@AAAAAAAA*/)
    GOTO_invoke(invokeInterface, true, true);
OP_END

/* File: c/OP_UNUSED_27FF.c */
HANDLE_OPCODE(OP_UNUSED_27FF)
OP_END

/* File: c/OP_UNUSED_28FF.c */
HANDLE_OPCODE(OP_UNUSED_28FF)
OP_END

/* File: c/OP_UNUSED_29FF.c */
HANDLE_OPCODE(OP_UNUSED_29FF)
OP_END

/* File: c/OP_UNUSED_2AFF.c */
HANDLE_OPCODE(OP_UNUSED_2AFF)
OP_END

/* File: c/OP_UNUSED_2BFF.c */
HANDLE_OPCODE(OP_UNUSED_2BFF)
OP_END

/* File: c/OP_UNUSED_2CFF.c */
HANDLE_OPCODE(OP_UNUSED_2CFF)
OP_END

/* File: c/OP_UNUSED_2DFF.c */
HANDLE_OPCODE(OP_UNUSED_2DFF)
OP_END

/* File: c/OP_UNUSED_2EFF.c */
HANDLE_OPCODE(OP_UNUSED_2EFF)
OP_END

/* File: c/OP_UNUSED_2FFF.c */
HANDLE_OPCODE(OP_UNUSED_2FFF)
OP_END

/* File: c/OP_UNUSED_30FF.c */
HANDLE_OPCODE(OP_UNUSED_30FF)
OP_END

/* File: c/OP_UNUSED_31FF.c */
HANDLE_OPCODE(OP_UNUSED_31FF)
OP_END

/* File: c/OP_UNUSED_32FF.c */
HANDLE_OPCODE(OP_UNUSED_32FF)
OP_END

/* File: c/OP_UNUSED_33FF.c */
HANDLE_OPCODE(OP_UNUSED_33FF)
OP_END

/* File: c/OP_UNUSED_34FF.c */
HANDLE_OPCODE(OP_UNUSED_34FF)
OP_END

/* File: c/OP_UNUSED_35FF.c */
HANDLE_OPCODE(OP_UNUSED_35FF)
OP_END

/* File: c/OP_UNUSED_36FF.c */
HANDLE_OPCODE(OP_UNUSED_36FF)
OP_END

/* File: c/OP_UNUSED_37FF.c */
HANDLE_OPCODE(OP_UNUSED_37FF)
OP_END

/* File: c/OP_UNUSED_38FF.c */
HANDLE_OPCODE(OP_UNUSED_38FF)
OP_END

/* File: c/OP_UNUSED_39FF.c */
HANDLE_OPCODE(OP_UNUSED_39FF)
OP_END

/* File: c/OP_UNUSED_3AFF.c */
HANDLE_OPCODE(OP_UNUSED_3AFF)
OP_END

/* File: c/OP_UNUSED_3BFF.c */
HANDLE_OPCODE(OP_UNUSED_3BFF)
OP_END

/* File: c/OP_UNUSED_3CFF.c */
HANDLE_OPCODE(OP_UNUSED_3CFF)
OP_END

/* File: c/OP_UNUSED_3DFF.c */
HANDLE_OPCODE(OP_UNUSED_3DFF)
OP_END

/* File: c/OP_UNUSED_3EFF.c */
HANDLE_OPCODE(OP_UNUSED_3EFF)
OP_END

/* File: c/OP_UNUSED_3FFF.c */
HANDLE_OPCODE(OP_UNUSED_3FFF)
OP_END

/* File: c/OP_UNUSED_40FF.c */
HANDLE_OPCODE(OP_UNUSED_40FF)
OP_END

/* File: c/OP_UNUSED_41FF.c */
HANDLE_OPCODE(OP_UNUSED_41FF)
OP_END

/* File: c/OP_UNUSED_42FF.c */
HANDLE_OPCODE(OP_UNUSED_42FF)
OP_END

/* File: c/OP_UNUSED_43FF.c */
HANDLE_OPCODE(OP_UNUSED_43FF)
OP_END

/* File: c/OP_UNUSED_44FF.c */
HANDLE_OPCODE(OP_UNUSED_44FF)
OP_END

/* File: c/OP_UNUSED_45FF.c */
HANDLE_OPCODE(OP_UNUSED_45FF)
OP_END

/* File: c/OP_UNUSED_46FF.c */
HANDLE_OPCODE(OP_UNUSED_46FF)
OP_END

/* File: c/OP_UNUSED_47FF.c */
HANDLE_OPCODE(OP_UNUSED_47FF)
OP_END

/* File: c/OP_UNUSED_48FF.c */
HANDLE_OPCODE(OP_UNUSED_48FF)
OP_END

/* File: c/OP_UNUSED_49FF.c */
HANDLE_OPCODE(OP_UNUSED_49FF)
OP_END

/* File: c/OP_UNUSED_4AFF.c */
HANDLE_OPCODE(OP_UNUSED_4AFF)
OP_END

/* File: c/OP_UNUSED_4BFF.c */
HANDLE_OPCODE(OP_UNUSED_4BFF)
OP_END

/* File: c/OP_UNUSED_4CFF.c */
HANDLE_OPCODE(OP_UNUSED_4CFF)
OP_END

/* File: c/OP_UNUSED_4DFF.c */
HANDLE_OPCODE(OP_UNUSED_4DFF)
OP_END

/* File: c/OP_UNUSED_4EFF.c */
HANDLE_OPCODE(OP_UNUSED_4EFF)
OP_END

/* File: c/OP_UNUSED_4FFF.c */
HANDLE_OPCODE(OP_UNUSED_4FFF)
OP_END

/* File: c/OP_UNUSED_50FF.c */
HANDLE_OPCODE(OP_UNUSED_50FF)
OP_END

/* File: c/OP_UNUSED_51FF.c */
HANDLE_OPCODE(OP_UNUSED_51FF)
OP_END

/* File: c/OP_UNUSED_52FF.c */
HANDLE_OPCODE(OP_UNUSED_52FF)
OP_END

/* File: c/OP_UNUSED_53FF.c */
HANDLE_OPCODE(OP_UNUSED_53FF)
OP_END

/* File: c/OP_UNUSED_54FF.c */
HANDLE_OPCODE(OP_UNUSED_54FF)
OP_END

/* File: c/OP_UNUSED_55FF.c */
HANDLE_OPCODE(OP_UNUSED_55FF)
OP_END

/* File: c/OP_UNUSED_56FF.c */
HANDLE_OPCODE(OP_UNUSED_56FF)
OP_END

/* File: c/OP_UNUSED_57FF.c */
HANDLE_OPCODE(OP_UNUSED_57FF)
OP_END

/* File: c/OP_UNUSED_58FF.c */
HANDLE_OPCODE(OP_UNUSED_58FF)
OP_END

/* File: c/OP_UNUSED_59FF.c */
HANDLE_OPCODE(OP_UNUSED_59FF)
OP_END

/* File: c/OP_UNUSED_5AFF.c */
HANDLE_OPCODE(OP_UNUSED_5AFF)
OP_END

/* File: c/OP_UNUSED_5BFF.c */
HANDLE_OPCODE(OP_UNUSED_5BFF)
OP_END

/* File: c/OP_UNUSED_5CFF.c */
HANDLE_OPCODE(OP_UNUSED_5CFF)
OP_END

/* File: c/OP_UNUSED_5DFF.c */
HANDLE_OPCODE(OP_UNUSED_5DFF)
OP_END

/* File: c/OP_UNUSED_5EFF.c */
HANDLE_OPCODE(OP_UNUSED_5EFF)
OP_END

/* File: c/OP_UNUSED_5FFF.c */
HANDLE_OPCODE(OP_UNUSED_5FFF)
OP_END

/* File: c/OP_UNUSED_60FF.c */
HANDLE_OPCODE(OP_UNUSED_60FF)
OP_END

/* File: c/OP_UNUSED_61FF.c */
HANDLE_OPCODE(OP_UNUSED_61FF)
OP_END

/* File: c/OP_UNUSED_62FF.c */
HANDLE_OPCODE(OP_UNUSED_62FF)
OP_END

/* File: c/OP_UNUSED_63FF.c */
HANDLE_OPCODE(OP_UNUSED_63FF)
OP_END

/* File: c/OP_UNUSED_64FF.c */
HANDLE_OPCODE(OP_UNUSED_64FF)
OP_END

/* File: c/OP_UNUSED_65FF.c */
HANDLE_OPCODE(OP_UNUSED_65FF)
OP_END

/* File: c/OP_UNUSED_66FF.c */
HANDLE_OPCODE(OP_UNUSED_66FF)
OP_END

/* File: c/OP_UNUSED_67FF.c */
HANDLE_OPCODE(OP_UNUSED_67FF)
OP_END

/* File: c/OP_UNUSED_68FF.c */
HANDLE_OPCODE(OP_UNUSED_68FF)
OP_END

/* File: c/OP_UNUSED_69FF.c */
HANDLE_OPCODE(OP_UNUSED_69FF)
OP_END

/* File: c/OP_UNUSED_6AFF.c */
HANDLE_OPCODE(OP_UNUSED_6AFF)
OP_END

/* File: c/OP_UNUSED_6BFF.c */
HANDLE_OPCODE(OP_UNUSED_6BFF)
OP_END

/* File: c/OP_UNUSED_6CFF.c */
HANDLE_OPCODE(OP_UNUSED_6CFF)
OP_END

/* File: c/OP_UNUSED_6DFF.c */
HANDLE_OPCODE(OP_UNUSED_6DFF)
OP_END

/* File: c/OP_UNUSED_6EFF.c */
HANDLE_OPCODE(OP_UNUSED_6EFF)
OP_END

/* File: c/OP_UNUSED_6FFF.c */
HANDLE_OPCODE(OP_UNUSED_6FFF)
OP_END

/* File: c/OP_UNUSED_70FF.c */
HANDLE_OPCODE(OP_UNUSED_70FF)
OP_END

/* File: c/OP_UNUSED_71FF.c */
HANDLE_OPCODE(OP_UNUSED_71FF)
OP_END

/* File: c/OP_UNUSED_72FF.c */
HANDLE_OPCODE(OP_UNUSED_72FF)
OP_END

/* File: c/OP_UNUSED_73FF.c */
HANDLE_OPCODE(OP_UNUSED_73FF)
OP_END

/* File: c/OP_UNUSED_74FF.c */
HANDLE_OPCODE(OP_UNUSED_74FF)
OP_END

/* File: c/OP_UNUSED_75FF.c */
HANDLE_OPCODE(OP_UNUSED_75FF)
OP_END

/* File: c/OP_UNUSED_76FF.c */
HANDLE_OPCODE(OP_UNUSED_76FF)
OP_END

/* File: c/OP_UNUSED_77FF.c */
HANDLE_OPCODE(OP_UNUSED_77FF)
OP_END

/* File: c/OP_UNUSED_78FF.c */
HANDLE_OPCODE(OP_UNUSED_78FF)
OP_END

/* File: c/OP_UNUSED_79FF.c */
HANDLE_OPCODE(OP_UNUSED_79FF)
OP_END

/* File: c/OP_UNUSED_7AFF.c */
HANDLE_OPCODE(OP_UNUSED_7AFF)
OP_END

/* File: c/OP_UNUSED_7BFF.c */
HANDLE_OPCODE(OP_UNUSED_7BFF)
OP_END

/* File: c/OP_UNUSED_7CFF.c */
HANDLE_OPCODE(OP_UNUSED_7CFF)
OP_END

/* File: c/OP_UNUSED_7DFF.c */
HANDLE_OPCODE(OP_UNUSED_7DFF)
OP_END

/* File: c/OP_UNUSED_7EFF.c */
HANDLE_OPCODE(OP_UNUSED_7EFF)
OP_END

/* File: c/OP_UNUSED_7FFF.c */
HANDLE_OPCODE(OP_UNUSED_7FFF)
OP_END

/* File: c/OP_UNUSED_80FF.c */
HANDLE_OPCODE(OP_UNUSED_80FF)
OP_END

/* File: c/OP_UNUSED_81FF.c */
HANDLE_OPCODE(OP_UNUSED_81FF)
OP_END

/* File: c/OP_UNUSED_82FF.c */
HANDLE_OPCODE(OP_UNUSED_82FF)
OP_END

/* File: c/OP_UNUSED_83FF.c */
HANDLE_OPCODE(OP_UNUSED_83FF)
OP_END

/* File: c/OP_UNUSED_84FF.c */
HANDLE_OPCODE(OP_UNUSED_84FF)
OP_END

/* File: c/OP_UNUSED_85FF.c */
HANDLE_OPCODE(OP_UNUSED_85FF)
OP_END

/* File: c/OP_UNUSED_86FF.c */
HANDLE_OPCODE(OP_UNUSED_86FF)
OP_END

/* File: c/OP_UNUSED_87FF.c */
HANDLE_OPCODE(OP_UNUSED_87FF)
OP_END

/* File: c/OP_UNUSED_88FF.c */
HANDLE_OPCODE(OP_UNUSED_88FF)
OP_END

/* File: c/OP_UNUSED_89FF.c */
HANDLE_OPCODE(OP_UNUSED_89FF)
OP_END

/* File: c/OP_UNUSED_8AFF.c */
HANDLE_OPCODE(OP_UNUSED_8AFF)
OP_END

/* File: c/OP_UNUSED_8BFF.c */
HANDLE_OPCODE(OP_UNUSED_8BFF)
OP_END

/* File: c/OP_UNUSED_8CFF.c */
HANDLE_OPCODE(OP_UNUSED_8CFF)
OP_END

/* File: c/OP_UNUSED_8DFF.c */
HANDLE_OPCODE(OP_UNUSED_8DFF)
OP_END

/* File: c/OP_UNUSED_8EFF.c */
HANDLE_OPCODE(OP_UNUSED_8EFF)
OP_END

/* File: c/OP_UNUSED_8FFF.c */
HANDLE_OPCODE(OP_UNUSED_8FFF)
OP_END

/* File: c/OP_UNUSED_90FF.c */
HANDLE_OPCODE(OP_UNUSED_90FF)
OP_END

/* File: c/OP_UNUSED_91FF.c */
HANDLE_OPCODE(OP_UNUSED_91FF)
OP_END

/* File: c/OP_UNUSED_92FF.c */
HANDLE_OPCODE(OP_UNUSED_92FF)
OP_END

/* File: c/OP_UNUSED_93FF.c */
HANDLE_OPCODE(OP_UNUSED_93FF)
OP_END

/* File: c/OP_UNUSED_94FF.c */
HANDLE_OPCODE(OP_UNUSED_94FF)
OP_END

/* File: c/OP_UNUSED_95FF.c */
HANDLE_OPCODE(OP_UNUSED_95FF)
OP_END

/* File: c/OP_UNUSED_96FF.c */
HANDLE_OPCODE(OP_UNUSED_96FF)
OP_END

/* File: c/OP_UNUSED_97FF.c */
HANDLE_OPCODE(OP_UNUSED_97FF)
OP_END

/* File: c/OP_UNUSED_98FF.c */
HANDLE_OPCODE(OP_UNUSED_98FF)
OP_END

/* File: c/OP_UNUSED_99FF.c */
HANDLE_OPCODE(OP_UNUSED_99FF)
OP_END

/* File: c/OP_UNUSED_9AFF.c */
HANDLE_OPCODE(OP_UNUSED_9AFF)
OP_END

/* File: c/OP_UNUSED_9BFF.c */
HANDLE_OPCODE(OP_UNUSED_9BFF)
OP_END

/* File: c/OP_UNUSED_9CFF.c */
HANDLE_OPCODE(OP_UNUSED_9CFF)
OP_END

/* File: c/OP_UNUSED_9DFF.c */
HANDLE_OPCODE(OP_UNUSED_9DFF)
OP_END

/* File: c/OP_UNUSED_9EFF.c */
HANDLE_OPCODE(OP_UNUSED_9EFF)
OP_END

/* File: c/OP_UNUSED_9FFF.c */
HANDLE_OPCODE(OP_UNUSED_9FFF)
OP_END

/* File: c/OP_UNUSED_A0FF.c */
HANDLE_OPCODE(OP_UNUSED_A0FF)
OP_END

/* File: c/OP_UNUSED_A1FF.c */
HANDLE_OPCODE(OP_UNUSED_A1FF)
OP_END

/* File: c/OP_UNUSED_A2FF.c */
HANDLE_OPCODE(OP_UNUSED_A2FF)
OP_END

/* File: c/OP_UNUSED_A3FF.c */
HANDLE_OPCODE(OP_UNUSED_A3FF)
OP_END

/* File: c/OP_UNUSED_A4FF.c */
HANDLE_OPCODE(OP_UNUSED_A4FF)
OP_END

/* File: c/OP_UNUSED_A5FF.c */
HANDLE_OPCODE(OP_UNUSED_A5FF)
OP_END

/* File: c/OP_UNUSED_A6FF.c */
HANDLE_OPCODE(OP_UNUSED_A6FF)
OP_END

/* File: c/OP_UNUSED_A7FF.c */
HANDLE_OPCODE(OP_UNUSED_A7FF)
OP_END

/* File: c/OP_UNUSED_A8FF.c */
HANDLE_OPCODE(OP_UNUSED_A8FF)
OP_END

/* File: c/OP_UNUSED_A9FF.c */
HANDLE_OPCODE(OP_UNUSED_A9FF)
OP_END

/* File: c/OP_UNUSED_AAFF.c */
HANDLE_OPCODE(OP_UNUSED_AAFF)
OP_END

/* File: c/OP_UNUSED_ABFF.c */
HANDLE_OPCODE(OP_UNUSED_ABFF)
OP_END

/* File: c/OP_UNUSED_ACFF.c */
HANDLE_OPCODE(OP_UNUSED_ACFF)
OP_END

/* File: c/OP_UNUSED_ADFF.c */
HANDLE_OPCODE(OP_UNUSED_ADFF)
OP_END

/* File: c/OP_UNUSED_AEFF.c */
HANDLE_OPCODE(OP_UNUSED_AEFF)
OP_END

/* File: c/OP_UNUSED_AFFF.c */
HANDLE_OPCODE(OP_UNUSED_AFFF)
OP_END

/* File: c/OP_UNUSED_B0FF.c */
HANDLE_OPCODE(OP_UNUSED_B0FF)
OP_END

/* File: c/OP_UNUSED_B1FF.c */
HANDLE_OPCODE(OP_UNUSED_B1FF)
OP_END

/* File: c/OP_UNUSED_B2FF.c */
HANDLE_OPCODE(OP_UNUSED_B2FF)
OP_END

/* File: c/OP_UNUSED_B3FF.c */
HANDLE_OPCODE(OP_UNUSED_B3FF)
OP_END

/* File: c/OP_UNUSED_B4FF.c */
HANDLE_OPCODE(OP_UNUSED_B4FF)
OP_END

/* File: c/OP_UNUSED_B5FF.c */
HANDLE_OPCODE(OP_UNUSED_B5FF)
OP_END

/* File: c/OP_UNUSED_B6FF.c */
HANDLE_OPCODE(OP_UNUSED_B6FF)
OP_END

/* File: c/OP_UNUSED_B7FF.c */
HANDLE_OPCODE(OP_UNUSED_B7FF)
OP_END

/* File: c/OP_UNUSED_B8FF.c */
HANDLE_OPCODE(OP_UNUSED_B8FF)
OP_END

/* File: c/OP_UNUSED_B9FF.c */
HANDLE_OPCODE(OP_UNUSED_B9FF)
OP_END

/* File: c/OP_UNUSED_BAFF.c */
HANDLE_OPCODE(OP_UNUSED_BAFF)
OP_END

/* File: c/OP_UNUSED_BBFF.c */
HANDLE_OPCODE(OP_UNUSED_BBFF)
OP_END

/* File: c/OP_UNUSED_BCFF.c */
HANDLE_OPCODE(OP_UNUSED_BCFF)
OP_END

/* File: c/OP_UNUSED_BDFF.c */
HANDLE_OPCODE(OP_UNUSED_BDFF)
OP_END

/* File: c/OP_UNUSED_BEFF.c */
HANDLE_OPCODE(OP_UNUSED_BEFF)
OP_END

/* File: c/OP_UNUSED_BFFF.c */
HANDLE_OPCODE(OP_UNUSED_BFFF)
OP_END

/* File: c/OP_UNUSED_C0FF.c */
HANDLE_OPCODE(OP_UNUSED_C0FF)
OP_END

/* File: c/OP_UNUSED_C1FF.c */
HANDLE_OPCODE(OP_UNUSED_C1FF)
OP_END

/* File: c/OP_UNUSED_C2FF.c */
HANDLE_OPCODE(OP_UNUSED_C2FF)
OP_END

/* File: c/OP_UNUSED_C3FF.c */
HANDLE_OPCODE(OP_UNUSED_C3FF)
OP_END

/* File: c/OP_UNUSED_C4FF.c */
HANDLE_OPCODE(OP_UNUSED_C4FF)
OP_END

/* File: c/OP_UNUSED_C5FF.c */
HANDLE_OPCODE(OP_UNUSED_C5FF)
OP_END

/* File: c/OP_UNUSED_C6FF.c */
HANDLE_OPCODE(OP_UNUSED_C6FF)
OP_END

/* File: c/OP_UNUSED_C7FF.c */
HANDLE_OPCODE(OP_UNUSED_C7FF)
OP_END

/* File: c/OP_UNUSED_C8FF.c */
HANDLE_OPCODE(OP_UNUSED_C8FF)
OP_END

/* File: c/OP_UNUSED_C9FF.c */
HANDLE_OPCODE(OP_UNUSED_C9FF)
OP_END

/* File: c/OP_UNUSED_CAFF.c */
HANDLE_OPCODE(OP_UNUSED_CAFF)
OP_END

/* File: c/OP_UNUSED_CBFF.c */
HANDLE_OPCODE(OP_UNUSED_CBFF)
OP_END

/* File: c/OP_UNUSED_CCFF.c */
HANDLE_OPCODE(OP_UNUSED_CCFF)
OP_END

/* File: c/OP_UNUSED_CDFF.c */
HANDLE_OPCODE(OP_UNUSED_CDFF)
OP_END

/* File: c/OP_UNUSED_CEFF.c */
HANDLE_OPCODE(OP_UNUSED_CEFF)
OP_END

/* File: c/OP_UNUSED_CFFF.c */
HANDLE_OPCODE(OP_UNUSED_CFFF)
OP_END

/* File: c/OP_UNUSED_D0FF.c */
HANDLE_OPCODE(OP_UNUSED_D0FF)
OP_END

/* File: c/OP_UNUSED_D1FF.c */
HANDLE_OPCODE(OP_UNUSED_D1FF)
OP_END

/* File: c/OP_UNUSED_D2FF.c */
HANDLE_OPCODE(OP_UNUSED_D2FF)
OP_END

/* File: c/OP_UNUSED_D3FF.c */
HANDLE_OPCODE(OP_UNUSED_D3FF)
OP_END

/* File: c/OP_UNUSED_D4FF.c */
HANDLE_OPCODE(OP_UNUSED_D4FF)
OP_END

/* File: c/OP_UNUSED_D5FF.c */
HANDLE_OPCODE(OP_UNUSED_D5FF)
OP_END

/* File: c/OP_UNUSED_D6FF.c */
HANDLE_OPCODE(OP_UNUSED_D6FF)
OP_END

/* File: c/OP_UNUSED_D7FF.c */
HANDLE_OPCODE(OP_UNUSED_D7FF)
OP_END

/* File: c/OP_UNUSED_D8FF.c */
HANDLE_OPCODE(OP_UNUSED_D8FF)
OP_END

/* File: c/OP_UNUSED_D9FF.c */
HANDLE_OPCODE(OP_UNUSED_D9FF)
OP_END

/* File: c/OP_UNUSED_DAFF.c */
HANDLE_OPCODE(OP_UNUSED_DAFF)
OP_END

/* File: c/OP_UNUSED_DBFF.c */
HANDLE_OPCODE(OP_UNUSED_DBFF)
OP_END

/* File: c/OP_UNUSED_DCFF.c */
HANDLE_OPCODE(OP_UNUSED_DCFF)
OP_END

/* File: c/OP_UNUSED_DDFF.c */
HANDLE_OPCODE(OP_UNUSED_DDFF)
OP_END

/* File: c/OP_UNUSED_DEFF.c */
HANDLE_OPCODE(OP_UNUSED_DEFF)
OP_END

/* File: c/OP_UNUSED_DFFF.c */
HANDLE_OPCODE(OP_UNUSED_DFFF)
OP_END

/* File: c/OP_UNUSED_E0FF.c */
HANDLE_OPCODE(OP_UNUSED_E0FF)
OP_END

/* File: c/OP_UNUSED_E1FF.c */
HANDLE_OPCODE(OP_UNUSED_E1FF)
OP_END

/* File: c/OP_UNUSED_E2FF.c */
HANDLE_OPCODE(OP_UNUSED_E2FF)
OP_END

/* File: c/OP_UNUSED_E3FF.c */
HANDLE_OPCODE(OP_UNUSED_E3FF)
OP_END

/* File: c/OP_UNUSED_E4FF.c */
HANDLE_OPCODE(OP_UNUSED_E4FF)
OP_END

/* File: c/OP_UNUSED_E5FF.c */
HANDLE_OPCODE(OP_UNUSED_E5FF)
OP_END

/* File: c/OP_UNUSED_E6FF.c */
HANDLE_OPCODE(OP_UNUSED_E6FF)
OP_END

/* File: c/OP_UNUSED_E7FF.c */
HANDLE_OPCODE(OP_UNUSED_E7FF)
OP_END

/* File: c/OP_UNUSED_E8FF.c */
HANDLE_OPCODE(OP_UNUSED_E8FF)
OP_END

/* File: c/OP_UNUSED_E9FF.c */
HANDLE_OPCODE(OP_UNUSED_E9FF)
OP_END

/* File: c/OP_UNUSED_EAFF.c */
HANDLE_OPCODE(OP_UNUSED_EAFF)
OP_END

/* File: c/OP_UNUSED_EBFF.c */
HANDLE_OPCODE(OP_UNUSED_EBFF)
OP_END

/* File: c/OP_UNUSED_ECFF.c */
HANDLE_OPCODE(OP_UNUSED_ECFF)
OP_END

/* File: c/OP_UNUSED_EDFF.c */
HANDLE_OPCODE(OP_UNUSED_EDFF)
OP_END

/* File: c/OP_UNUSED_EEFF.c */
HANDLE_OPCODE(OP_UNUSED_EEFF)
OP_END

/* File: c/OP_UNUSED_EFFF.c */
HANDLE_OPCODE(OP_UNUSED_EFFF)
OP_END

/* File: c/OP_UNUSED_F0FF.c */
HANDLE_OPCODE(OP_UNUSED_F0FF)
OP_END

/* File: c/OP_UNUSED_F1FF.c */
HANDLE_OPCODE(OP_UNUSED_F1FF)
OP_END

/* File: c/OP_UNUSED_F2FF.c */
HANDLE_OPCODE(OP_UNUSED_F2FF)
OP_END

/* File: c/OP_UNUSED_F3FF.c */
HANDLE_OPCODE(OP_UNUSED_F3FF)
OP_END

/* File: c/OP_UNUSED_F4FF.c */
HANDLE_OPCODE(OP_UNUSED_F4FF)
OP_END

/* File: c/OP_UNUSED_F5FF.c */
HANDLE_OPCODE(OP_UNUSED_F5FF)
OP_END

/* File: c/OP_UNUSED_F6FF.c */
HANDLE_OPCODE(OP_UNUSED_F6FF)
OP_END

/* File: c/OP_UNUSED_F7FF.c */
HANDLE_OPCODE(OP_UNUSED_F7FF)
OP_END

/* File: c/OP_UNUSED_F8FF.c */
HANDLE_OPCODE(OP_UNUSED_F8FF)
OP_END

/* File: c/OP_UNUSED_F9FF.c */
HANDLE_OPCODE(OP_UNUSED_F9FF)
OP_END

/* File: c/OP_UNUSED_FAFF.c */
HANDLE_OPCODE(OP_UNUSED_FAFF)
OP_END

/* File: c/OP_UNUSED_FBFF.c */
HANDLE_OPCODE(OP_UNUSED_FBFF)
OP_END

/* File: c/OP_UNUSED_FCFF.c */
HANDLE_OPCODE(OP_UNUSED_FCFF)
OP_END

/* File: c/OP_UNUSED_FDFF.c */
HANDLE_OPCODE(OP_UNUSED_FDFF)
OP_END

/* File: c/OP_UNUSED_FEFF.c */
HANDLE_OPCODE(OP_UNUSED_FEFF)
  /*
   * In portable interp, most unused opcodes will fall through to here.
   */
  LOGE("unknown opcode 0x%04x\n", INST_INST(inst));
  dvmAbort();
  FINISH(1);
OP_END

/* File: c/OP_THROW_VERIFICATION_ERROR_JUMBO.c */
HANDLE_OPCODE(OP_THROW_VERIFICATION_ERROR_JUMBO)
    EXPORT_PC();
    vsrc1 = FETCH(1);
    ref = FETCH(2) | (u4)FETCH(3) << 16;      /* class/field/method ref */
    dvmThrowVerificationError(curMethod, vsrc1, ref);
    GOTO_exceptionThrown();
OP_END

/* File: c/gotoTargets.c */
/*
 * C footer.  This has some common code shared by the various targets.
 */

/*
 * Everything from here on is a "goto target".  In the basic interpreter
 * we jump into these targets and then jump directly to the handler for
 * next instruction.  Here, these are subroutines that return to the caller.
 */

GOTO_TARGET(filledNewArray, bool methodCallRange, bool jumboFormat)
    {
        ClassObject* arrayClass;
        ArrayObject* newArray;
        u4* contents;
        char typeCh;
        int i;
        u4 arg5;

        EXPORT_PC();

        if (jumboFormat) {
            ref = FETCH(1) | (u4)FETCH(2) << 16;  /* class ref */
            vsrc1 = FETCH(3);                     /* #of elements */
            vdst = FETCH(4);                      /* range base */
            arg5 = -1;                            /* silence compiler warning */
            ILOGV("|filled-new-array/jumbo args=%d @0x%08x {regs=v%d-v%d}",
                vsrc1, ref, vdst, vdst+vsrc1-1);
        } else {
            ref = FETCH(1);             /* class ref */
            vdst = FETCH(2);            /* first 4 regs -or- range base */

            if (methodCallRange) {
                vsrc1 = INST_AA(inst);  /* #of elements */
                arg5 = -1;              /* silence compiler warning */
                ILOGV("|filled-new-array-range args=%d @0x%04x {regs=v%d-v%d}",
                    vsrc1, ref, vdst, vdst+vsrc1-1);
            } else {
                arg5 = INST_A(inst);
                vsrc1 = INST_B(inst);   /* #of elements */
                ILOGV("|filled-new-array args=%d @0x%04x {regs=0x%04x %x}",
                   vsrc1, ref, vdst, arg5);
            }
        }

        /*
         * Resolve the array class.
         */
        arrayClass = dvmDexGetResolvedClass(methodClassDex, ref);
        if (arrayClass == NULL) {
            arrayClass = dvmResolveClass(curMethod->clazz, ref, false);
            if (arrayClass == NULL)
                GOTO_exceptionThrown();
        }
        /*
        if (!dvmIsArrayClass(arrayClass)) {
            dvmThrowException("Ljava/lang/RuntimeError;",
                "filled-new-array needs array class");
            GOTO_exceptionThrown();
        }
        */
        /* verifier guarantees this is an array class */
        assert(dvmIsArrayClass(arrayClass));
        assert(dvmIsClassInitialized(arrayClass));

        /*
         * Create an array of the specified type.
         */
        LOGVV("+++ filled-new-array type is '%s'\n", arrayClass->descriptor);
        typeCh = arrayClass->descriptor[1];
        if (typeCh == 'D' || typeCh == 'J') {
            /* category 2 primitives not allowed */
            dvmThrowException("Ljava/lang/RuntimeError;",
                "bad filled array req");
            GOTO_exceptionThrown();
        } else if (typeCh != 'L' && typeCh != '[' && typeCh != 'I') {
            /* TODO: requires multiple "fill in" loops with different widths */
            LOGE("non-int primitives not implemented\n");
            dvmThrowException("Ljava/lang/InternalError;",
                "filled-new-array not implemented for anything but 'int'");
            GOTO_exceptionThrown();
        }

        newArray = dvmAllocArrayByClass(arrayClass, vsrc1, ALLOC_DONT_TRACK);
        if (newArray == NULL)
            GOTO_exceptionThrown();

        /*
         * Fill in the elements.  It's legal for vsrc1 to be zero.
         */
        contents = (u4*) newArray->contents;
        if (methodCallRange) {
            for (i = 0; i < vsrc1; i++)
                contents[i] = GET_REGISTER(vdst+i);
        } else {
            assert(vsrc1 <= 5);
            if (vsrc1 == 5) {
                contents[4] = GET_REGISTER(arg5);
                vsrc1--;
            }
            for (i = 0; i < vsrc1; i++) {
                contents[i] = GET_REGISTER(vdst & 0x0f);
                vdst >>= 4;
            }
        }
        if (typeCh == 'L' || typeCh == '[') {
            dvmWriteBarrierArray(newArray, 0, newArray->length);
        }

        retval.l = newArray;
    }
    if (jumboFormat) {
        FINISH(5);
    } else {
        FINISH(3);
    }
GOTO_TARGET_END


GOTO_TARGET(invokeVirtual, bool methodCallRange, bool jumboFormat)
    {
        Method* baseMethod;
        Object* thisPtr;

        EXPORT_PC();

        if (jumboFormat) {
            ref = FETCH(1) | (u4)FETCH(2) << 16;  /* method ref */
            vsrc1 = FETCH(3);                     /* count */
            vdst = FETCH(4);                      /* first reg */
            ADJUST_PC(2);     /* advance pc partially to make returns easier */
            ILOGV("|invoke-virtual/jumbo args=%d @0x%08x {regs=v%d-v%d}",
                vsrc1, ref, vdst, vdst+vsrc1-1);
            thisPtr = (Object*) GET_REGISTER(vdst);
        } else {
            vsrc1 = INST_AA(inst);      /* AA (count) or BA (count + arg 5) */
            ref = FETCH(1);             /* method ref */
            vdst = FETCH(2);            /* 4 regs -or- first reg */

            /*
             * The object against which we are executing a method is always
             * in the first argument.
             */
            if (methodCallRange) {
                assert(vsrc1 > 0);
                ILOGV("|invoke-virtual-range args=%d @0x%04x {regs=v%d-v%d}",
                    vsrc1, ref, vdst, vdst+vsrc1-1);
                thisPtr = (Object*) GET_REGISTER(vdst);
            } else {
                assert((vsrc1>>4) > 0);
                ILOGV("|invoke-virtual args=%d @0x%04x {regs=0x%04x %x}",
                    vsrc1 >> 4, ref, vdst, vsrc1 & 0x0f);
                thisPtr = (Object*) GET_REGISTER(vdst & 0x0f);
            }
        }

        if (!checkForNull(thisPtr))
            GOTO_exceptionThrown();

        /*
         * Resolve the method.  This is the correct method for the static
         * type of the object.  We also verify access permissions here.
         */
        baseMethod = dvmDexGetResolvedMethod(methodClassDex, ref);
        if (baseMethod == NULL) {
            baseMethod = dvmResolveMethod(curMethod->clazz, ref,METHOD_VIRTUAL);
            if (baseMethod == NULL) {
                ILOGV("+ unknown method or access denied\n");
                GOTO_exceptionThrown();
            }
        }

        /*
         * Combine the object we found with the vtable offset in the
         * method.
         */
        assert(baseMethod->methodIndex < thisPtr->clazz->vtableCount);
        methodToCall = thisPtr->clazz->vtable[baseMethod->methodIndex];

#if defined(WITH_JIT) && (INTERP_TYPE == INTERP_DBG)
        callsiteClass = thisPtr->clazz;
#endif

#if 0
        if (dvmIsAbstractMethod(methodToCall)) {
            /*
             * This can happen if you create two classes, Base and Sub, where
             * Sub is a sub-class of Base.  Declare a protected abstract
             * method foo() in Base, and invoke foo() from a method in Base.
             * Base is an "abstract base class" and is never instantiated
             * directly.  Now, Override foo() in Sub, and use Sub.  This
             * Works fine unless Sub stops providing an implementation of
             * the method.
             */
            dvmThrowException("Ljava/lang/AbstractMethodError;",
                "abstract method not implemented");
            GOTO_exceptionThrown();
        }
#else
        assert(!dvmIsAbstractMethod(methodToCall) ||
            methodToCall->nativeFunc != NULL);
#endif

        LOGVV("+++ base=%s.%s virtual[%d]=%s.%s\n",
            baseMethod->clazz->descriptor, baseMethod->name,
            (u4) baseMethod->methodIndex,
            methodToCall->clazz->descriptor, methodToCall->name);
        assert(methodToCall != NULL);

#if 0
        if (vsrc1 != methodToCall->insSize) {
            LOGW("WRONG METHOD: base=%s.%s virtual[%d]=%s.%s\n",
                baseMethod->clazz->descriptor, baseMethod->name,
                (u4) baseMethod->methodIndex,
                methodToCall->clazz->descriptor, methodToCall->name);
            //dvmDumpClass(baseMethod->clazz);
            //dvmDumpClass(methodToCall->clazz);
            dvmDumpAllClasses(0);
        }
#endif

        GOTO_invokeMethod(methodCallRange, methodToCall, vsrc1, vdst);
    }
GOTO_TARGET_END

GOTO_TARGET(invokeSuper, bool methodCallRange, bool jumboFormat)
    {
        Method* baseMethod;
        u2 thisReg;

        EXPORT_PC();

        if (jumboFormat) {
            ref = FETCH(1) | (u4)FETCH(2) << 16;  /* method ref */
            vsrc1 = FETCH(3);                     /* count */
            vdst = FETCH(4);                      /* first reg */
            ADJUST_PC(2);     /* advance pc partially to make returns easier */
            ILOGV("|invoke-super/jumbo args=%d @0x%08x {regs=v%d-v%d}",
                vsrc1, ref, vdst, vdst+vsrc1-1);
            thisReg = vdst;
        } else {
            vsrc1 = INST_AA(inst);      /* AA (count) or BA (count + arg 5) */
            ref = FETCH(1);             /* method ref */
            vdst = FETCH(2);            /* 4 regs -or- first reg */

            if (methodCallRange) {
                ILOGV("|invoke-super-range args=%d @0x%04x {regs=v%d-v%d}",
                    vsrc1, ref, vdst, vdst+vsrc1-1);
                thisReg = vdst;
            } else {
                ILOGV("|invoke-super args=%d @0x%04x {regs=0x%04x %x}",
                    vsrc1 >> 4, ref, vdst, vsrc1 & 0x0f);
                thisReg = vdst & 0x0f;
            }
        }

        /* impossible in well-formed code, but we must check nevertheless */
        if (!checkForNull((Object*) GET_REGISTER(thisReg)))
            GOTO_exceptionThrown();

        /*
         * Resolve the method.  This is the correct method for the static
         * type of the object.  We also verify access permissions here.
         * The first arg to dvmResolveMethod() is just the referring class
         * (used for class loaders and such), so we don't want to pass
         * the superclass into the resolution call.
         */
        baseMethod = dvmDexGetResolvedMethod(methodClassDex, ref);
        if (baseMethod == NULL) {
            baseMethod = dvmResolveMethod(curMethod->clazz, ref,METHOD_VIRTUAL);
            if (baseMethod == NULL) {
                ILOGV("+ unknown method or access denied\n");
                GOTO_exceptionThrown();
            }
        }

        /*
         * Combine the object we found with the vtable offset in the
         * method's class.
         *
         * We're using the current method's class' superclass, not the
         * superclass of "this".  This is because we might be executing
         * in a method inherited from a superclass, and we want to run
         * in that class' superclass.
         */
        if (baseMethod->methodIndex >= curMethod->clazz->super->vtableCount) {
            /*
             * Method does not exist in the superclass.  Could happen if
             * superclass gets updated.
             */
            dvmThrowException("Ljava/lang/NoSuchMethodError;",
                baseMethod->name);
            GOTO_exceptionThrown();
        }
        methodToCall = curMethod->clazz->super->vtable[baseMethod->methodIndex];
#if 0
        if (dvmIsAbstractMethod(methodToCall)) {
            dvmThrowException("Ljava/lang/AbstractMethodError;",
                "abstract method not implemented");
            GOTO_exceptionThrown();
        }
#else
        assert(!dvmIsAbstractMethod(methodToCall) ||
            methodToCall->nativeFunc != NULL);
#endif
        LOGVV("+++ base=%s.%s super-virtual=%s.%s\n",
            baseMethod->clazz->descriptor, baseMethod->name,
            methodToCall->clazz->descriptor, methodToCall->name);
        assert(methodToCall != NULL);

        GOTO_invokeMethod(methodCallRange, methodToCall, vsrc1, vdst);
    }
GOTO_TARGET_END

GOTO_TARGET(invokeInterface, bool methodCallRange, bool jumboFormat)
    {
        Object* thisPtr;
        ClassObject* thisClass;

        EXPORT_PC();

        if (jumboFormat) {
            ref = FETCH(1) | (u4)FETCH(2) << 16;  /* method ref */
            vsrc1 = FETCH(3);                     /* count */
            vdst = FETCH(4);                      /* first reg */
            ADJUST_PC(2);     /* advance pc partially to make returns easier */
            ILOGV("|invoke-interface/jumbo args=%d @0x%08x {regs=v%d-v%d}",
                vsrc1, ref, vdst, vdst+vsrc1-1);
            thisPtr = (Object*) GET_REGISTER(vdst);
        } else {
            vsrc1 = INST_AA(inst);      /* AA (count) or BA (count + arg 5) */
            ref = FETCH(1);             /* method ref */
            vdst = FETCH(2);            /* 4 regs -or- first reg */

            /*
             * The object against which we are executing a method is always
             * in the first argument.
             */
            if (methodCallRange) {
                assert(vsrc1 > 0);
                ILOGV("|invoke-interface-range args=%d @0x%04x {regs=v%d-v%d}",
                    vsrc1, ref, vdst, vdst+vsrc1-1);
                thisPtr = (Object*) GET_REGISTER(vdst);
            } else {
                assert((vsrc1>>4) > 0);
                ILOGV("|invoke-interface args=%d @0x%04x {regs=0x%04x %x}",
                    vsrc1 >> 4, ref, vdst, vsrc1 & 0x0f);
                thisPtr = (Object*) GET_REGISTER(vdst & 0x0f);
            }
        }

        if (!checkForNull(thisPtr))
            GOTO_exceptionThrown();

        thisClass = thisPtr->clazz;

#if defined(WITH_JIT) && (INTERP_TYPE == INTERP_DBG)
        callsiteClass = thisClass;
#endif

        /*
         * Given a class and a method index, find the Method* with the
         * actual code we want to execute.
         */
        methodToCall = dvmFindInterfaceMethodInCache(thisClass, ref, curMethod,
                        methodClassDex);
        if (methodToCall == NULL) {
            assert(dvmCheckException(self));
            GOTO_exceptionThrown();
        }

        GOTO_invokeMethod(methodCallRange, methodToCall, vsrc1, vdst);
    }
GOTO_TARGET_END

GOTO_TARGET(invokeDirect, bool methodCallRange, bool jumboFormat)
    {
        u2 thisReg;

        EXPORT_PC();

        if (jumboFormat) {
            ref = FETCH(1) | (u4)FETCH(2) << 16;  /* method ref */
            vsrc1 = FETCH(3);                     /* count */
            vdst = FETCH(4);                      /* first reg */
            ADJUST_PC(2);     /* advance pc partially to make returns easier */
            ILOGV("|invoke-direct/jumbo args=%d @0x%08x {regs=v%d-v%d}",
                vsrc1, ref, vdst, vdst+vsrc1-1);
            thisReg = vdst;
        } else {
            vsrc1 = INST_AA(inst);      /* AA (count) or BA (count + arg 5) */
            ref = FETCH(1);             /* method ref */
            vdst = FETCH(2);            /* 4 regs -or- first reg */

            if (methodCallRange) {
                ILOGV("|invoke-direct-range args=%d @0x%04x {regs=v%d-v%d}",
                    vsrc1, ref, vdst, vdst+vsrc1-1);
                thisReg = vdst;
            } else {
                ILOGV("|invoke-direct args=%d @0x%04x {regs=0x%04x %x}",
                    vsrc1 >> 4, ref, vdst, vsrc1 & 0x0f);
                thisReg = vdst & 0x0f;
            }
        }

        if (!checkForNull((Object*) GET_REGISTER(thisReg)))
            GOTO_exceptionThrown();

        methodToCall = dvmDexGetResolvedMethod(methodClassDex, ref);
        if (methodToCall == NULL) {
            methodToCall = dvmResolveMethod(curMethod->clazz, ref,
                            METHOD_DIRECT);
            if (methodToCall == NULL) {
                ILOGV("+ unknown direct method\n");     // should be impossible
                GOTO_exceptionThrown();
            }
        }
        GOTO_invokeMethod(methodCallRange, methodToCall, vsrc1, vdst);
    }
GOTO_TARGET_END

GOTO_TARGET(invokeStatic, bool methodCallRange, bool jumboFormat)
    EXPORT_PC();

    if (jumboFormat) {
        ref = FETCH(1) | (u4)FETCH(2) << 16;  /* method ref */
        vsrc1 = FETCH(3);                     /* count */
        vdst = FETCH(4);                      /* first reg */
        ADJUST_PC(2);     /* advance pc partially to make returns easier */
        ILOGV("|invoke-static/jumbo args=%d @0x%08x {regs=v%d-v%d}",
            vsrc1, ref, vdst, vdst+vsrc1-1);
    } else {
        vsrc1 = INST_AA(inst);      /* AA (count) or BA (count + arg 5) */
        ref = FETCH(1);             /* method ref */
        vdst = FETCH(2);            /* 4 regs -or- first reg */

        if (methodCallRange)
            ILOGV("|invoke-static-range args=%d @0x%04x {regs=v%d-v%d}",
                vsrc1, ref, vdst, vdst+vsrc1-1);
        else
            ILOGV("|invoke-static args=%d @0x%04x {regs=0x%04x %x}",
                vsrc1 >> 4, ref, vdst, vsrc1 & 0x0f);
    }

    methodToCall = dvmDexGetResolvedMethod(methodClassDex, ref);
    if (methodToCall == NULL) {
        methodToCall = dvmResolveMethod(curMethod->clazz, ref, METHOD_STATIC);
        if (methodToCall == NULL) {
            ILOGV("+ unknown method\n");
            GOTO_exceptionThrown();
        }

        /*
         * The JIT needs dvmDexGetResolvedMethod() to return non-null.
         * Since we use the portable interpreter to build the trace, this extra
         * check is not needed for mterp.
         */
        if (dvmDexGetResolvedMethod(methodClassDex, ref) == NULL) {
            /* Class initialization is still ongoing */
            END_JIT_TSELECT();
        }
    }
    GOTO_invokeMethod(methodCallRange, methodToCall, vsrc1, vdst);
GOTO_TARGET_END

GOTO_TARGET(invokeVirtualQuick, bool methodCallRange, bool jumboFormat)
    {
        Object* thisPtr;

        EXPORT_PC();

        vsrc1 = INST_AA(inst);      /* AA (count) or BA (count + arg 5) */
        ref = FETCH(1);             /* vtable index */
        vdst = FETCH(2);            /* 4 regs -or- first reg */

        /*
         * The object against which we are executing a method is always
         * in the first argument.
         */
        if (methodCallRange) {
            assert(vsrc1 > 0);
            ILOGV("|invoke-virtual-quick-range args=%d @0x%04x {regs=v%d-v%d}",
                vsrc1, ref, vdst, vdst+vsrc1-1);
            thisPtr = (Object*) GET_REGISTER(vdst);
        } else {
            assert((vsrc1>>4) > 0);
            ILOGV("|invoke-virtual-quick args=%d @0x%04x {regs=0x%04x %x}",
                vsrc1 >> 4, ref, vdst, vsrc1 & 0x0f);
            thisPtr = (Object*) GET_REGISTER(vdst & 0x0f);
        }

        if (!checkForNull(thisPtr))
            GOTO_exceptionThrown();

#if defined(WITH_JIT) && (INTERP_TYPE == INTERP_DBG)
        callsiteClass = thisPtr->clazz;
#endif

        /*
         * Combine the object we found with the vtable offset in the
         * method.
         */
        assert(ref < (unsigned int) thisPtr->clazz->vtableCount);
        methodToCall = thisPtr->clazz->vtable[ref];

#if 0
        if (dvmIsAbstractMethod(methodToCall)) {
            dvmThrowException("Ljava/lang/AbstractMethodError;",
                "abstract method not implemented");
            GOTO_exceptionThrown();
        }
#else
        assert(!dvmIsAbstractMethod(methodToCall) ||
            methodToCall->nativeFunc != NULL);
#endif

        LOGVV("+++ virtual[%d]=%s.%s\n",
            ref, methodToCall->clazz->descriptor, methodToCall->name);
        assert(methodToCall != NULL);

        GOTO_invokeMethod(methodCallRange, methodToCall, vsrc1, vdst);
    }
GOTO_TARGET_END

GOTO_TARGET(invokeSuperQuick, bool methodCallRange, bool jumboFormat)
    {
        u2 thisReg;

        EXPORT_PC();

        vsrc1 = INST_AA(inst);      /* AA (count) or BA (count + arg 5) */
        ref = FETCH(1);             /* vtable index */
        vdst = FETCH(2);            /* 4 regs -or- first reg */

        if (methodCallRange) {
            ILOGV("|invoke-super-quick-range args=%d @0x%04x {regs=v%d-v%d}",
                vsrc1, ref, vdst, vdst+vsrc1-1);
            thisReg = vdst;
        } else {
            ILOGV("|invoke-super-quick args=%d @0x%04x {regs=0x%04x %x}",
                vsrc1 >> 4, ref, vdst, vsrc1 & 0x0f);
            thisReg = vdst & 0x0f;
        }
        /* impossible in well-formed code, but we must check nevertheless */
        if (!checkForNull((Object*) GET_REGISTER(thisReg)))
            GOTO_exceptionThrown();

#if 0   /* impossible in optimized + verified code */
        if (ref >= curMethod->clazz->super->vtableCount) {
            dvmThrowException("Ljava/lang/NoSuchMethodError;", NULL);
            GOTO_exceptionThrown();
        }
#else
        assert(ref < (unsigned int) curMethod->clazz->super->vtableCount);
#endif

        /*
         * Combine the object we found with the vtable offset in the
         * method's class.
         *
         * We're using the current method's class' superclass, not the
         * superclass of "this".  This is because we might be executing
         * in a method inherited from a superclass, and we want to run
         * in the method's class' superclass.
         */
        methodToCall = curMethod->clazz->super->vtable[ref];

#if 0
        if (dvmIsAbstractMethod(methodToCall)) {
            dvmThrowException("Ljava/lang/AbstractMethodError;",
                "abstract method not implemented");
            GOTO_exceptionThrown();
        }
#else
        assert(!dvmIsAbstractMethod(methodToCall) ||
            methodToCall->nativeFunc != NULL);
#endif
        LOGVV("+++ super-virtual[%d]=%s.%s\n",
            ref, methodToCall->clazz->descriptor, methodToCall->name);
        assert(methodToCall != NULL);

        GOTO_invokeMethod(methodCallRange, methodToCall, vsrc1, vdst);
    }
GOTO_TARGET_END


    /*
     * General handling for return-void, return, and return-wide.  Put the
     * return value in "retval" before jumping here.
     */
GOTO_TARGET(returnFromMethod)
    {
        StackSaveArea* saveArea;

        /*
         * We must do this BEFORE we pop the previous stack frame off, so
         * that the GC can see the return value (if any) in the local vars.
         *
         * Since this is now an interpreter switch point, we must do it before
         * we do anything at all.
         */
        PERIODIC_CHECKS(kInterpEntryReturn, 0);

        ILOGV("> retval=0x%llx (leaving %s.%s %s)",
            retval.j, curMethod->clazz->descriptor, curMethod->name,
            curMethod->shorty);
        //DUMP_REGS(curMethod, fp);

        saveArea = SAVEAREA_FROM_FP(fp);

#ifdef EASY_GDB
        debugSaveArea = saveArea;
#endif
#if (INTERP_TYPE == INTERP_DBG)
        TRACE_METHOD_EXIT(self, curMethod);
#endif

        /* back up to previous frame and see if we hit a break */
        fp = (u4*)saveArea->prevFrame;
        assert(fp != NULL);
        if (dvmIsBreakFrame(fp)) {
            /* bail without popping the method frame from stack */
            LOGVV("+++ returned into break frame\n");
#if defined(WITH_JIT)
            /* Let the Jit know the return is terminating normally */
            CHECK_JIT_VOID();
#endif
            GOTO_bail();
        }

        /* update thread FP, and reset local variables */
        self->curFrame = fp;
        curMethod = SAVEAREA_FROM_FP(fp)->method;
        //methodClass = curMethod->clazz;
        methodClassDex = curMethod->clazz->pDvmDex;
        pc = saveArea->savedPc;
        ILOGD("> (return to %s.%s %s)", curMethod->clazz->descriptor,
            curMethod->name, curMethod->shorty);

        /* use FINISH on the caller's invoke instruction */
        //u2 invokeInstr = INST_INST(FETCH(0));
        if (true /*invokeInstr >= OP_INVOKE_VIRTUAL &&
            invokeInstr <= OP_INVOKE_INTERFACE*/)
        {
            FINISH(3);
        } else {
            //LOGE("Unknown invoke instr %02x at %d\n",
            //    invokeInstr, (int) (pc - curMethod->insns));
            assert(false);
        }
    }
GOTO_TARGET_END


    /*
     * Jump here when the code throws an exception.
     *
     * By the time we get here, the Throwable has been created and the stack
     * trace has been saved off.
     */
GOTO_TARGET(exceptionThrown)
    {
        Object* exception;
        int catchRelPc;

        /*
         * Since this is now an interpreter switch point, we must do it before
         * we do anything at all.
         */
        PERIODIC_CHECKS(kInterpEntryThrow, 0);

#if defined(WITH_JIT)
        // Something threw during trace selection - end the current trace
        END_JIT_TSELECT();
#endif
        /*
         * We save off the exception and clear the exception status.  While
         * processing the exception we might need to load some Throwable
         * classes, and we don't want class loader exceptions to get
         * confused with this one.
         */
        assert(dvmCheckException(self));
        exception = dvmGetException(self);
        dvmAddTrackedAlloc(exception, self);
        dvmClearException(self);

        LOGV("Handling exception %s at %s:%d\n",
            exception->clazz->descriptor, curMethod->name,
            dvmLineNumFromPC(curMethod, pc - curMethod->insns));

#if (INTERP_TYPE == INTERP_DBG)
        /*
         * Tell the debugger about it.
         *
         * TODO: if the exception was thrown by interpreted code, control
         * fell through native, and then back to us, we will report the
         * exception at the point of the throw and again here.  We can avoid
         * this by not reporting exceptions when we jump here directly from
         * the native call code above, but then we won't report exceptions
         * that were thrown *from* the JNI code (as opposed to *through* it).
         *
         * The correct solution is probably to ignore from-native exceptions
         * here, and have the JNI exception code do the reporting to the
         * debugger.
         */
        if (DEBUGGER_ACTIVE) {
            void* catchFrame;
            catchRelPc = dvmFindCatchBlock(self, pc - curMethod->insns,
                        exception, true, &catchFrame);
            dvmDbgPostException(fp, pc - curMethod->insns, catchFrame,
                catchRelPc, exception);
        }
#endif

        /*
         * We need to unroll to the catch block or the nearest "break"
         * frame.
         *
         * A break frame could indicate that we have reached an intermediate
         * native call, or have gone off the top of the stack and the thread
         * needs to exit.  Either way, we return from here, leaving the
         * exception raised.
         *
         * If we do find a catch block, we want to transfer execution to
         * that point.
         *
         * Note this can cause an exception while resolving classes in
         * the "catch" blocks.
         */
        catchRelPc = dvmFindCatchBlock(self, pc - curMethod->insns,
                    exception, false, (void**)(void*)&fp);

        /*
         * Restore the stack bounds after an overflow.  This isn't going to
         * be correct in all circumstances, e.g. if JNI code devours the
         * exception this won't happen until some other exception gets
         * thrown.  If the code keeps pushing the stack bounds we'll end
         * up aborting the VM.
         *
         * Note we want to do this *after* the call to dvmFindCatchBlock,
         * because that may need extra stack space to resolve exception
         * classes (e.g. through a class loader).
         *
         * It's possible for the stack overflow handling to cause an
         * exception (specifically, class resolution in a "catch" block
         * during the call above), so we could see the thread's overflow
         * flag raised but actually be running in a "nested" interpreter
         * frame.  We don't allow doubled-up StackOverflowErrors, so
         * we can check for this by just looking at the exception type
         * in the cleanup function.  Also, we won't unroll past the SOE
         * point because the more-recent exception will hit a break frame
         * as it unrolls to here.
         */
        if (self->stackOverflowed)
            dvmCleanupStackOverflow(self, exception);

        if (catchRelPc < 0) {
            /* falling through to JNI code or off the bottom of the stack */
#if DVM_SHOW_EXCEPTION >= 2
            LOGD("Exception %s from %s:%d not caught locally\n",
                exception->clazz->descriptor, dvmGetMethodSourceFile(curMethod),
                dvmLineNumFromPC(curMethod, pc - curMethod->insns));
#endif
            dvmSetException(self, exception);
            dvmReleaseTrackedAlloc(exception, self);
            GOTO_bail();
        }

#if DVM_SHOW_EXCEPTION >= 3
        {
            const Method* catchMethod = SAVEAREA_FROM_FP(fp)->method;
            LOGD("Exception %s thrown from %s:%d to %s:%d\n",
                exception->clazz->descriptor, dvmGetMethodSourceFile(curMethod),
                dvmLineNumFromPC(curMethod, pc - curMethod->insns),
                dvmGetMethodSourceFile(catchMethod),
                dvmLineNumFromPC(catchMethod, catchRelPc));
        }
#endif

        /*
         * Adjust local variables to match self->curFrame and the
         * updated PC.
         */
        //fp = (u4*) self->curFrame;
        curMethod = SAVEAREA_FROM_FP(fp)->method;
        //methodClass = curMethod->clazz;
        methodClassDex = curMethod->clazz->pDvmDex;
        pc = curMethod->insns + catchRelPc;
        ILOGV("> pc <-- %s.%s %s", curMethod->clazz->descriptor,
            curMethod->name, curMethod->shorty);
        DUMP_REGS(curMethod, fp, false);            // show all regs

        /*
         * Restore the exception if the handler wants it.
         *
         * The Dalvik spec mandates that, if an exception handler wants to
         * do something with the exception, the first instruction executed
         * must be "move-exception".  We can pass the exception along
         * through the thread struct, and let the move-exception instruction
         * clear it for us.
         *
         * If the handler doesn't call move-exception, we don't want to
         * finish here with an exception still pending.
         */
        if (INST_INST(FETCH(0)) == OP_MOVE_EXCEPTION)
            dvmSetException(self, exception);

        dvmReleaseTrackedAlloc(exception, self);
        FINISH(0);
    }
GOTO_TARGET_END



    /*
     * General handling for invoke-{virtual,super,direct,static,interface},
     * including "quick" variants.
     *
     * Set "methodToCall" to the Method we're calling, and "methodCallRange"
     * depending on whether this is a "/range" instruction.
     *
     * For a range call:
     *  "vsrc1" holds the argument count (8 bits)
     *  "vdst" holds the first argument in the range
     * For a non-range call:
     *  "vsrc1" holds the argument count (4 bits) and the 5th argument index
     *  "vdst" holds four 4-bit register indices
     *
     * The caller must EXPORT_PC before jumping here, because any method
     * call can throw a stack overflow exception.
     */
GOTO_TARGET(invokeMethod, bool methodCallRange, const Method* _methodToCall,
    u2 count, u2 regs)
    {
        STUB_HACK(vsrc1 = count; vdst = regs; methodToCall = _methodToCall;);

        //printf("range=%d call=%p count=%d regs=0x%04x\n",
        //    methodCallRange, methodToCall, count, regs);
        //printf(" --> %s.%s %s\n", methodToCall->clazz->descriptor,
        //    methodToCall->name, methodToCall->shorty);

        u4* outs;
        int i;

        /*
         * Copy args.  This may corrupt vsrc1/vdst.
         */
        if (methodCallRange) {
            // could use memcpy or a "Duff's device"; most functions have
            // so few args it won't matter much
            assert(vsrc1 <= curMethod->outsSize);
            assert(vsrc1 == methodToCall->insSize);
            outs = OUTS_FROM_FP(fp, vsrc1);
            for (i = 0; i < vsrc1; i++)
                outs[i] = GET_REGISTER(vdst+i);
        } else {
            u4 count = vsrc1 >> 4;

            assert(count <= curMethod->outsSize);
            assert(count == methodToCall->insSize);
            assert(count <= 5);

            outs = OUTS_FROM_FP(fp, count);
#if 0
            if (count == 5) {
                outs[4] = GET_REGISTER(vsrc1 & 0x0f);
                count--;
            }
            for (i = 0; i < (int) count; i++) {
                outs[i] = GET_REGISTER(vdst & 0x0f);
                vdst >>= 4;
            }
#else
            // This version executes fewer instructions but is larger
            // overall.  Seems to be a teensy bit faster.
            assert((vdst >> 16) == 0);  // 16 bits -or- high 16 bits clear
            switch (count) {
            case 5:
                outs[4] = GET_REGISTER(vsrc1 & 0x0f);
            case 4:
                outs[3] = GET_REGISTER(vdst >> 12);
            case 3:
                outs[2] = GET_REGISTER((vdst & 0x0f00) >> 8);
            case 2:
                outs[1] = GET_REGISTER((vdst & 0x00f0) >> 4);
            case 1:
                outs[0] = GET_REGISTER(vdst & 0x0f);
            default:
                ;
            }
#endif
        }
    }

    /*
     * (This was originally a "goto" target; I've kept it separate from the
     * stuff above in case we want to refactor things again.)
     *
     * At this point, we have the arguments stored in the "outs" area of
     * the current method's stack frame, and the method to call in
     * "methodToCall".  Push a new stack frame.
     */
    {
        StackSaveArea* newSaveArea;
        u4* newFp;

        ILOGV("> %s%s.%s %s",
            dvmIsNativeMethod(methodToCall) ? "(NATIVE) " : "",
            methodToCall->clazz->descriptor, methodToCall->name,
            methodToCall->shorty);

        newFp = (u4*) SAVEAREA_FROM_FP(fp) - methodToCall->registersSize;
        newSaveArea = SAVEAREA_FROM_FP(newFp);

        /* verify that we have enough space */
        if (true) {
            u1* bottom;
            bottom = (u1*) newSaveArea - methodToCall->outsSize * sizeof(u4);
            if (bottom < self->interpStackEnd) {
                /* stack overflow */
                LOGV("Stack overflow on method call (start=%p end=%p newBot=%p(%d) size=%d '%s')\n",
                    self->interpStackStart, self->interpStackEnd, bottom,
                    (u1*) fp - bottom, self->interpStackSize,
                    methodToCall->name);
                dvmHandleStackOverflow(self, methodToCall);
                assert(dvmCheckException(self));
                GOTO_exceptionThrown();
            }
            //LOGD("+++ fp=%p newFp=%p newSave=%p bottom=%p\n",
            //    fp, newFp, newSaveArea, bottom);
        }

#ifdef LOG_INSTR
        if (methodToCall->registersSize > methodToCall->insSize) {
            /*
             * This makes valgrind quiet when we print registers that
             * haven't been initialized.  Turn it off when the debug
             * messages are disabled -- we want valgrind to report any
             * used-before-initialized issues.
             */
            memset(newFp, 0xcc,
                (methodToCall->registersSize - methodToCall->insSize) * 4);
        }
#endif

#ifdef EASY_GDB
        newSaveArea->prevSave = SAVEAREA_FROM_FP(fp);
#endif
        newSaveArea->prevFrame = fp;
        newSaveArea->savedPc = pc;
#if defined(WITH_JIT)
        newSaveArea->returnAddr = 0;
#endif
        newSaveArea->method = methodToCall;

        if (!dvmIsNativeMethod(methodToCall)) {
            /*
             * "Call" interpreted code.  Reposition the PC, update the
             * frame pointer and other local state, and continue.
             */
            curMethod = methodToCall;
            methodClassDex = curMethod->clazz->pDvmDex;
            pc = methodToCall->insns;
            self->curFrame = fp = newFp;
#ifdef EASY_GDB
            debugSaveArea = SAVEAREA_FROM_FP(newFp);
#endif
#if INTERP_TYPE == INTERP_DBG
            debugIsMethodEntry = true;              // profiling, debugging
#endif
            ILOGD("> pc <-- %s.%s %s", curMethod->clazz->descriptor,
                curMethod->name, curMethod->shorty);
            DUMP_REGS(curMethod, fp, true);         // show input args
            FINISH(0);                              // jump to method start
        } else {
            /* set this up for JNI locals, even if not a JNI native */
#ifdef USE_INDIRECT_REF
            newSaveArea->xtra.localRefCookie = self->jniLocalRefTable.segmentState.all;
#else
            newSaveArea->xtra.localRefCookie = self->jniLocalRefTable.nextEntry;
#endif

            self->curFrame = newFp;

            DUMP_REGS(methodToCall, newFp, true);   // show input args

#if (INTERP_TYPE == INTERP_DBG)
            if (DEBUGGER_ACTIVE) {
                dvmDbgPostLocationEvent(methodToCall, -1,
                    dvmGetThisPtr(curMethod, fp), DBG_METHOD_ENTRY);
            }
#endif
#if (INTERP_TYPE == INTERP_DBG)
            TRACE_METHOD_ENTER(self, methodToCall);
#endif

            {
                ILOGD("> native <-- %s.%s %s", methodToCall->clazz->descriptor,
                        methodToCall->name, methodToCall->shorty);
            }

#if defined(WITH_JIT)
            /* Allow the Jit to end any pending trace building */
            CHECK_JIT_VOID();
#endif

            /*
             * Jump through native call bridge.  Because we leave no
             * space for locals on native calls, "newFp" points directly
             * to the method arguments.
             */
            (*methodToCall->nativeFunc)(newFp, &retval, methodToCall, self);

#if (INTERP_TYPE == INTERP_DBG)
            if (DEBUGGER_ACTIVE) {
                dvmDbgPostLocationEvent(methodToCall, -1,
                    dvmGetThisPtr(curMethod, fp), DBG_METHOD_EXIT);
            }
#endif
#if (INTERP_TYPE == INTERP_DBG)
            TRACE_METHOD_EXIT(self, methodToCall);
#endif

            /* pop frame off */
            dvmPopJniLocals(self, newSaveArea);
            self->curFrame = fp;

            /*
             * If the native code threw an exception, or interpreted code
             * invoked by the native call threw one and nobody has cleared
             * it, jump to our local exception handling.
             */
            if (dvmCheckException(self)) {
                LOGV("Exception thrown by/below native code\n");
                GOTO_exceptionThrown();
            }

            ILOGD("> retval=0x%llx (leaving native)", retval.j);
            ILOGD("> (return from native %s.%s to %s.%s %s)",
                methodToCall->clazz->descriptor, methodToCall->name,
                curMethod->clazz->descriptor, curMethod->name,
                curMethod->shorty);

            //u2 invokeInstr = INST_INST(FETCH(0));
            if (true /*invokeInstr >= OP_INVOKE_VIRTUAL &&
                invokeInstr <= OP_INVOKE_INTERFACE*/)
            {
                FINISH(3);
            } else {
                //LOGE("Unknown invoke instr %02x at %d\n",
                //    invokeInstr, (int) (pc - curMethod->insns));
                assert(false);
            }
        }
    }
    assert(false);      // should not get here
GOTO_TARGET_END

/* File: portable/enddefs.c */
/*--- end of opcodes ---*/

#ifndef THREADED_INTERP
        } // end of "switch"
    } // end of "while"
#endif

bail:
    ILOGD("|-- Leaving interpreter loop");      // note "curMethod" may be NULL

    interpState->retval = retval;
    return false;

bail_switch:
    /*
     * The standard interpreter currently doesn't set or care about the
     * "debugIsMethodEntry" value, so setting this is only of use if we're
     * switching between two "debug" interpreters, which we never do.
     *
     * TODO: figure out if preserving this makes any sense.
     */
#if INTERP_TYPE == INTERP_DBG
    interpState->debugIsMethodEntry = debugIsMethodEntry;
#else
    interpState->debugIsMethodEntry = false;
#endif

    /* export state changes */
    interpState->method = curMethod;
    interpState->pc = pc;
    interpState->fp = fp;
    /* debugTrackedRefStart doesn't change */
    interpState->retval = retval;   /* need for _entryPoint=ret */
    interpState->nextMode =
        (INTERP_TYPE == INTERP_STD) ? INTERP_DBG : INTERP_STD;
    LOGVV(" meth='%s.%s' pc=0x%x fp=%p\n",
        curMethod->clazz->descriptor, curMethod->name,
        pc - curMethod->insns, fp);
    return true;
}

