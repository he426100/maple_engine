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

#include "jsvalue.h"
#include "jsvalueinline.h"
#include "jsarray.h"
#include "jsobject.h"
#include "jsobjectinline.h"
#include "jsstring.h"
#include "jsfunction.h"

void dumpJSString(uint16_t *ptr) {
#if MIR_FEATURE_FULL | MIR_DEBUG
  uint32_t i = 0;
  while (ptr[i] != 0) {
    printf("%c", ptr[i++]);
  }
  printf("\n");
#endif  // MIR_FEATURE_FULL
}

void dumpJSValue(__jsvalue *jsval) {
#if MIR_FEATURE_FULL | MIR_DEBUG
  printf("tag is %x, value is %x\n", jsval->s.tag, jsval->s.payload.i32);
  if (__is_string(jsval)) {
    dumpJSString((uint16_t *) memory_manager->GetRealAddr(jsval->s.payload.ptr));
  }
#endif  // MIR_FEATURE_FULL
}

uint32_t __jsop_length(__jsvalue *v) {
  __jsobject *obj = __js_ToObject(v);
  uint32_t length = __jsobj_helper_get_length(obj);
  if (!__is_js_object(v)) {
    memory_manager->ManageObject(obj, RECALL);
  }
  return length;
}

#ifdef MACHINE64
__jsstring *__jsval_to_string(__jsvalue *data) {
  MAPLE_JS_ASSERT(__is_string(data));
  return (__jsstring *)memory_manager->GetRealAddr(data->s.payload.str);
}

__jsobject *__jsval_to_object(__jsvalue *data) {
  MAPLE_JS_ASSERT(__is_js_object(data));
  return (__jsobject *)memory_manager->GetRealAddr(data->s.payload.obj);
}
void __set_string(__jsvalue *data, __jsstring *str) {
  data->s.tag = JSTYPE_STRING;
  data->s.payload.str = memory_manager->MapRealAddr(str);
}
void __set_object(__jsvalue *data, __jsobject *obj) {
  data->s.tag = JSTYPE_OBJECT;
  data->s.payload.obj = memory_manager->MapRealAddr(obj);
}
bool __is_js_function(__jsvalue *data) {
  if (__is_js_object(data)) {
    __jsobject *obj = (__jsobject *)(memory_manager->GetRealAddr(data->s.payload.obj));
    return obj->object_class == JSFUNCTION;
  }
  return false;
}
bool __is_js_array(__jsvalue *data) {
  if (__is_js_object(data)) {
    __jsobject *obj = (__jsobject *)(memory_manager->GetRealAddr(data->s.payload.obj));
    return obj->object_class == JSARRAY;
  }
  return false;
}
double __jsval_to_double(__jsvalue *data) {
  if (__is_number(data))
    return (double) __jsval_to_number(data);
  MAPLE_JS_ASSERT(__is_double(data));
  return memory_manager->GetF64FromU32(data->s.payload.u32);
}

__jsvalue __double_value(double x) {
  __jsvalue jsval;
  jsval.s.tag = JSTYPE_DOUBLE;
  jsval.s.payload.u32 = memory_manager->SetF64ToU32(x);
  return jsval;
}
#endif

