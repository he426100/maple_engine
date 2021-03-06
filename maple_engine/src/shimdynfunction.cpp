/*
 * Copyright (C) [2021] Futurewei Technologies, Inc. All rights reserved.
 *
 * OpenArkCompiler is licensed underthe Mulan Permissive Software License v2.
 * You can use this software according to the terms and conditions of the MulanPSL - 2.0.
 * You may obtain a copy of MulanPSL - 2.0 at:
 *
 *   https://opensource.org/licenses/MulanPSL-2.0
 *
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY OR
 * FIT FOR A PARTICULAR PURPOSE.
 * See the MulanPSL - 2.0 for more details.
 */

#include <cstdarg>
#include <cstring>

#include "mfunction.h"
#include "massert.h" // for MASSERT
#include "mshimdyn.h"
#include "vmmemory.h"
#include "jsvalueinline.h"
#include "jstycnv.h"
#include "jsiter.h"
#include "ccall.h"
#include <string.h>
#include "jsarray.h"
#include "jsregexp.h"
#include "jsdate.h"
#include "jseh.h"
#include "jsobjectinline.h"


namespace maple {

#define MPUSH(x) (shim_caller.operand_stack.at(++shim_caller.sp) = x)

JavaScriptGlobal *jsGlobal = NULL;
uint32_t *jsGlobalMemmap = NULL;
InterSource *gInterSource = NULL;


uint8 InterSource::ptypesizetable[kPtyDerived] = {
  0, // PTY_invalid
  0, // PTY_(void)
  1, // PTY_i8
  2, // PTY_i16
  4, // PTY_i32
  8, // PTY_i64
  1, // PTY_u8
  2, // PTY_u16
  4, // PTY_u32
  8, // PTY_u64
  1, // PTY_u1
  4, // PTY_ptr
  4, // PTY_ref
  4, // PTY_a32
  8, // PTY_a64
  4, // PTY_f32
  8, // PTY_f64
  16, // PTY_f128
  16, // PTY_c64
  32, // PTY_c128
  8, // PTY_simplestr
  8, // PTY_simpleobj
  8, // PTY_dynany
  8, // PTY_dynundef
  8, // PTY_dynnull
  8, // PTY_dynbool
  8, // PTY_dyni32
  8, // PTY_dynstr
  8, // PTY_dynobj
  8, // PTY_dynnone
  8, // PTY_dynf64
  8, // PTY_dynf32
  0 // PTY_agg
};

InterSource::InterSource() {
  memory = (void *)malloc(TOTAL_MEMORY_SIZE);
  void * internalMemory = (void *)((char *)memory + APP_MEMORY_SIZE);
  stack = STACKOFFSET;
  sp = stack;
  fp = stack;
  heap = 0;
  retVal0.x.u64 = 0;
  currEH = nullptr;
  EHstackReuseSize = 0;

  // retVal0.payload.asbits = 0;
  memory_manager = new MemoryManager();
  memory_manager->Init(memory, HEAP_SIZE, internalMemory, VM_MEMORY_SIZE);
  gInterSource = this;
  // currEH = NULL;
  // EHstackReuseSize = 0;
}

void InterSource::SetRetval0 (MValue mval, bool isdyntype) {
  if (!isdyntype)
    SetMValueTag(mval, 0);
  GCCheckAndUpdateRf(retVal0.x.u64, mval.x.u64);
  retVal0 = mval;
}

void InterSource::SetRetval0Object (void *obj, bool isdyntype) {
  GCIncRf(obj);
  GCCheckAndDecRf(retVal0.x.u64);
  retVal0.x.a64 = (uint8_t *)obj;
}

void InterSource::SetRetval0NoInc (uint64_t val) {
  GCCheckAndDecRf(retVal0.x.u64);
  retVal0.x.u64 = val;
}

void InterSource::EmulateStore(uint8_t *memory, MValue mval, PrimType pty) {
  uint32_t bytesize = ptypesizetable[pty];
  if (bytesize == 8) {
    *((uint64_t *)memory) = mval.x.u64;
    return;
  }
  if (bytesize == 4) {
    *((uint32_t *)memory) = mval.x.u32;
    return;
  }
  if (bytesize == 2) {
    *((uint16_t *)memory) = mval.x.u32;
    return;
  }
  if (bytesize == 1) {
    *(memory) = mval.x.u32; return;
    return;
  }
  assert(false && "unexpected type");
  return;
}

void InterSource::EmulateStoreGC(uint8_t *memory, MValue mval, PrimType pty) {
  if (pty == PTY_simpleobj || pty == PTY_simplestr) {
    GCUpdateRf((void *)(*(uint64_t *)memory), mval.x.a64);
    *(uint64_t *)memory = mval.x.u64;
    return;
  }
  if (IsPrimitiveDyn(pty)) {
    GCCheckAndUpdateRf(*(uint64_t *)(memory), mval.x.u64);
    *(uint64_t *)(memory) = mval.x.u64;
    return;
  }
  EmulateStore(memory, mval, pty);
}

uint64_t InterSource::EmulateLoad(uint8_t *memory, PrimType pty) {
  uint32 bytesize = ptypesizetable[pty];
  if (bytesize == 8)
    return  *((uint64_t *)memory);
  if (bytesize == 4)
    return *((uint32_t *)memory);
  if (bytesize == 2)
    return *((uint16_t *)memory);
  if (bytesize == 1)
    return *memory;
  assert(false && "unexpected type");
  return (uint64_t) 0;
}

int32_t InterSource::PassArguments(MValue this_arg, void *env, MValue *arg_list,
                              int32_t narg, int32_t func_narg) {
  // The arguments' number of a function must be [0, 255].
  // If func_narg = -1, means func_narg equal to narg.
  assert(func_narg <= 255 && func_narg >= -1 && "error");
  assert(narg <= 255 && narg >= 0 && "error");

  uint8 *spaddr = (uint8 *)GetSPAddr();
  int32_t offset = 0;

  // If actual arguments is less than formal arguments, push undefined value,
  // else ignore the redundant actual arguments.
  // It's a C style call, formal arguments' number must be equal the actuals'.
  if (func_narg == -1) {
    // Do nothing.
  } else if (func_narg - narg > 0) {
    MValue undefined = JsvalToMValue(__undefined_value());
    for (int32_t i = func_narg - narg; i > 0; i--) {
      offset -= MVALSIZE;
      ALIGNMENTNEGOFFSET(offset, MVALSIZE);
      EmulateStore(spaddr + offset, undefined, PTY_u64);
    }
  } else /* Set the actual argument-number to the function acceptant. */ {
    narg = func_narg;
  }
  // Pass actual arguments.
  for (int32_t i = narg - 1; i >= 0; i--) {
    MValue actual = arg_list[i];
    offset -= MVALSIZE;
    ALIGNMENTNEGOFFSET(offset, MVALSIZE);
    EmulateStore(spaddr + offset, actual, PTY_u64);
    GCCheckAndIncRf(actual.x.u64);
  }
  if (env) {
    offset -= PTRSIZE + MVALSIZE;
  } else {
    offset -= MVALSIZE;
  }
  ALIGNMENTNEGOFFSET(offset, MVALSIZE);
  // Pass env pointer.
  if (env) {
    MValue envMal;
    envMal.x.a64 = (uint8_t *)env;
    // SetMValueValue(envMal, memory_manager->MapRealAddr(env));
    // SetMValueTag(envMal, JSTYPE_ENV);
    EmulateStore(spaddr + offset + MVALSIZE, envMal, PTY_u64);
    // GCCheckAndIncRf(envMal.x.u64);
  }
  // Pass the 'this'.
  EmulateStore(spaddr + offset, this_arg, PTY_u64);
  GCCheckAndIncRf(this_arg.x.u64);
  return offset;
}

MValue InterSource::VmJSopAdd(MValue mv0, MValue mv1) {
  __jsvalue v0 = MValueToJsval(mv0);
  __jsvalue v1 = MValueToJsval(mv1);
  return JsvalToMValue(__jsop_add(&v0, &v1));
}

MValue InterSource::PrimAdd(MValue mv0, MValue mv1, PrimType ptyp) {
  MValue res;
  switch (ptyp) {
    case PTY_i8:  res.x.i8  = mv0.x.i8  + mv1.x.i8;  break;
    case PTY_i16: res.x.i16 = mv0.x.i16 + mv1.x.i16; break;
    case PTY_i32: res.x.i32 = mv0.x.i32 + mv1.x.i32; break;
    case PTY_i64: res.x.i64 = mv0.x.i64 + mv1.x.i64; break;
    case PTY_u16: res.x.u16 = mv0.x.u16 + mv1.x.u16; break;
    case PTY_u1:  res.x.u1  = mv0.x.u1  + mv1.x.u1;  break;
    case PTY_a32:
    case PTY_a64: res.x.a64 = mv0.x.a64 + mv1.x.i64; break;
    case PTY_f32: res.x.f32 = mv0.x.f32 + mv1.x.f32; break;
    case PTY_f64: assert(false && "nyi");
    default: assert(false && "error");
  }
  res.ptyp = ptyp;
  return res;
}

MValue InterSource::JSopDiv(MValue &mv0, MValue &mv1, PrimType ptyp, Opcode op) {
  __jstype resTag;
  uint32_t resVal;
  __jstype tag0 = (__jstype)GetMValueTag(mv0);
  __jstype tag1 = (__jstype)GetMValueTag(mv1);
  if (tag0 == JSTYPE_NAN || tag1 == JSTYPE_NAN
    || tag0 == JSTYPE_UNDEFINED || tag1 == JSTYPE_UNDEFINED
    || (tag0 == JSTYPE_NULL && tag1 == JSTYPE_NULL)) {
    return JsvalToMValue(__nan_value());
  }
  switch (tag0) {
    case JSTYPE_BOOLEAN: {
      int32_t x0 = mv0.x.i32;
      switch (tag1) {
        case JSTYPE_BOOLEAN: {
          int32_t x1 = mv1.x.i32;
          if (x1 == 0) {
            return JsvalToMValue(x0 >= 0 ? __number_infinity() : __number_neg_infinity());
          }
          resTag = JSTYPE_NUMBER;
          resVal = x0;
          break;
        }
        case JSTYPE_NUMBER: {
          int32_t x1 = mv1.x.i32;
          if (x1 == 0) {
            return JsvalToMValue(x0 >= 0 ? __number_infinity() : __number_neg_infinity());
          }
          resTag = JSTYPE_NUMBER;
          resVal = x0 / x1;
          break;
        }
        case JSTYPE_OBJECT: {
          __jsvalue v0 = MValueToJsval(mv0);
          __jsvalue v1 = MValueToJsval(mv1);
          return JsvalToMValue(__jsop_object_div(&v0, &v1));
        }
        case JSTYPE_NULL: {
          return JsvalToMValue(__number_infinity());
        }
        case JSTYPE_UNDEFINED: {
          return JsvalToMValue(__nan_value());
        }
        default:
          assert(false && "unexpected div op1");
      }
      break;
    }
    case JSTYPE_NUMBER: {
      int32_t x0 = mv0.x.i32;
      switch (tag1) {
        case JSTYPE_BOOLEAN: {
          int32_t x1 = mv1.x.i32;
          if (x1 == 0) {
            return JsvalToMValue(x0 >= 0 ? __number_infinity() : __number_neg_infinity());
          }
          resTag = JSTYPE_NUMBER;
          resVal = mv0.x.i32 / mv1.x.i32;
          break;
        }
        case JSTYPE_NUMBER: {
          int32_t x1 = mv1.x.i32;
          if (x1 == 0) {
            return JsvalToMValue(x0 >= 0 ? __number_infinity() : __number_neg_infinity());
          }
          resTag = JSTYPE_NUMBER;
          resVal = x0 / x1;
          break;
        }
        case JSTYPE_DOUBLE: {
          resTag = JSTYPE_DOUBLE;
          if (mv1.x.u64 == 0x900000000) {
            // negative_zero
            return JsvalToMValue(x0 < 0 ? __number_infinity() : __number_neg_infinity());
          }
          double x1 = memory_manager->GetF64FromU32(mv1.x.u32);
          if (fabs(x1) < NumberMinValue) {
            return JsvalToMValue(x0 < 0 ? __number_infinity() : __number_neg_infinity());
          }
          double rtx = (double)mv0.x.i32 / x1;
          if (std::isinf(rtx)) {
            resTag = JSTYPE_INFINITY;
            resVal = (rtx == (-1.0/0.0) ? 1 : 0);
          } else {
            resVal = memory_manager->SetF64ToU32(rtx);
          }
          break;
        }
        case JSTYPE_OBJECT: {
          __jsvalue v0 = MValueToJsval(mv0);
          __jsvalue v1 = MValueToJsval(mv1);
          return JsvalToMValue(__jsop_object_div(&v0, &v1));
        }
        case JSTYPE_NULL: {
          return JsvalToMValue(mv0.x.i32 > 0 ? __number_infinity() : __number_neg_infinity());
        }
        case JSTYPE_INFINITY: {
          __jsvalue v1 = MValueToJsval(mv1);
          bool isPos = __is_neg_infinity(&v1) ? x0 < 0 : x0 >= 0;
          return JsvalToMValue(isPos ? __number_value(0) :  __double_value(0));
        }
        default:
          assert(false && "unexpected div op1");
      }
      break;
    }
    case JSTYPE_DOUBLE: {
      double ldb = memory_manager->GetF64FromU32(mv0.x.u32);
      switch (tag1) {
        case JSTYPE_NUMBER:
        case JSTYPE_DOUBLE: {
          resTag = JSTYPE_DOUBLE;
          double rhs = (tag1 == JSTYPE_NUMBER ? (double)mv1.x.i32 : memory_manager->GetF64FromU32(mv1.x.u32));
          if (fabs(rhs) < NumberMinValue) {
            return JsvalToMValue(ldb >= 0 ? __number_infinity() : __number_neg_infinity());
          }
          double rtx = ldb / rhs;
          if (std::isinf(rtx)) {
            resTag = JSTYPE_INFINITY;
            resVal = (rtx == (-1.0/0.0) ? 1 : 0);
          } else {
            resVal = memory_manager->SetF64ToU32(rtx);
          }
          break;
        }
        case JSTYPE_OBJECT: {
          __jsvalue v0 = MValueToJsval(mv0);
          __jsvalue v1 = MValueToJsval(mv1);
          return JsvalToMValue(__jsop_object_div(&v0, &v1));
        }
        case JSTYPE_NULL: {
          double rhs = memory_manager->GetF64FromU32(mv0.x.u32);
          return JsvalToMValue(rhs > 0 ? __number_infinity() : __number_neg_infinity());
        }
        case JSTYPE_INFINITY: {
          return JsvalToMValue(__number_value(0));
        }
        default:
          assert(false && "unexpected div op1");
      }
      break;
    }
    case JSTYPE_OBJECT: {
      __jsvalue v0 = MValueToJsval(mv0);
      __jsvalue v1 = MValueToJsval(mv1);
      return JsvalToMValue(__jsop_object_div(&v0, &v1));
    }
    case JSTYPE_NULL: {
      resTag = JSTYPE_NUMBER;
      resVal = 0;
      break;
    }
    case JSTYPE_INFINITY: {
      bool isPos0 = mv0.x.i32 == 0;
      bool isPos1 = false;
      switch (tag1) {
        case JSTYPE_NUMBER: {
          int32_t val1 = mv1.x.i32;
          if (val1 == 0) {
            return mv0;
          } else {
           isPos1 = val1 > 0;
          }
          break;
        }
        case JSTYPE_DOUBLE: {
          double rhs = memory_manager->GetF64FromU32(mv1.x.u32);
          if (fabs(rhs) < NumberMinValue) {
            return JsvalToMValue(isPos0 ? __number_neg_infinity() : __number_infinity());
          } else {
            isPos1 = rhs > 0;
          }
          break;
        }
        case JSTYPE_OBJECT: {
          __jsvalue v0 = MValueToJsval(mv0);
          __jsvalue v1 = MValueToJsval(mv1);
          return JsvalToMValue(__jsop_object_div(&v0, &v1));
        }
        case JSTYPE_NULL: {
          assert(false && "unexpected sub op1");
          resTag = JSTYPE_NUMBER;
          resVal = 0;
          break;
        }
        case JSTYPE_INFINITY: {
          return JsvalToMValue(__nan_value());
        }
        default:
          assert(false && "unexpected div op1");
      }
      return (isPos0 == isPos1) ? JsvalToMValue(__number_infinity()) : JsvalToMValue(__number_neg_infinity());
    }
    case JSTYPE_UNDEFINED: {
      return JsvalToMValue(__nan_value());
    }
    case JSTYPE_STRING: {
      bool isConvertible = false;
      __jsvalue v0 = MValueToJsval(mv0);
      __jsvalue leftV = __js_ToNumberSlow2(&v0, isConvertible);
      if (!isConvertible) {
        return JsvalToMValue(__nan_value());
      } else {
        MValue newMv0 = JsvalToMValue(leftV);
        return JSopDiv(newMv0, mv1, ptyp, op);
      }
      break;
    }
    default:
      assert(false && "unexpected mul op0");
  }
  MValue res;
  SetMValueValue(res, resVal);
  SetMValueTag(res, resTag);
  res.ptyp = ptyp;
  return res;
}

MValue InterSource::JSopRem(MValue &mv0, MValue &mv1, PrimType ptyp, Opcode op) {
  __jstype resTag;
  uint32_t resVal;
  __jstype tag0 = (__jstype)GetMValueTag(mv0);
  __jstype tag1 = (__jstype)GetMValueTag(mv1);
  if (tag0 == JSTYPE_NAN || tag1 == JSTYPE_NAN
    || tag0 == JSTYPE_UNDEFINED || tag1 == JSTYPE_UNDEFINED || tag1 == JSTYPE_NULL) {
    return JsvalToMValue(__nan_value());
  }
  switch (tag0) {
    case JSTYPE_STRING: {

      bool isNum;
      __jsvalue jsval = MValueToJsval(mv0);
      int32_t x0 = __js_str2num2(__jsval_to_string(&jsval), isNum, true);
      if (!isNum) {
        return JsvalToMValue(__nan_value());
      }

      switch (tag1) {
        case JSTYPE_STRING: {
          jsval = MValueToJsval(mv1);
          int32_t x1 = __js_str2num2(__jsval_to_string(&jsval), isNum, true);
          if (!isNum) {
            return JsvalToMValue(__nan_value());
          }
          resTag = JSTYPE_NUMBER;
          resVal = x0 % x1;
          break;
        }
        case JSTYPE_BOOLEAN: {
          int32_t x1 = mv1.x.i32;
          if (x1 == 0) {
            return JsvalToMValue(x0 >= 0 ? __number_infinity() : __number_neg_infinity());
          }
          resTag = JSTYPE_NUMBER;
          resVal = x0 % mv1.x.i32;
          break;
        }
        case JSTYPE_NUMBER: {
          int32_t x1 = mv1.x.i32;
          if (x1 == 0) {
            return JsvalToMValue(x0 >= 0 ? __number_infinity() : __number_neg_infinity());
          }
          resTag = JSTYPE_NUMBER;
          resVal = x0 % x1;
          break;
        }
        case JSTYPE_DOUBLE: {
          resTag = JSTYPE_DOUBLE;
          double x1 = memory_manager->GetF64FromU32(mv1.x.u32);
          if (fabs(x1) < NumberMinValue) {
            return JsvalToMValue(x0 < 0 ? __number_infinity() : __number_neg_infinity());
          }
          //double rtx = (double)mv0.x.i32 % x1;
          double rtx = fmod((double)x0,x1);
          if (std::isinf(rtx)) {
            resTag = JSTYPE_INFINITY;
            resVal = (rtx == (-1.0/0.0) ? 1 : 0);  // ???
          } else {
            resVal = memory_manager->SetF64ToU32(rtx);
          }
          break;
        }
        case JSTYPE_OBJECT: {
          __jsvalue v0 = MValueToJsval(mv0);
          __jsvalue v1 = MValueToJsval(mv1);
          return JsvalToMValue(__jsop_object_rem(&v0, &v1));
        }
        case JSTYPE_NULL: {
          return JsvalToMValue(x0 > 0 ? __number_infinity() : __number_neg_infinity());
        }
        case JSTYPE_INFINITY: {
          __jsvalue v1 = MValueToJsval(mv1);
          bool isPos = __is_neg_infinity(&v1) ? x0 < 0 : x0 >= 0;
          return JsvalToMValue(isPos ? __number_value(0) :  __double_value(0));
        }
        default:
          assert(false && "unexpected rem op1");
      }
      break;
    }

    case JSTYPE_BOOLEAN: {
      int32_t x0 = mv0.x.i32;
      switch (tag1) {
        case JSTYPE_BOOLEAN: {
          int32_t x1 = mv1.x.i32;
          if (x1 == 0) {
            return JsvalToMValue(x0 >= 0 ? __number_infinity() : __number_neg_infinity());
          }
          resTag = JSTYPE_NUMBER;
          resVal = 0;
          break;
        }
        case JSTYPE_NUMBER: {
          int32_t x1 = mv1.x.i32;
          if (x1 == 0) {
            return JsvalToMValue(x0 >= 0 ? __number_infinity() : __number_neg_infinity());
          }
          resTag = JSTYPE_NUMBER;
          resVal = x0 % x1;
          break;
        }
        case JSTYPE_OBJECT: {
          __jsvalue v0 = MValueToJsval(mv0);
          __jsvalue v1 = MValueToJsval(mv1);
          return JsvalToMValue(__jsop_object_rem(&v0, &v1));
        }
        case JSTYPE_NULL: {
          return JsvalToMValue(__number_infinity());
        }
        case JSTYPE_UNDEFINED: {
          return JsvalToMValue(__nan_value());
        }
        default:
          assert(false && "unexpected rem op1");
      }
      break;
    }
    case JSTYPE_NUMBER: {
      int32_t x0 = mv0.x.i32;
      switch (tag1) {
        case JSTYPE_BOOLEAN: {
          int32_t x1 = mv1.x.i32;
          if (x1 == 0) {
            return JsvalToMValue(x0 >= 0 ? __number_infinity() : __number_neg_infinity());
          }
          resTag = JSTYPE_NUMBER;
          resVal = mv0.x.i32 % mv1.x.i32;
          break;
        }
        case JSTYPE_NUMBER: {
          int32_t x1 = mv1.x.i32;
          if (x1 == 0) {
            return JsvalToMValue(x0 >= 0 ? __number_infinity() : __number_neg_infinity());
          }
          resTag = JSTYPE_NUMBER;
          resVal = x0 % x1;
          break;
        }
        case JSTYPE_DOUBLE: {
          resTag = JSTYPE_DOUBLE;
          double x1 = memory_manager->GetF64FromU32(mv1.x.u32);
          if (fabs(x1) < NumberMinValue) {
            return JsvalToMValue(x0 < 0 ? __number_infinity() : __number_neg_infinity());
          }
          //double rtx = (double)mv0.x.i32 % x1;
          double rtx = fmod((double)mv0.x.i32,x1);
          if (std::isinf(rtx)) {
            resTag = JSTYPE_INFINITY;
            resVal = (rtx == (-1.0/0.0) ? 1 : 0);  // ???
          } else {
            resVal = memory_manager->SetF64ToU32(rtx);
          }
          break;
        }
        case JSTYPE_OBJECT: {
          __jsvalue v0 = MValueToJsval(mv0);
          __jsvalue v1 = MValueToJsval(mv1);
          return JsvalToMValue(__jsop_object_rem(&v0, &v1));
        }
        case JSTYPE_NULL: {
          return JsvalToMValue(mv0.x.i32 > 0 ? __number_infinity() : __number_neg_infinity());
        }
        case JSTYPE_INFINITY: {
          __jsvalue v1 = MValueToJsval(mv1);
          bool isPos = __is_neg_infinity(&v1) ? x0 < 0 : x0 >= 0;
          return JsvalToMValue(isPos ? __number_value(0) :  __double_value(0));
        }
        default:
          assert(false && "unexpected rem op1");
      }
      break;
    }
    case JSTYPE_DOUBLE: {
      double ldb = memory_manager->GetF64FromU32(mv0.x.u32);
      switch (tag1) {
        case JSTYPE_NUMBER:
        case JSTYPE_DOUBLE: {
          resTag = JSTYPE_DOUBLE;
          double rhs = (tag1 == JSTYPE_NUMBER ? (double)mv1.x.i32 : memory_manager->GetF64FromU32(mv1.x.u32));
          if (fabs(rhs) < NumberMinValue) {
            return JsvalToMValue(ldb >= 0 ? __number_infinity() : __number_neg_infinity());
          }
          //double rtx = ldb % rhs;
          double rtx = fmod(ldb, rhs);
          if (std::isinf(rtx)) {
            resTag = JSTYPE_INFINITY;
            resVal = (rtx == (-1.0/0.0) ? 1 : 0);  // ???
          } else {
            resVal = memory_manager->SetF64ToU32(rtx);
          }
          break;
        }
        case JSTYPE_OBJECT: {
          __jsvalue v0 = MValueToJsval(mv0);
          __jsvalue v1 = MValueToJsval(mv1);
          return JsvalToMValue(__jsop_object_rem(&v0, &v1));
        }
        case JSTYPE_NULL: {
          double rhs = memory_manager->GetF64FromU32(mv0.x.u32);
          return JsvalToMValue(rhs > 0 ? __number_infinity() : __number_neg_infinity());
        }
        case JSTYPE_INFINITY: {
          return JsvalToMValue(__number_value(0));
        }
        default:
          assert(false && "unexpected rem op1");
      }
      break;
    }
    case JSTYPE_OBJECT: {
      __jsvalue v0 = MValueToJsval(mv0);
      __jsvalue v1 = MValueToJsval(mv1);
      return JsvalToMValue(__jsop_object_rem(&v0, &v1));
    }
    case JSTYPE_NULL: {
      resTag = JSTYPE_NUMBER;
      resVal = 0;
      break;
    }
    case JSTYPE_INFINITY: {
      bool isPos0 = mv0.x.i32 == 0;
      bool isPos1 = false;
      switch (tag1) {
        case JSTYPE_NUMBER: {
          int32_t val1 = mv1.x.i32;
          if (val1 == 0) {
            return mv0;
          } else {
           isPos1 = val1 > 0;
          }
          break;
        }
        case JSTYPE_DOUBLE: {
          double rhs = memory_manager->GetF64FromU32(mv1.x.u32);
          if (fabs(rhs) < NumberMinValue) {
            return JsvalToMValue(isPos0 ? __number_neg_infinity() : __number_infinity());
          } else {
            isPos1 = rhs > 0;
          }
          break;
        }
        case JSTYPE_OBJECT: {
          __jsvalue v0 = MValueToJsval(mv0);
          __jsvalue v1 = MValueToJsval(mv1);
          return JsvalToMValue(__jsop_object_rem(&v0, &v1));
        }
        case JSTYPE_NULL: {
          assert(false && "unexpected rem op1");
          resTag = JSTYPE_NUMBER;
          resVal = 0;
          break;
        }
        case JSTYPE_INFINITY: {
          return JsvalToMValue(__nan_value());
        }
        default:
          assert(false && "unexpected rem op1");
      }
      return (isPos0 == isPos1) ? JsvalToMValue(__number_infinity()) : JsvalToMValue(__number_neg_infinity());
    }
    case JSTYPE_UNDEFINED: {
      return JsvalToMValue(__nan_value());
    }
    default:
      assert(false && "unexpected rem op0");
  }
  MValue res;
  SetMValueValue(res, resVal);
  SetMValueTag(res, resTag);
  res.ptyp = ptyp;
  return res;
}

MValue InterSource::JSopBitOp(MValue &mv0, MValue &mv1, PrimType ptyp, Opcode op) {
  __jstype tag0 = (__jstype)GetMValueTag(mv0);
  __jstype tag1 = (__jstype)GetMValueTag(mv1);
  MValue res;
  res.ptyp = PTY_dyni32;
  int32_t lVal;
  int32_t rVal;
  if (tag0 == JSTYPE_NAN || tag0 == JSTYPE_NULL
    || tag0 == JSTYPE_UNDEFINED) {
    lVal = 0;
  } else {
    __jsvalue jsv0 = MValueToJsval(mv0);
    lVal = __js_ToNumber(&jsv0);
  }

  if (tag1 == JSTYPE_NAN || tag1 == JSTYPE_NULL
    || tag1 == JSTYPE_UNDEFINED) {
    rVal = 0;
  } else {
    __jsvalue jsv1 = MValueToJsval(mv1);
    rVal = __js_ToNumber(&jsv1);
  }
  int32_t resVal = 0;
  switch (op) {
    case OP_ashr: {
      resVal = lVal >> rVal;
      break;
    }
    case OP_band: {
      resVal = lVal & rVal;
      break;
    }
    case OP_bior: {
      resVal = lVal | rVal;
      break;
    }
    case OP_bxor: {
      resVal = lVal ^ rVal;
      break;
    }
    case OP_lshr: {
      uint32_t ulVal = lVal;
      uint32_t urVal = rVal;
      resVal = ulVal >> urVal;
      break;
    }
    case OP_shl: {
      resVal = lVal << rVal;
      break;
    }
    case OP_land: {
      resVal = lVal && rVal;
      break;
    }
    case OP_lior: {
      resVal = lVal || rVal;
      break;
    }
    default:
      MIR_FATAL("unknown arithmetic op");
  }
  res = JsvalToMValue(__number_value(resVal));
  return res;
}


MValue InterSource::JSopSub(MValue &mv0, MValue &mv1, PrimType ptyp, Opcode op) {
  __jstype resTag;
  uint32_t resVal;
  __jstype tag0 = (__jstype)GetMValueTag(mv0);
  __jstype tag1 = (__jstype)GetMValueTag(mv1);
  switch (tag0) {
    case JSTYPE_BOOLEAN: {
      switch (tag1) {
        case JSTYPE_BOOLEAN: {
          resTag = JSTYPE_NUMBER;
          resVal = mv0.x.i32 - mv1.x.i32;
          break;
        }
        case JSTYPE_NUMBER: {
          resTag = JSTYPE_NUMBER;
          resVal = mv0.x.i32 - mv1.x.i32;
          break;
        }
        case JSTYPE_OBJECT: {
          __jsvalue v0 = MValueToJsval(mv0);
          __jsvalue v1 = MValueToJsval(mv1);
          return JsvalToMValue(__jsop_object_sub(&v0, &v1));
        }
        case JSTYPE_NULL: {
          resTag = JSTYPE_NUMBER;
          resVal = mv0.x.i32;
          break;
        }
        case JSTYPE_UNDEFINED: {
          return JsvalToMValue(__nan_value());
        }
        case JSTYPE_NAN: {
          return JsvalToMValue(__nan_value());
        }
        default:
          assert(false && "unexpected mul op1");
      }
      break;
    }
    case JSTYPE_NUMBER: {
      switch (tag1) {
        case JSTYPE_BOOLEAN: {
          resTag = JSTYPE_NUMBER;
          resVal = mv0.x.i32 - mv1.x.i32;
          break;
        }
        case JSTYPE_NUMBER: {
          resTag = JSTYPE_NUMBER;
          resVal = mv0.x.i32 - mv1.x.i32;
          break;
        }
        case JSTYPE_DOUBLE: {
          resTag = JSTYPE_DOUBLE;
          double rtx = (double)mv0.x.i32 - memory_manager->GetF64FromU32(mv1.x.u32);
          if (std::isinf(rtx)) {
            resTag = JSTYPE_INFINITY;
            resVal = (rtx == (-1.0/0.0) ? 1 : 0);
          } else {
            resVal = memory_manager->SetF64ToU32(rtx);
          }
          break;
        }
        case JSTYPE_OBJECT: {
          __jsvalue v0 = MValueToJsval(mv0);
          __jsvalue v1 = MValueToJsval(mv1);
          return JsvalToMValue(__jsop_object_sub(&v0, &v1));
        }
        case JSTYPE_NULL: {
          resTag = JSTYPE_NUMBER;
          resVal = mv0.x.i32;
          break;
        }
        case JSTYPE_UNDEFINED: {
          return JsvalToMValue(__nan_value());
        }
        case JSTYPE_NAN: {
          return JsvalToMValue(__nan_value());
        }
        case JSTYPE_INFINITY: {
          resTag = JSTYPE_INFINITY;
          resVal = (mv1.x.i32 == 0 ? 1 : 0);
          break;
        }
        default:
          assert(false && "unexpected mul op1");
      }
      break;
    }
    case JSTYPE_DOUBLE: {
      switch (tag1) {
        case JSTYPE_NUMBER:
        case JSTYPE_DOUBLE: {
          resTag = JSTYPE_DOUBLE;
          double rhs = (tag1 == JSTYPE_NUMBER ? (double)mv1.x.i32 : memory_manager->GetF64FromU32(mv1.x.u32));
          double rtx = memory_manager->GetF64FromU32(mv0.x.u32) - rhs;
          if (std::isinf(rtx)) {
            resTag = JSTYPE_INFINITY;
            resVal = (rtx == (-1.0/0.0) ? 1 : 0);
          } else {
            if (fabs(rtx) < NumberMinValue) {
              resTag = JSTYPE_NUMBER;
              resVal = 0;
            } else {
              resVal = memory_manager->SetF64ToU32(rtx);
            }
          }
          break;
        }
        case JSTYPE_OBJECT: {
          __jsvalue v0 = MValueToJsval(mv0);
          __jsvalue v1 = MValueToJsval(mv1);
          return JsvalToMValue(__jsop_object_sub(&v0, &v1));
        }
        case JSTYPE_NULL: {
          resTag = JSTYPE_DOUBLE;
          resVal = mv0.x.u32;
          break;
        }
        case JSTYPE_UNDEFINED: {
          return JsvalToMValue(__nan_value());
        }
        case JSTYPE_NAN: {
          return JsvalToMValue(__nan_value());
        }
        case JSTYPE_INFINITY: {
          resTag = JSTYPE_INFINITY;
          resVal = (mv1.x.i32 == 0 ? 1 : 0);
          break;
        }
        default:
          assert(false && "unexpected mul op1");
      }
      break;
    }
    case JSTYPE_OBJECT: {
      if (tag1 == JSTYPE_NAN) {
        return JsvalToMValue(__nan_value());
      }
      __jsvalue v0 = MValueToJsval(mv0);
      __jsvalue v1 = MValueToJsval(mv1);
      return JsvalToMValue(__jsop_object_sub(&v0, &v1));
    }
    case JSTYPE_NULL: {
      resTag = JSTYPE_NUMBER;
      resVal = 0;
      switch (tag1) {
        case JSTYPE_BOOLEAN: {
          resTag = JSTYPE_NUMBER;
          resVal = -mv1.x.i32;
          break;
        }
        case JSTYPE_NUMBER:
        case JSTYPE_DOUBLE: {
          resTag = JSTYPE_DOUBLE;
          double rhs = (tag1 == JSTYPE_NUMBER ? (double)mv1.x.i32 : memory_manager->GetF64FromU32(mv1.x.u32));
          resVal = memory_manager->SetF64ToU32(-rhs);
          break;
        }
        case JSTYPE_OBJECT: {
          __jsvalue v1 = MValueToJsval(mv1);
          __jsvalue v0 = __number_value(0);
          return JsvalToMValue(__jsop_object_sub(&v0, &v1));
        }
        case JSTYPE_NULL: {
          break;
        }
        case JSTYPE_UNDEFINED: {
          return JsvalToMValue(__nan_value());
        }
        case JSTYPE_NAN: {
          return JsvalToMValue(__nan_value());
        }
        case JSTYPE_INFINITY: {
          resTag = JSTYPE_INFINITY;
          resVal = (mv1.x.i32 == 0 ? 1 : 0);
          break;
        }
        default:
          assert(false && "unexpected sub op1");
      }
      break;
    }
    case JSTYPE_UNDEFINED: {
      return JsvalToMValue(__nan_value());
    }
    case JSTYPE_INFINITY: {
      if (tag1 == JSTYPE_INFINITY) {
        bool isPos0 = mv0.x.i32 == 0;
        bool isPos1 = mv1.x.i32 == 0;
        if ((isPos0 && isPos1) || (!isPos0 && !isPos1)) {
          return JsvalToMValue(__nan_value());
        } else {
          return mv0;
        }
      } else if (tag1 == JSTYPE_NAN) {
        return JsvalToMValue(__nan_value());
      } else {
        return mv0;// infinity - something = infinity
      }
    }
    case JSTYPE_NAN: {
      return JsvalToMValue(__nan_value());
    }
    case JSTYPE_STRING: {
      bool isConvertible = false;
      __jsvalue v0 = MValueToJsval(mv0);
      __jsvalue leftV = __js_ToNumberSlow2(&v0, isConvertible);
      if (isConvertible) {
        MValue leftMv = JsvalToMValue(leftV);
        return JSopSub(leftMv, mv1, ptyp, op);
      } else {
        return JsvalToMValue(__nan_value());
      }
    }
    default:
      assert(false && "unexpected sub op0");
  }
  MValue res;
  SetMValueValue(res, resVal);
  SetMValueTag(res, resTag);
  res.ptyp = ptyp;
  return res;
}

MValue InterSource::JSopMul(MValue &mv0, MValue &mv1, PrimType ptyp, Opcode op) {
  __jstype resTag;
  uint32_t resVal;
  __jstype tag0 = (__jstype)GetMValueTag(mv0);
  __jstype tag1 = (__jstype)GetMValueTag(mv1);
  if (tag0 == JSTYPE_NAN || tag1 == JSTYPE_NAN ||
       tag0 == JSTYPE_UNDEFINED || tag1 == JSTYPE_UNDEFINED) {
    return JsvalToMValue(__nan_value());
  }
  switch (tag0) {
    case JSTYPE_BOOLEAN: {
      switch (tag1) {
        case JSTYPE_BOOLEAN: {
          resTag = JSTYPE_NUMBER;
          resVal = mv0.x.i32 * mv1.x.i32;
          break;
        }
        case JSTYPE_NUMBER: {
          resTag = JSTYPE_NUMBER;
          resVal = mv0.x.i32 * mv1.x.i32;
          break;
        }
        case JSTYPE_OBJECT: {
          __jsvalue v0 = MValueToJsval(mv0);
          __jsvalue v1 = MValueToJsval(mv1);
          return JsvalToMValue(__jsop_object_mul(&v0, &v1));
        }
        case JSTYPE_NULL: {
          resTag = JSTYPE_NUMBER;
          resVal = 0;
          break;
        }
        default:
          assert(false && "unexpected mul op1");
      }
      break;
    }
    case JSTYPE_NUMBER: {
      switch (tag1) {
        case JSTYPE_BOOLEAN: {
          resTag = JSTYPE_NUMBER;
          resVal = mv0.x.i32 * mv1.x.i32;
          break;
        }
        case JSTYPE_NUMBER: {
          double rtx = (double)mv0.x.i32 * (double)mv1.x.i32;
          if (fabs(rtx) < (double)0x7fffffff) {
            resTag = JSTYPE_NUMBER;
            resVal = mv0.x.i32 * mv1.x.i32;
          } else if (std::isinf(rtx)) {
            resTag = JSTYPE_INFINITY;
            resVal = (rtx == (-1.0/0.0) ? 1 : 0);
          } else {
            resTag = JSTYPE_DOUBLE;
            resVal = memory_manager->SetF64ToU32(rtx);
          }
          break;
        }
        case JSTYPE_DOUBLE: {
          resTag = JSTYPE_DOUBLE;
          double rtx = (double)mv0.x.i32 * memory_manager->GetF64FromU32(mv1.x.u32);
          if (std::isinf(rtx)) {
            resTag = JSTYPE_INFINITY;
            resVal = (rtx == (-1.0/0.0) ? 1 : 0);
          } else {
            resVal = memory_manager->SetF64ToU32(rtx);
          }
          break;
        }
        case JSTYPE_OBJECT: {
          __jsvalue v0 = MValueToJsval(mv0);
          __jsvalue v1 = MValueToJsval(mv1);
          return JsvalToMValue(__jsop_object_mul(&v0, &v1));
        }
        case JSTYPE_NULL: {
          resTag = JSTYPE_NUMBER;
          resVal = 0;
          break;
        }
        case JSTYPE_INFINITY: {
          return JSopMul(mv1, mv0, ptyp, op);
        }
        default:
          assert(false && "unexpected mul op1");
      }
      break;
    }
    case JSTYPE_DOUBLE: {
      switch (tag1) {
        case JSTYPE_NUMBER:
        case JSTYPE_DOUBLE: {
          resTag = JSTYPE_DOUBLE;
          double rhs = (tag1 == JSTYPE_NUMBER ? (double)mv1.x.i32 : memory_manager->GetF64FromU32(mv1.x.u32));
          double rtx = memory_manager->GetF64FromU32(mv0.x.u32) * rhs;
          if (std::isinf(rtx)) {
            resTag = JSTYPE_INFINITY;
            resVal = (rtx == (-1.0/0.0) ? 1 : 0);
          } else {
            resVal = memory_manager->SetF64ToU32(rtx);
          }
          break;
        }
        case JSTYPE_OBJECT: {
          __jsvalue v0 = MValueToJsval(mv0);
          __jsvalue v1 = MValueToJsval(mv1);
          return JsvalToMValue(__jsop_object_mul(&v0, &v1));
        }
        case JSTYPE_NULL: {
          resTag = JSTYPE_NUMBER;
          resVal = 0;
          break;
        }
        case JSTYPE_INFINITY: {
          return JSopMul(mv1, mv0, ptyp, op);
        }
        default:
          assert(false && "unexpected mul op1");
      }
      break;
    }
    case JSTYPE_OBJECT: {
      __jsvalue v0 = MValueToJsval(mv0);
      __jsvalue v1 = MValueToJsval(mv1);
      return JsvalToMValue(__jsop_object_mul(&v0, &v1));
    }
    case JSTYPE_NULL: {
      resTag = JSTYPE_NUMBER;
      resVal = 0;
      break;
    }
    case JSTYPE_INFINITY: {
      bool isPos0 = mv0.x.i32 == 0;
      bool isPos1 = false;
      switch (tag1) {
        case JSTYPE_INFINITY: {
          isPos1 = mv1.x.i32 == 0;
          break;
        }
        case JSTYPE_NUMBER: {
          if (mv1.x.i32 == 0)
            return JsvalToMValue(__nan_value());
          isPos1 = mv1.x.i32 > 0;
          break;
        }
        case JSTYPE_DOUBLE: {
          double x = memory_manager->GetF64FromU32(mv1.x.u32);
          if (fabs(x) < NumberMinValue)
            return JsvalToMValue(__nan_value());
          isPos1 = x > 0;
          break;
        }
        default:
          assert(false && "unexpected mul op0");
      }
      if ((isPos0 && isPos1) || (!isPos0 && !isPos1)) {
        return JsvalToMValue(__number_infinity());
      } else {
        return JsvalToMValue(__number_neg_infinity());
      }
    }
    case JSTYPE_STRING: {
      bool isConvertible = false;
      __jsvalue v0 = MValueToJsval(mv0);
      __jsvalue leftV = __js_ToNumberSlow2(&v0, isConvertible);
      if (isConvertible) {
        MValue leftMv = JsvalToMValue(leftV);
        return JSopMul(leftMv, mv1, ptyp, op);
      } else {
        return JsvalToMValue(__nan_value());
      }
    }
    default:
      assert(false && "unexpected mul op0");
  }
  MValue res;
  SetMValueValue(res, resVal);
  SetMValueTag(res, resTag);
  res.ptyp = ptyp;
  return res;
}

MValue InterSource::JSopArith(MValue &mv0, MValue &mv1, PrimType ptyp, Opcode op) {
  MValue retval;
  switch (ptyp) {
    case PTY_i8:
    case PTY_u8:
    case PTY_i16:
    case PTY_u16:
      MIR_FATAL("InterpreteArith: small integer types not supported");
      break;
    case PTY_i32: {
      int32 val;
      int32 v0 = mv0.x.i32;
      int32 v1 = mv1.x.i32;
      switch (op) {
        case OP_ashr:
          val = v0 >> v1;
          break;
        case OP_band:
          val = v0 & v1;
          break;
        case OP_bior:
          val = v0 | v1;
          break;
        case OP_bxor:
          val = v0 ^ v1;
          break;
        case OP_lshr:
          val = v0 >> v1;
          break;
        case OP_max:
          val = v0 > v1 ? v0 : v1;
          break;
        case OP_min:
          val = v0 < v1 ? v0 : v1;
          break;
        case OP_rem:
          val = v0 % v1;
          break;
        case OP_shl:
          val = v0 << v1;
          break;
        case OP_div:
          val = v0 / v1;
          break;
        case OP_sub:
          val = v0 - v1;
          break;
        case OP_mul:
          val = v0 * v1;
          break;
        default:
          MIR_FATAL("unknown i32 arithmetic operations");
      }
      retval.x.u64 = 0;
      retval.x.i32 = val;
      break;
    }
    case PTY_u32:
    case PTY_a32: {
      uint32 val;
      uint32 v0 = mv0.x.u32;
      uint32 v1 = mv1.x.u32;
      switch (op) {
        case OP_ashr:
          val = v0 >> v1;
          break;
        case OP_band:
          val = v0 & v1;
          break;
        case OP_bior:
          val = v0 | v1;
          break;
        case OP_bxor:
          val = v0 ^ v1;
          break;
        case OP_lshr:
          val = v0 >> v1;
          break;
        case OP_max:
          val = v0 > v1 ? v0 : v1;
          break;
        case OP_min:
          val = v0 < v1 ? v0 : v1;
          break;
        case OP_rem:
          val = v0 % v1;
          break;
        case OP_shl:
          val = v0 << v1;
          break;
        case OP_div:
          val = v0 / v1;
          break;
        case OP_sub:
          val = v0 - v1;
          break;
        case OP_mul:
          val = v0 * v1;
          break;
        default:
          MIR_FATAL("unknown u32/a32 arithmetic operations");
          val = 0;  // to suppress warning
      }
      retval.x.u64 = 0;  // clear the tag field
      retval.x.u32 = val;
      break;
    }
    case PTY_a64: {
      switch (op) {
        case OP_mul:
         retval.x.u64 = mv0.x.u64 * mv1.x.u64;
         break;
        default:
          MIR_FATAL("unknown a64 arithmetic operations");
      }
      break;
    }
    default:
      MIR_FATAL("unknown data types for arithmetic operations");
  }
  return retval;
}

void InterSource::JSPrint(MValue mv0) {
  __jsop_print_item(MValueToJsval(mv0));
}


bool InterSource::JSopPrimCmp(Opcode op, MValue &mv0, MValue &mv1, PrimType rpty0) {
  bool resBool;
  switch (rpty0) {
    case PTY_i8:
    case PTY_u8:
    case PTY_i16:
    case PTY_u16:
      MIR_FATAL("InterpreteCmp: small integer types not supported");
      break;
    case PTY_i32: {
      int32 val;
      int32 v0 = mv0.x.i32;
      int32 v1 = mv1.x.i32;
      switch(op) {
        case OP_eq: val = v0 == v1; break;
        case OP_ne: val = v0 != v1; break;
        case OP_ge: val = v0 >= v1; break;
        case OP_gt: val = v0 > v1; break;
        case OP_le: val = v0 <= v1; break;
        case OP_lt: val = v0 < v1; break;
        default: assert(false && "error");
      }
      resBool = val;
      break;
    }
    case PTY_u32: {
      uint32 val;
      uint32 v0 = mv0.x.u32;
      uint32 v1 = mv1.x.u32;
      switch(op) {
        case OP_eq: val = v0 == v1; break;
        case OP_ne: val = v0 != v1; break;
        case OP_ge: val = v0 >= v1; break;
        case OP_gt: val = v0 > v1; break;
        case OP_le: val = v0 <= v1; break;
        case OP_lt: val = v0 < v1; break;
        default: assert(false && "error");
      }
      resBool = val;
      break;
    }
    default: assert(false && "error");
  }
  return resBool;
}

MValue InterSource::JSopCmp(MValue mv0, MValue mv1, Opcode op) {
  PrimType rpty0 = mv0.ptyp;
  PrimType rpty1 = mv1.ptyp;
  if (IsPrimitiveDyn(rpty0) || IsPrimitiveDyn(rpty1)) {
    __jsvalue jsv0 = MValueToJsval(mv0);
    __jsvalue jsv1 = MValueToJsval(mv1);
    __jsvalue resvalue;
    // SetMValueTag(resvalue, JSTYPE_BOOLEAN);
    if (__is_nan(&jsv0) || __is_nan(&jsv1)) {
      resvalue.s.payload.u32 = op == OP_ne;;
    } else {
      bool isAlwaysFalse = false; // there are some weird comparison to be alwasy false for JS
      bool resCmp = false;
      switch (op) {
        case OP_eq: {
            resCmp = __js_AbstractEquality(&jsv0, &jsv1, isAlwaysFalse);
          }
          break;
        case OP_ne: {
            resCmp = !__js_AbstractEquality(&jsv0, &jsv1, isAlwaysFalse);
          }
          break;
        case OP_ge: {
            resCmp = !__js_AbstractRelationalComparison(&jsv0, &jsv1, isAlwaysFalse);
          }
          break;
        case OP_gt: {
            resCmp = __js_AbstractRelationalComparison(&jsv1, &jsv0, isAlwaysFalse, false);
          }
          break;
        case OP_le: {
            resCmp = !__js_AbstractRelationalComparison(&jsv1, &jsv0, isAlwaysFalse, false);
          }
          break;
        case OP_lt: {
            resCmp = __js_AbstractRelationalComparison(&jsv0, &jsv1, isAlwaysFalse);
          }
          break;
        default:
          MIR_FATAL("unknown comparison ops");
      }
      resvalue.s.payload.u32 = (isAlwaysFalse ? false : resCmp);
    }
    return JsvalToMValue(resvalue);
  }
  MValue retVal;
  assert(rpty0 == rpty1 && "illegal comparison");
  retVal.x.u64 = JSopPrimCmp(op, mv0, mv1, rpty0);
  return retVal;
}

MValue InterSource::JSopCVT(MValue mv, PrimType toPtyp, PrimType fromPtyp) {
  MValue retMv;
   if (IsPrimitiveDyn(toPtyp)) {
    __jstype tag = JSTYPE_UNDEFINED;
    uint32_t u32Val = 0;
    switch (fromPtyp) {
      case PTY_u1: {
        tag = JSTYPE_BOOLEAN;
        u32Val = mv.x.u32;
        break;
      }
      case PTY_a32:
      case PTY_i32: {
        tag = JSTYPE_NUMBER;
        u32Val = mv.x.u32;
        break;
      }
      case PTY_u32: {
        u32Val = mv.x.u32;
        if (u32Val <= INT32_MAX) {
          tag = JSTYPE_NUMBER;
        } else {
          tag = JSTYPE_DOUBLE;
          u32Val = memory_manager->SetF64ToU32(mv.x.u32);
        }
        break;
      }
      case PTY_simplestr: {
        tag = JSTYPE_STRING;
#ifdef MACHINE64
        u32Val = memory_manager->MapRealAddr((void *)mv.x.u64);
#else
        u32Val = mv.x.u32;
#endif
        break;
      }
      case PTY_simpleobj:
        tag = JSTYPE_OBJECT;
#ifdef MACHINE64
        u32Val = memory_manager->MapRealAddr((void *)mv.x.u64);
#else
        u32Val = mv.u32;
#endif
        break;
      default:
        MIR_FATAL("interpreteCvt: NYI");
    }
    SetMValueValue(retMv, u32Val);
    SetMValueTag(retMv, tag);
  } else if (IsPrimitiveDyn(fromPtyp)) {
    switch (toPtyp) {
      case PTY_u1: {
        __jsvalue jsv = MValueToJsval(mv);
        retMv.x.i32 = __js_ToBoolean(&jsv);
        break;
      }
      case PTY_a32:
      case PTY_u32:
      case PTY_i32: {
        __jsvalue jsv = MValueToJsval(mv);
        retMv.x.i32 = __js_ToInteger(&jsv);
        break;
      }
      case PTY_simpleobj: {
        assert(false&&"NYI");
        // retval.asptr = __js_ToObject(&jsv);
        break;
      }
      default:
        MIR_FATAL("interpreteCvt: NYI");
    }
  }
  return retMv;
}

MValue InterSource::JSopNew(MValue &size) {
  MValue mv;
  // SetMValueValue(mv, memory_manager->MapRealAddr(VMMallocGC(size.x.i32, MemHeadEnv)));
  // SetMValueTag(mv,JSTYPE_ENV);
  mv.x.a64 = (uint8_t *)VMMallocGC(size.x.i32, MemHeadEnv);
  // SetMValueTag(mv, JSTYPE_ENV);
  return mv;
}

void* InterSource::JSopNewArrLength(MValue &conVal) {
  __jsvalue v0 = MValueToJsval(conVal);
  return (void *) __js_new_arr_length(&v0);
}


void InterSource::JSopSetProp(MValue &mv0, MValue &mv1, MValue &mv2) {
  __jsvalue v0 = MValueToJsval(mv0);
  __jsvalue v1 = MValueToJsval(mv1);
  __jsvalue v2 = MValueToJsval(mv2);
  __jsop_setprop(&v0, &v1, &v2);
  // check if it's set prop for arguments
  DynMFunction *curFunc = GetCurFunc();
  if (!curFunc->is_strict() && __is_js_object(&v0)) {
    __jsobject *obj = __jsval_to_object(&v0);
    if (obj == (__jsobject *)curFunc->argumentsObj) {
      __jstype jstp = (__jstype) GetJSTag(mv1);
      if (jstp == JSTYPE_NUMBER) {
        uint32_t index = mv1.x.u32;
        uint32_t numArgs = curFunc->header->upFormalSize/sizeof(void *);
        if (index < numArgs - 1 && !curFunc->IsIndexDeleted(index)) {
          EmulateStoreGC((uint8 *)GetFPAddr() + (index + 1)*sizeof(void *), mv2, mv2.ptyp);
        }
      }
    }
  }
}

uint64_t InterSource::JSopNewIterator(MValue &mv0, MValue &mv1) {
  __jsvalue v0 = MValueToJsval(mv0);
  MValue retMv;
  retMv.x.a64 = (uint8_t *)__jsop_valueto_iterator(&v0, mv1.x.u32);
  retMv.ptyp = PTY_a64;
  return retMv.x.u64;
}

MValue InterSource::JSopNextIterator(MValue &mv0) {
  MValue retMv;
  retMv.x.u64 = __jsop_iterator_next((void *)mv0.x.a64).asbits;
  retMv.ptyp = PTY_dynany;
  return retMv;
}

uint32_t InterSource::JSopMoreIterator(MValue &mv0) {
  return (uint32_t)__jsop_more_iterator((void *)mv0.x.a64);
}

void InterSource::InsertProlog(uint16_t frameSize) {
  uint8 *addrs = (uint8 *)memory + sp;
  *(uint32_t *)(addrs - 4) = fp;
  memset(addrs - frameSize,0,frameSize - 4);
  fp = sp;
  sp -= frameSize;
}

uint64_t InterSource::JSopBinary(MIRIntrinsicID id, MValue &mv0, MValue &mv1) {
  __jsvalue v0 = MValueToJsval(mv0);
  __jsvalue v1 = MValueToJsval(mv1);
  uint64_t u64Ret = 0;
  switch (id) {
    case INTRN_JSOP_STRICTEQ:
      u64Ret = __jsop_stricteq(&v0, &v1);
      break;
    case INTRN_JSOP_STRICTNE:
      u64Ret = __jsop_strictne(&v0, &v1);
      break;
    case INTRN_JSOP_INSTANCEOF:
      u64Ret = __jsop_instanceof(&v0, &v1);
      break;
    case INTRN_JSOP_IN:
      u64Ret = __jsop_in(&v0, &v1);
      break;
    default:
      MIR_FATAL("unsupported binary operators");
  }
  return u64Ret;
}



MValue InterSource::JSopConcat(MValue &mv0, MValue &mv1) {
  // __jsstring *v0 = (__jstring *)memory_manager->GetRealAddr(mv0.x.u32);
  // __jsstring *v1 = (__jstring *)memory_manager->GetRealAddr(mv1.x.u32);
  __jsstring *v0 = (__jsstring *)mv0.x.a64;
  __jsstring *v1 = (__jsstring *)mv1.x.a64;
  MValue res;
  res.x.a64 = (uint8_t *)__jsstr_concat_2(v0, v1);
  return res;
}

MValue InterSource::JSopNewObj0() {
  MValue mv;
  mv.x.a64 = (uint8_t *) __js_new_obj_obj_0();
  return mv;
}

MValue InterSource::JSopNewObj1(MValue &mv0) {
  __jsvalue v0 = MValueToJsval(mv0);
  MValue mv;
  mv.x.a64 = (uint8_t *) __js_new_obj_obj_1(&v0);
  return mv;
}

void InterSource::JSopInitThisPropByName (MValue &mv0) {
  __jsvalue &v0 = __js_Global_ThisBinding;
  __jsstring *v1 = (__jsstring *) mv0.x.a64;
  __jsop_init_this_prop_by_name(&v0, v1);
}

void InterSource::JSopSetThisPropByName (MValue &mv1, MValue &mv2) {
  __jsvalue &v0 = __js_Global_ThisBinding;
  __jsstring *v1 = (__jsstring *) mv1.x.a64;
  __jsvalue v2 = MValueToJsval(mv2);
  if(GetCurFunc()->is_strict() && (__is_undefined(&__js_ThisBinding) || \
              __js_SameValue(&__js_Global_ThisBinding, &__js_ThisBinding)) && \
          __jsstr_throw_typeerror(v1)) {
    MAPLE_JS_TYPEERROR_EXCEPTION();
  }
  __jsop_set_this_prop_by_name(&v0, v1, &v2);
}

void InterSource::JSopSetPropByName (MValue &mv0, MValue &mv1, MValue &mv2, bool isStrict) {
  __jsvalue v0 = MValueToJsval(mv0);
  // __jsvalue v1 = MValueToJsval(mv1);
  __jsstring *v1 = (__jsstring *) mv1.x.a64;
  __jsvalue v2 = MValueToJsval(mv2);
  if (v0.asbits == __js_Global_ThisBinding.asbits) {
    __jsop_set_this_prop_by_name(&v0, v1, &v2);
  } else {
    __jsop_setprop_by_name(&v0, v1, &v2, isStrict);
  }
}


MValue InterSource::JSopGetProp (MValue &mv0, MValue &mv1) {
  __jsvalue v0 = MValueToJsval(mv0);
  __jsvalue v1 = MValueToJsval(mv1);
  return JsvalToMValue(__jsop_getprop(&v0, &v1));
}

MValue InterSource::JSopGetThisPropByName(MValue &mv1) {
  __jsvalue &v0 = __js_Global_ThisBinding;
  __jsstring *v1 = (__jsstring *) mv1.x.a64;
  return JsvalToMValue(__jsop_get_this_prop_by_name(&v0, v1));
}

MValue InterSource::JSopGetPropByName(MValue &mv0, MValue &mv1) {
  __jsvalue v0 = MValueToJsval(mv0);
  __jsstring *v1 = (__jsstring *) mv1.x.a64;
  return JsvalToMValue(__jsop_getprop_by_name(&v0, v1));
}


MValue InterSource::JSopDelProp (MValue &mv0, MValue &mv1, bool throw_p) {
  __jsvalue v0 = MValueToJsval(mv0);
  __jsvalue v1 = MValueToJsval(mv1);
  MValue retMv = JsvalToMValue(__jsop_delprop(&v0, &v1, throw_p));
  DynMFunction *curFunc = GetCurFunc();
  if (!curFunc->is_strict() && __is_js_object(&v0)) {
    __jsobject *obj = __jsval_to_object(&v0);
    if (obj == (__jsobject *)curFunc->argumentsObj) {
      __jstype jstp = (__jstype) GetJSTag(mv1);
      if (jstp == JSTYPE_NUMBER) {
        uint32_t index = mv1.x.u32;
        curFunc->MarkArgumentsDeleted(index);
      }
    }
  }
  return retMv;
}

void InterSource::JSopInitPropByName(MValue &mv0, MValue &mv1, MValue &mv2) {
  __jsvalue v0 = MValueToJsval(mv0);
  __jsstring *v1 = (__jsstring *) mv1.x.a64;
  __jsvalue v2 = MValueToJsval(mv2);
  __jsop_initprop_by_name(&v0, v1, &v2);
}


void InterSource::JSopInitPropGetter(MValue &mv0, MValue &mv1, MValue &mv2) {
  __jsvalue v0 = MValueToJsval(mv0);
  __jsvalue v1 = MValueToJsval(mv1);
  __jsvalue v2 = MValueToJsval(mv2);
  __jsop_initprop_getter(&v0, &v1, &v2);
}

void InterSource::JSopInitPropSetter(MValue &mv0, MValue &mv1, MValue &mv2) {
  __jsvalue v0 = MValueToJsval(mv0);
  __jsvalue v1 = MValueToJsval(mv1);
  __jsvalue v2 = MValueToJsval(mv2);
  __jsop_initprop_setter(&v0, &v1, &v2);
}

MValue InterSource::JSopNewArrElems(MValue &mv0, MValue &mv1) {
  __jsvalue *v0 = (__jsvalue *)mv0.x.a64;
  uint32 v1 = mv1.x.u32;
  MValue mv;
  mv.x.a64 = (uint8_t *)__js_new_arr_elems(v0, v1);
  return mv;
}


MValue InterSource::JSopGetBuiltinString(MValue &mv) {
  MValue ret;
  ret.x.a64 = (uint8_t *) __jsstr_get_builtin((__jsbuiltin_string_id)mv.x.u32);
  return ret;
}

MValue InterSource::JSopGetBuiltinObject(MValue &mv) {
  MValue ret;
  ret.x.a64 = (uint8_t *) __jsobj_get_or_create_builtin((__jsbuiltin_object_id)mv.x.u32);
  return ret;
}

MValue InterSource::JSBoolean(MValue &mv) {
  __jsvalue v0 = MValueToJsval(mv);
  __jsvalue res;
  res.s.payload.boo = __js_ToBoolean(&v0);
  return JsvalToMValue(res);
}

MValue InterSource::JSNumber(MValue &mv) {
  __jsvalue v0 = MValueToJsval(mv);
  if (__is_undefined(&v0)) {
    return JsvalToMValue(__nan_value());
  }
  if (__is_double(&v0)) {
    return mv;
  }
  bool ok2Convert = false;
  __jsvalue i32v = __js_ToNumber2(&v0, ok2Convert);
  if (!ok2Convert) {
    return JsvalToMValue(__nan_value());
  } else {
    return JsvalToMValue(i32v);
  }
}

MValue InterSource::JSDate(MValue &mv) {
  //TODO: Handle return string correctly.
  char charray[64] = "";
  __jsstring *jsstr = __jsstr_new_from_char(charray);

  return JsvalToMValue(__string_value(jsstr));
}

MValue InterSource::JSRegExp(MValue &mv) {
  // Note that, different from other functions, mv contains JS string as simplestr instead of dynstr.
  __jsstring *jsstr = (__jsstring *)mv.x.u64;
  assert(jsstr && "shouldn't be null");

  return JsvalToMValue(__object_value(__js_ToRegExp(jsstr)));
}

MValue InterSource::JSString(MValue &mv) {
  __jsvalue v0 = MValueToJsval(mv);
  if (__is_none(&v0)) {
    if (mv.ptyp == PTY_dynany)
      v0 = __undefined_value();
  }
  MValue res;
  res.x.a64 = (uint8_t *)__js_ToString(&v0);
  return res;
}

MValue InterSource::JSopLength(MValue &mv) {
  __jsvalue v0 = MValueToJsval(mv);
  __jsobject *obj = __js_ToObject(&v0);
  return JsvalToMValue(__jsobj_helper_get_length_value(obj));
}

MValue InterSource::JSopThis() {
  return JsvalToMValue(__js_ThisBinding);
}

MValue InterSource::JSUnary(MIRIntrinsicID id, MValue &mval) {
  __jsvalue jsv = MValueToJsval(mval);
  __jsvalue res;
  switch (id) {
    case INTRN_JSOP_TYPEOF:
      res = __jsop_typeof(&jsv);
      break;
    default:
      MIR_FATAL("unsupported unary ops");
  }
  return JsvalToMValue(res);
}

bool InterSource::JSopSwitchCmp(MValue &mv0, MValue &mv1, MValue &mv2) {
  __jstype tag = (__jstype)GetJSTag(mv1);
  if (tag != JSTYPE_NUMBER)
    return false;
  Opcode cmpOp = (Opcode)mv0.x.u32;
  return JSopPrimCmp(cmpOp, mv1, mv2, PTY_i32);
}

MValue InterSource::JSopUnary(MValue &mval, Opcode op, PrimType pty) {
  int32 val = mval.x.i32;
  switch (op) {
    case OP_lnot: {
      switch (pty) {
        case PTY_u1:
          val = (val == 0);
          break;
        default:
          MIR_FATAL("InterpreteUnary: NYI");
      }
      break;
    }
    case OP_bnot: {
      switch (pty) {
        case PTY_i32:
        case PTY_u32:
        case PTY_a32:
          val = ~val;
          break;
        default:
          MIR_FATAL("InterpreteUnary: NYI");
      }
      break;
    }
    case OP_neg: {
      if (mval.ptyp == maple::PTY_simplestr || mval.ptyp == PTY_a64) {
        bool isConvertible = false;
        //__jsvalue v = MValueToJsval(mval);
        __jsstring *str = (__jsstring *)mval.x.a64;
        __jsvalue v = __string_value(str);
        __jsvalue Mval = __js_ToNumberSlow2(&v, isConvertible);
        if (__is_infinity((&Mval))) {
          if (__is_neg_infinity((&Mval)))
            return JsvalToMValue(__number_infinity());
          else
            return JsvalToMValue(__number_neg_infinity());
        }
        if (__is_nan(&Mval)) {
          return JsvalToMValue(__nan_value());
        }
        MValue mmval = JsvalToMValue(Mval);
        MValue v0 = JsvalToMValue(__number_value(0));
        return JSopSub(v0, mmval, pty, op);
      } else if (mval.ptyp == maple::PTY_simpleobj) {
        __jsobject *obj = (__jsobject *)(mval.x.a64);
        __jsvalue xval = __object_internal_DefaultValue(obj, JSTYPE_NUMBER);
        if (__is_string(&xval)) {
          MValue mmval;
          mmval.x.a64 = (uint8_t *)__jsval_to_string(&xval);
          mmval.ptyp = maple::PTY_simplestr;
          return JSopUnary(mmval, op, pty);
        } else {
          MValue mmval = JsvalToMValue(xval);
          MValue v0 = JsvalToMValue(__number_value(0));
          return JSopSub(v0, mmval, pty, op);
        }
      }
      switch (pty) {
        case PTY_i32:
        case PTY_u32:
        case PTY_a32:
          val = -val;
          break;
        case PTY_dynany: {
          __jsvalue jsv = MValueToJsval(mval);
          if (__is_number(&jsv)) {
            int32_t value = jsv.s.payload.i32;
            if (value == 0) {
              return JsvalToMValue(__double_value(-0));
            } else {
              return JsvalToMValue(__number_value(-value));
            }
          } else if (__is_double(&jsv)) {
            double dv = __jsval_to_double(&jsv);
            if (fabs(dv - 0.0f) < NumberMinValue) {
              return JsvalToMValue(__number_value(0));
            } else {
              return JsvalToMValue(__double_value(-dv));
            }
          }
          MValue v0 = JsvalToMValue(__number_value(0));
          return JSopSub(v0, mval, pty, op);
        }
        default:
          MIR_FATAL("InterpreteUnary: NYI");
      }
      break;
    }
    case OP_abs:
    case OP_extractbits:
    case OP_recip:
    case OP_sqrt:
    default:
      MIR_FATAL("InterpreteUnary: NYI");
  }
  MValue res;
  res.x.i32 = val;
  return res;
}

MValue InterSource::JSIsNan(MValue &mval) {
  __jstype tag = (__jstype)GetMValueTag(mval);
  uint32 resVal = 0;
  switch (tag) {
    case JSTYPE_STRING: {
      resVal = __jsstr_is_number((__jsstring *)memory_manager->GetRealAddr(mval.x.u32));
      break;
    }
    case JSTYPE_NAN: {
      resVal = 1;
      break;
    }
    default:
      break;
  }
  MValue resMv;
  SetMValueValue(resMv, resVal);
  SetMValueTag(resMv, JSTYPE_NUMBER);
  return resMv;
}

void InterSource::JsTry(void *tryPc, void *catchPc, void *finallyPc, DynMFunction *func) {
  JsEh *eh;
  if (EHstackReuseSize) {
    eh = EHstackReuse.Pop();
    EHstackReuseSize--;
  } else {
    eh = (JsEh *)VMMallocNOGC(sizeof(JsEh));
  }
  eh->dynFunc = func;
  eh->Init(tryPc, catchPc, finallyPc);
  EHstack.Push(eh);
  currEH = eh;
}

void* InterSource::CreateArgumentsObject(MValue *mvArgs, uint32_t passedNargs, MValue *calleeMv) {
  __jsobject *argumentsObj = __create_object();
  __jsobj_set_prototype(argumentsObj, JSBUILTIN_OBJECTPROTOTYPE);
  argumentsObj->object_class = JSARGUMENTS;
  argumentsObj->extensible = (uint8_t)true;
  argumentsObj->object_type = (uint8_t)JSREGULAR_OBJECT;
  uint32_t actualArgs = 0;
  for (int32 i = 0; i < passedNargs; i++) {
    __jsvalue elemVal = MValueToJsval(mvArgs[i]);
    if (__is_undefined(&elemVal)) {
      continue;
    }
    actualArgs++;
    // __jsobj_internal_Put(argumentsObj, i, &elemVal, false);
    __jsobj_helper_init_value_propertyByValue(argumentsObj, i, &elemVal, JSPROP_DESC_HAS_VWEC);
  }
  __jsvalue calleeJv = MValueToJsval(*calleeMv);
  __jsobj_helper_init_value_property(argumentsObj, JSBUILTIN_STRING_CALLEE, &calleeJv, JSPROP_DESC_HAS_VWUEC);
  __jsvalue lengthJv = __number_value(actualArgs);
  __jsobj_helper_init_value_property(argumentsObj, JSBUILTIN_STRING_LENGTH, &lengthJv, JSPROP_DESC_HAS_VWUEC);
  __jsvalue jsObj = __object_value(argumentsObj);
  __jsop_set_this_prop_by_name(&__js_Global_ThisBinding, __jsstr_get_builtin(JSBUILTIN_STRING_ARGUMENTS), &jsObj, true);
  GCIncRf(argumentsObj); // so far argumentsObj is supposed to be refered twice. #1 for global this, #2 for the DynMFunction
  return (void *)argumentsObj;
}

MValue InterSource::IntrinCall(MIRIntrinsicID id, MValue *args, int numArgs) {
  MValue mval0 = args[0];
  // 11.2.3 step 4: if args[0] is not object, throw type exception
  if (GetMValueTag(mval0) != (uint32)JSTYPE_OBJECT) {
    MAPLE_JS_TYPEERROR_EXCEPTION();
  }
  __jsobject *f = (__jsobject *)memory_manager->GetRealAddr(GetMvalueValue(mval0));;
  __jsfunction *func = (__jsfunction *)f->shared.fun;
  MValue retCall;
  retCall.x.u64 = 0;
  if (!func || f->object_class != JSFUNCTION) {
    // trying to call a null function, throw TypeError directly
    MAPLE_JS_TYPEERROR_EXCEPTION();
  }
  if (func->attrs & 0xff & JSFUNCPROP_NATIVE || id == INTRN_JSOP_NEW) {
    retCall = NativeFuncCall(id, args, numArgs);
    return retCall;
  }
  if (func->attrs & 0xff & JSFUNCPROP_BOUND) {
    retCall = BoundFuncCall(args, numArgs);
    return retCall;
  }
  void *callee = func->fp;
  int nargs = func->attrs >> 16 & 0xff;
  bool strictP = func->attrs & 0xff & JSFUNCPROP_STRICT;
  JsFileInforNode *oldFin = jsPlugin->formalFileInfo;
  bool contextSwitched = false;
  if (func->fileIndex != -1 && ((uint32_t)func->fileIndex != jsPlugin->formalFileInfo->fileIndex)) {
    JsFileInforNode *newFileInfo = jsPlugin->FindJsFile((uint32_t)func->fileIndex);
    SwitchPluginContext(newFileInfo);
    contextSwitched = true;
  }
  retCall = FuncCall(callee, true, func->env, args, numArgs, 2, nargs, strictP);
  if (contextSwitched)
    RestorePluginContext(oldFin);
  return retCall;
}

MValue InterSource::NativeFuncCall(MIRIntrinsicID id, MValue *args, int numArgs) {
  MValue mv0 = args[0];
  MValue mv1 = args[1];
  int argNum = numArgs - 2;
  __jsvalue funcNode = MValueToJsval(mv0);
  __jsvalue thisNode = MValueToJsval(mv1);
  __jsvalue jsArgs[MAXCALLARGNUM];
  for (int i = 0; i < argNum; i++) {
    jsArgs[i] = MValueToJsval(args[2 + i]);
  }
  // for __jsobj_defineProperty to arguments built-in, it will affects the actual parameters
  DynMFunction *curFunc = GetCurFunc();
  if (!curFunc->is_strict() && id != INTRN_JSOP_NEW && argNum == 3 && __js_IsCallable(&funcNode)) {
    // for Object.defineProperty (arguments, "0", {})
    // the number of args is 3
    __jsobject *fObj = __jsval_to_object(&funcNode);
    __jsfunction *func = fObj->shared.fun;
    if (func->fp == __jsobj_defineProperty) {
      __jsvalue arg0 = jsArgs[0];
      __jsvalue arg1 = jsArgs[1];
      if (__is_js_object(&arg0)) {
        __jsobject *obj = __jsval_to_object(&arg0);
        if (obj == (__jsobject *)curFunc->argumentsObj) {
          int32_t index = -1;
          if (__is_number(&arg1)) {
            index = __jsval_to_int32(&arg1);
          }else if (__is_string(&arg1)) {
            bool isNum;
            index = __js_str2num2(__jsval_to_string(&arg1), isNum, true);
            if (!isNum)
              index = -1;
          }
          if (index != -1 && !curFunc->IsIndexDeleted(index)) {
            assert(index >=0 && index <=32 && "extended the range");
            __jsvalue idxJs = __number_value(index);
            MValue mv2 = args[2 + 2];
            __jsvalue arg2 = jsArgs[2];
            __jsobj_defineProperty(&thisNode, &arg0, &idxJs, &arg2);
            __jsvalue idxValue = __jsobj_GetValueFromPropertyByValue(obj, index);
            if (!__is_undefined(&idxValue)) {
              EmulateStoreGC((uint8 *)GetFPAddr() + (index + 1)*sizeof(void *),
                              JsvalToMValue(idxValue), mv2.ptyp);
            }
           SetRetval0(JsvalToMValue(arg0), true);
           return JsvalToMValue(arg0);
          }
        }
      }
    }
  }
  __jsvalue res = (id == INTRN_JSOP_NEW) ? __jsop_new(&funcNode, &thisNode, &jsArgs[0], argNum) :
                        __jsop_call(&funcNode, &thisNode, &jsArgs[0], argNum);
  SetRetval0(JsvalToMValue(res), true);
  return JsvalToMValue(res);
}

MValue InterSource::BoundFuncCall(MValue *args, int numArgs) {
  MValue mv0 = args[0];
  int argNum = numArgs - 2;
  // __jsvalue jsArgs[MAXCALLARGNUM];
  __jsobject *f = (__jsobject *)memory_manager->GetRealAddr(GetMvalueValue(mv0));
  __jsfunction *func = (__jsfunction *)f->shared.fun;
  MIR_ASSERT(func->attrs & 0xff & JSFUNCPROP_BOUND);
  __jsvalue func_node = __object_value((__jsobject *)(func->fp));
  __jsvalue *bound_this = NULL;
  __jsvalue *bound_args = NULL;
  if (func->env) {
    bound_this = &((__jsvalue *)func->env)[0];
    bound_args = &((__jsvalue *)func->env)[1];
  } else {
    __jsvalue nullvalue = __undefined_value();
    bound_this = bound_args = &nullvalue;
  }
  int32_t bound_argnumber = (((func->attrs) >> 16) & 0xff) - 1;
  bound_argnumber = bound_argnumber >= 0 ? bound_argnumber : 0;
  __jsvalue jsArgs[MAXCALLARGNUM];
  MIR_ASSERT(argNum + bound_argnumber <= MAXCALLARGNUM);
  for (int32_t i = 0; i < bound_argnumber; i++) {
    jsArgs[i] = bound_args[i];
  }
  for (uint32 i = 0; i < argNum; i++) {
    jsArgs[i + bound_argnumber] = MValueToJsval(args[2 + i]);
  }
  MValue retCall = JsvalToMValue(__jsop_call(&func_node, bound_this, &jsArgs[0], bound_argnumber + argNum));
  SetRetval0(retCall, true);
  return retCall;
}

MValue InterSource::FuncCall(void *callee, bool isIntrinsiccall, void *env, MValue *args, int numArgs,
                int start, int nargs, bool strictP) {
  int32_t passedNargs = numArgs - start;
  MValue mvArgs[MAXCALLARGNUM];
  MIR_ASSERT(passedNargs <= MAXCALLARGNUM);
  for (int i = 0; i < passedNargs; i++) {
    mvArgs[i] = args[i + start];
  }
  MValue thisval = JsvalToMValue(__undefined_value());
  if (isIntrinsiccall) {
    thisval = args[1];
  }
  int32_t offset = PassArguments(thisval, env, mvArgs, passedNargs, nargs);
  sp += offset;
  __jsvalue this_arg = MValueToJsval(thisval);

  DynamicMethodHeaderT* calleeHeader = (DynamicMethodHeaderT *)((uint8_t *)callee + 4);
  __jsvalue old_this = __js_entry_function(&this_arg, (calleeHeader->attribute & FUNCATTRSTRICT) | strictP);
  __jsvalue oldArgs = __jsop_get_this_prop_by_name(&__js_Global_ThisBinding, __jsstr_get_builtin(JSBUILTIN_STRING_ARGUMENTS));
  // bool isVargs = (passedNargs > 0) && ((calleeHeader->upFormalSize/8 - 1) != passedNargs);
  // MValue ret = maple_invoke_dynamic_method(calleeHeader, !isVargs ? nullptr : CreateArgumentsObject(mvArgs, passedNargs));
  DynMFunction *oldDynFunc = GetCurFunc();
  MValue ret = maple_invoke_dynamic_method(calleeHeader, CreateArgumentsObject(mvArgs, passedNargs, &args[0]));
  if (oldArgs.asbits != 0) {
    __jsop_set_this_prop_by_name(&__js_Global_ThisBinding, __jsstr_get_builtin(JSBUILTIN_STRING_ARGUMENTS), &oldArgs, true);
  } else {
    __jsobj_internal_Delete(__jsval_to_object(&__js_Global_ThisBinding), __jsstr_get_builtin(JSBUILTIN_STRING_ARGUMENTS));
  }
  __js_exit_function(&this_arg, old_this, (calleeHeader->attribute & FUNCATTRSTRICT)|strictP);
  sp -= offset;
  SetCurFunc(oldDynFunc);
  return ret;
}

MValue InterSource::FuncCall_JS(__jsobject *fObject, __jsvalue *this_arg, void *env, __jsvalue *arg_list, int32_t nargs) {
  __jsfunction *func = fObject->shared.fun;
  void *callee = func->fp;
  if (!callee) {
    return JsvalToMValue(__undefined_value());
  }

  MValue mvArgList[MAXCALLARGNUM];
  for (int i = 0; i < nargs; i++) {
    mvArgList[i] = JsvalToMValue(arg_list[i]);
  }

  int32_t func_nargs = func->attrs >> 16 & 0xff;
  int32_t offset = PassArguments(JsvalToMValue(*this_arg), env, mvArgList, nargs, func_nargs);
  // Update sp_, set sp_ to sp_ + offset.
  sp += offset;
  __jsvalue oldArgs = __jsop_get_this_prop_by_name(&__js_Global_ThisBinding, __jsstr_get_builtin(JSBUILTIN_STRING_ARGUMENTS));
  DynMFunction *oldDynFunc = GetCurFunc();

  DynamicMethodHeaderT* calleeHeader = (DynamicMethodHeaderT*)((uint8_t *)callee + 4);
  // bool isVargs = (nargs > 0) && ((calleeHeader->upFormalSize/8 - 1) != nargs);
  // MValue ret = maple_invoke_dynamic_method(calleeHeader, isVargs ? nullptr : CreateArgumentsObject(mvArgList, nargs));
  MValue fObjMv = JsvalToMValue(__object_value(fObject));
  MValue ret = maple_invoke_dynamic_method(calleeHeader,  CreateArgumentsObject(mvArgList, nargs, &fObjMv));
  if (oldArgs.asbits != 0) {
    __jsop_set_this_prop_by_name(&__js_Global_ThisBinding, __jsstr_get_builtin(JSBUILTIN_STRING_ARGUMENTS), &oldArgs);
  } else {
    __jsobj_internal_Delete(__jsval_to_object(&__js_Global_ThisBinding), __jsstr_get_builtin(JSBUILTIN_STRING_ARGUMENTS));
  }
  // Restore sp_, set sp_ to sp_ - offset.
  sp -= offset;
  SetCurFunc(oldDynFunc);

  return ret;
}


void InterSource::IntrnError(MValue *args, int numArgs) {
  for (int i = 0; i < numArgs; i++)
    JSPrint(args[i]);
  exit(8);
}

MValue InterSource::IntrinCCall(MValue *args, int numArgs) {
  MValue mval0 = args[0];
  __jsvalue jval = MValueToJsval(mval0);
  const char *name = __jsval_to_string(&jval)->x.ascii;
  funcCallback funcptr = getCCallback(name);
  MValue &mval1 = args[1];
  uint32 argc = mval1.x.u32;
  uint32 argsSize = argc * sizeof(uint32);
  int *argsI = (int *)VMMallocNOGC(argsSize);
  for (uint32 i = 0; i < argc; i++) {
    argsI[i] = (int)args[2+i].x.i32;
  }

  // make ccall
  __jsvalue res;
  res.s.tag = JSTYPE_NUMBER;
  res.s.payload.i32 = funcptr(argsI);

  SetRetval0(JsvalToMValue(res), true /* dynamic type */ );
  VMFreeNOGC(argsI, argsSize);
  MValue retMval;
  retMval.x.u64 = 0;
  return retMval;
}

MValue InterSource::JSopRequire(MValue mv0) {
  __jsvalue v0 = MValueToJsval(mv0);
  __jsstring *str = __js_ToString(&v0);
  uint32_t length = __jsstr_get_length(str);
  char fileName[100];
  assert(length + 5 + 1 < 100 && "required file name tool long");
  for (uint32 i = 0; i < length; i++) {
    fileName[i] = (char)__jsstr_get_char(str, i);
  }
  memcpy(&fileName[length], ".so", sizeof(".so"));
  fileName[length + 3] = '\0';
  bool isCached = false;
  JsFileInforNode *jsFileInfo = jsPlugin->LoadRequiredFile(fileName, isCached);
  if (isCached) {
    __jsvalue objJsval = __object_value(__jsobj_get_or_create_builtin(JSBUILTIN_MODULE));
    __jsvalue retJsval = __jsop_getprop_by_name(&objJsval, __jsstr_get_builtin(JSBUILTIN_STRING_EXPORTS));
    SetRetval0(JsvalToMValue(retJsval), true);
    return retVal0;
  }
  JsFileInforNode *formalJsFileInfo = jsPlugin->formalFileInfo;
  if (formalJsFileInfo == jsFileInfo) {
   MIR_FATAL("plugin formalfile is the same with current file");
  }
  MIR_ASSERT(jsFileInfo);
  SwitchPluginContext(jsFileInfo);
  DynMFunction *oldDynFunc = GetCurFunc();
  // DynMFunction *entryFunc = jsFileInfo->GetEntryFunction();
  DynamicMethodHeaderT * header = (DynamicMethodHeaderT *)(jsFileInfo->mainFn);
  maple_invoke_dynamic_method(header, NULL);
  SetCurFunc(oldDynFunc);
  RestorePluginContext(formalJsFileInfo);
  // SetCurrFunc();
  // restore
  return retVal0;
}

uint64_t InterSource::GetIntFromJsstring(__jsstring* jsstr) {
  bool isConvertible = false;
  __jsvalue jsDv = __js_str2double(jsstr, isConvertible);
  if (!isConvertible)
    return 0;
  return (uint64_t)__jsval_to_double(&jsDv);
}

void InterSource::SwitchPluginContext(JsFileInforNode *jsFIN) {
  gp = jsFIN->gp;
  topGp = gp + jsFIN->glbMemsize;
  jsPlugin->formalFileInfo = jsFIN;
}

void InterSource::RestorePluginContext(JsFileInforNode *formaljsfileinfo) {
  jsPlugin->formalFileInfo = formaljsfileinfo;
  gp = formaljsfileinfo->gp;
  topGp = formaljsfileinfo->gp + formaljsfileinfo->glbMemsize;
}

void InterSource::JSdoubleConst(uint64_t u64Val, MValue &res) {
  union {
    uint64_t u64;
    double f64;
  }xx;
  xx.u64 = u64Val;
  MValue mv;
  SetMValueValue(res, memory_manager->SetF64ToU32(xx.f64));
  SetMValueTag(res, JSTYPE_DOUBLE);
}

void InterSource::InsertEplog() {
  sp = fp;
  fp = *(uint32_t *)((uint8_t *)memory + sp - 4);
}

void InterSource::UpdateArguments(int32_t index, MValue &mv) {
  if (!GetCurFunc()->IsIndexDeleted(index)) {
    __jsobject *obj = (__jsobject *)curDynFunction->argumentsObj;
    __jsvalue v0 = __object_value(obj);
    __jsvalue v1 = __number_value(index);
    __jsvalue v2 = MValueToJsval(mv);
    __jsop_setprop(&v0, &v1, &v2);
  }
}

void InterSource::CreateJsPlugin(char *fileName) {
  jsPlugin = (JsPlugin *)malloc(sizeof(JsPlugin));
  jsPlugin->mainFileInfo = (JsFileInforNode *)malloc(sizeof(JsFileInforNode));
  jsPlugin->fileIndex = 0;
  uint32 i = jsPlugin->GetIndexForSo(fileName);
  jsPlugin->mainFileInfo->Init(&fileName[i], jsPlugin->fileIndex, gp, topGp - gp, nullptr, nullptr);
  jsPlugin->formalFileInfo = jsPlugin->mainFileInfo;
}

MValue InterSource::JSopGetArgumentsObject(void *argumentsObject) {
  return JsvalToMValue(__object_value((__jsobject *)(argumentsObject)));
}

MValue InterSource::GetOrCreateBuiltinObj(__jsbuiltin_object_id id) {
  // __jsobject *obj = __jsobj_get_or_create_builtin(JSBUILTIN_REFERENCEERRORCONSTRUCTOR);
  // __jsobject *eObj = __js_new_obj_obj_0();
  // __jsobj_set_prototype(eObj, JSBUILTIN_REFERENCEERRORCONSTRUCTOR);
  // __jsobject *proto = __js_new_obj_obj_0();
  // __jsvalue proto_value = __object_value(proto);
  // __jsobj_helper_init_value_property(eObj, JSBUILTIN_STRING_PROTOTYPE, &proto_value, JSPROP_DESC_HAS_VWUEUC);
  __jsvalue refConVal = __object_value(__jsobj_get_or_create_builtin(id));
  __jsvalue jsVal = __jsop_new(&refConVal, nullptr, nullptr, 0);
  return JsvalToMValue(jsVal);
}

extern "C" int64_t EngineShimDynamic(int64_t firstArg, char *appPath) {
  if (!jsGlobal) {
    void *handle = dlopen(appPath, RTLD_LOCAL | RTLD_LAZY);
    if (!handle) {
      fprintf(stderr, "failed to open %s\n", appPath);
      return 1;
    }
    uint16_t *mpljsMdD = (uint16_t *)dlsym(handle, "__mpljs_module_decl__");
    if (!mpljsMdD) {
      fprintf(stderr, "failed to open __mpljs_module_decl__ %s\n", appPath);
      return 1;
    }
    uint16_t globalMemSize = mpljsMdD[0];
    gInterSource = new InterSource();
    gInterSource->gp = (uint8_t *)malloc(globalMemSize);
    memcpy(gInterSource->gp, mpljsMdD + 1, globalMemSize);
    gInterSource->topGp = gInterSource->gp + globalMemSize;
    gInterSource->CreateJsPlugin(appPath);
    jsGlobal = new JavaScriptGlobal();
    jsGlobal->flavor = 0;
    jsGlobal->srcLang = 2;
    jsGlobal->id = 0;
    jsGlobal->globalmemsize = globalMemSize;
    jsGlobal->globalwordstypetagged = *((uint8_t *)mpljsMdD + 2 + globalMemSize + 2);
    jsGlobal->globalwordsrefcounted = *((uint8_t *)mpljsMdD + 2 + globalMemSize + 6);
  }
  MValue val;
  uint8_t* addr = (uint8_t*)firstArg + 4; // skip signature
  DynamicMethodHeaderT *header = (DynamicMethodHeaderT *)(addr);
  assert(header->upFormalSize == 0 && "main function got a frame size?");
  addr += *(int32_t*)addr; // skip to 1st instruction
  val = maple_invoke_dynamic_method_main (addr, header);
  return val.x.i64;
}

} // namespace maple
