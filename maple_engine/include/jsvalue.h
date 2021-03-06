/*
 * Copyright (C) [2021] Futurewei Technologies, Inc. All rights reserved.
 *
 * OpenArkCompiler is licensed under the Mulan Permissive Software License v2.
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

#ifndef JSVALUE_H
#define JSVALUE_H
#include <cstdint>
#include <cstdbool>
#include <cassert>
#include <cmath>
#include "mir_config.h"

#if MIR_FEATURE_FULL || MIR_DEBUG
#include <cstdio>
#endif  // MIR_FEATURE_FULL

#ifdef DEBUG_MAPLE
// replaced by MIR_ASSERT for simplicity.
#define MAPLE_JS_ASSERT(expr) MIR_ASSERT((expr))
#define MAPLE_JS_EXCEPTION(expr) MIR_ASSERT((expr) && "Should throw a JS exception, issue #553")
#else
#define MAPLE_JS_ASSERT(expr) MIR_ASSERT((expr))
#define MAPLE_JS_EXCEPTION(expr) MIR_ASSERT((expr) && "Should throw a JS exception, issue #553")
#endif
/*
#define MAPLE_JS_TYPEERROR_EXCEPTION() \
 do {                                  \
  throw "TypeError";                   \
 } while(0);                           \
 */

inline void MAPLE_JS_TYPEERROR_EXCEPTION() {
  throw "TypeError";
}

inline void MAPLE_JS_SYNTAXERROR_EXCEPTION() {
  throw "SyntaxError";
}

inline void MAPLE_JS_RANGEERROR_EXCEPTION() {
  throw "RangeError";
}
inline void MAPLE_JS_URIERROR_EXCEPTION() {
  throw "UriError";
}
inline void MAPLE_JS_REFERENCEERROR_EXCEPTION() {
  throw "ReferenceError";
}

enum __jstype {
  JSTYPE_NONE = 0,
  JSTYPE_NULL,
  JSTYPE_BOOLEAN,
  JSTYPE_STRING,
  JSTYPE_NUMBER,
  JSTYPE_OBJECT,
  JSTYPE_ENV,
  JSTYPE_UNKNOWN,
  JSTYPE_UNDEFINED,
  JSTYPE_DOUBLE,
  JSTYPE_NAN,
  JSTYPE_INFINITY,
};

enum __jsbuiltin_object_id : uint8_t {  // must in accordance with js_value.h:js_builtin_id in the front-end (js2mpl/include/jsvalue.h)
  JSBUILTIN_GLOBALOBJECT = 0,
  JSBUILTIN_OBJECTCONSTRUCTOR,
  JSBUILTIN_OBJECTPROTOTYPE,
  JSBUILTIN_FUNCTIONCONSTRUCTOR,
  JSBUILTIN_FUNCTIONPROTOTYPE,
  JSBUILTIN_ARRAYCONSTRUCTOR,
  JSBUILTIN_ARRAYPROTOTYPE,
  JSBUILTIN_STRINGCONSTRUCTOR,
  JSBUILTIN_STRINGPROTOTYPE,
  JSBUILTIN_BOOLEANCONSTRUCTOR,
  JSBUILTIN_BOOLEANPROTOTYPE,
  JSBUILTIN_NUMBERCONSTRUCTOR,
  JSBUILTIN_NUMBERPROTOTYPE,
  JSBUILTIN_EXPORTS,
  JSBUILTIN_MODULE,
  JSBUILTIN_MATH,
  JSBUILTIN_JSON,
  JSBUILTIN_ERROR_CONSTRUCTOR,
  JSBUILTIN_ERROR_PROTOTYPE,
  JSBUILTIN_EVALERROR_CONSTRUCTOR,
  JSBUILTIN_EVALERROR_PROTOTYPE,
  JSBUILTIN_RANGEERROR_CONSTRUCTOR,
  JSBUILTIN_RANGEERROR_PROTOTYPE,
  JSBUILTIN_REFERENCEERRORCONSTRUCTOR,
  JSBUILTIN_REFERENCEERRORPROTOTYPE,
  JSBUILTIN_SYNTAXERROR_CONSTRUCTOR,
  JSBUILTIN_SYNTAXERROR_PROTOTYPE,
  JSBUILTIN_TYPEERROR_CONSTRUCTOR,
  JSBUILTIN_TYPEERROR_PROTOTYPE,
  JSBUILTIN_URIERROR_CONSTRUCTOR,
  JSBUILTIN_URIERROR_PROTOTYPE,
  JSBUILTIN_DATECONSTRUCTOR,
  JSBUILTIN_DATEPROTOTYPE,
  JSBUILTIN_ISNAN,
  JSBUILTIN_REGEXPCONSTRUCTOR,
  JSBUILTIN_REGEXPPROTOTYPE,
  JSBUILTIN_NAN,
  JSBUILTIN_INFINITY,
  JSBUILTIN_UNDEFINED,
  JSBUILTIN_PARSEINT_CONSTRUCTOR,
  JSBUILTIN_DECODEURI_CONSTRUCTOR,
  JSBUILTIN_DECODEURICOMPONENT_CONSTRUCTOR,
  JSBUILTIN_PARSEFLOAT_CONSTRUCTOR,
  JSBUILTIN_ISFINITE_CONSTRUCTOR,
  JSBUILTIN_ENCODEURI_CONSTRUCTOR,
  JSBUILTIN_ENCODEURICOMPONENT_CONSTRUCTOR,
  JSBUILTIN_EVAL_CONSTRUCTOR,
  JSBUILTIN_LAST_OBJECT,
};

typedef struct __jsobject __jsobject;
//typedef uint8_t __jsstring;

const double MathE = 2.718281828459045;
const double MathLn10 = 2.302585092994046;
const double MathLn2 = 0.6931471805599453;
const double MathLog10e = 0.4342944819032518;
const double MathLog2e = 1.4426950408889634;
const double MathPi = 3.141592653589793;
const double MathSqrt1_2 = 0.7071067811865476;
const double MathSqrt2 = 1.4142135623730951;
const double NumberMaxValue = 1.7976931348623157e+308;
const double NumberMinValue = 5e-324;

#define DOUBLE_ZERO_INDEX    0
#define MATH_E_INDEX    1
#define MATH_LN10_INDEX 2
#define MATH_LN2_INDEX  3
#define MATH_LOG10E_INDEX 4
#define MATH_LOG2E_INDEX 5
#define MATH_PI_INDEX 6
#define MATH_SQRT1_2_INDEX 7
#define MATH_SQRT2_INDEX 8
#define NUMBER_MAX_VALUE 9
#define NUMBER_MIN_VALUE 10
#define MATH_LAST_INDEX NUMBER_MIN_VALUE

#ifdef MACHINE64
union __jsvalue {
  uint64_t asbits;
  struct {
    union {
      int32_t i32;
      uint32_t u32;
      uint32_t boo;
      uint32_t str;
      uint32_t obj;
      uint32_t ptr;
    } payload;
    __jstype tag;
  } s;
};
#else
union __jsvalue {
  uint64_t asbits;
  struct {
    union {
      int32_t i32;
      uint32_t u32;
      uint32_t boo;
      __jsstring *str;
      __jsobject *obj;
      void *ptr;
    } payload;
    __jstype tag;
  } s;
};
#endif

void dumpJSValue(__jsvalue *jsval);
void dumpJSString(uint16_t *ptr);

#endif
