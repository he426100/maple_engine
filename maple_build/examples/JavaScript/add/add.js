//
// Copyright (C) [2020-2021] Futurewei Technologies, Inc. All rights reserved.
//
// OpenArkCompiler is licensed underthe Mulan Permissive Software License v2.
// You can use this software according to the terms and conditions of the MulanPSL - 2.0.
// You may obtain a copy of MulanPSL - 2.0 at:
//
//   https://opensource.org/licenses/MulanPSL-2.0
//
// THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY OR
// FIT FOR A PARTICULAR PURPOSE.
// See the MulanPSL - 2.0 for more details.
//

function Add(par1, par2)
{
  var sum;
  sum = par1 + par2;
  return(sum)
}
 
var v1 = Add(1, 2);

if (v1 ==3 ){
  print(" add: pass\n");
} else {
  $ERROR("test failed v1 expect 3 but get",  v1, "\n");
}