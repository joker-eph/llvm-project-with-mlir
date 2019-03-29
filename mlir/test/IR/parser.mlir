// RUN: mlir-translate -mlir-to-mlir %s | FileCheck %s

// CHECK-DAG: #map{{[0-9]+}} = (d0, d1, d2, d3, d4)[s0] -> (d0, d1, d2, d3, d4)
#map0 = (d0, d1, d2, d3, d4)[s0] -> (d0, d1, d2, d3, d4)

// CHECK-DAG: #map{{[0-9]+}} = (d0) -> (d0)
#map1 = (d0) -> (d0)

// CHECK-DAG: #map{{[0-9]+}} = (d0, d1, d2) -> (d0, d1, d2)
#map2 = (d0, d1, d2) -> (d0, d1, d2)

// CHECK-DAG: #map{{[0-9]+}} = (d0, d1, d2) -> (d1, d0, d2)
#map3 = (d0, d1, d2) -> (d1, d0, d2)

// CHECK-DAG: #map{{[0-9]+}} = (d0, d1, d2) -> (d2, d1, d0)
#map4 = (d0, d1, d2) -> (d2, d1, d0)

// CHECK-DAG: #map{{[0-9]+}} = ()[s0] -> (0, s0 - 1)
#inline_map_minmax_loop1 = ()[s0] -> (0, s0 - 1)

// CHECK-DAG: #map{{[0-9]+}} = ()[s0] -> (100, s0 + 1)
#inline_map_minmax_loop2 = ()[s0] -> (100, s0 + 1)

// CHECK: #map{{[0-9]+}} = (d0, d1)[s0] -> (d0 + d1, s0 + 1)
#inline_map_loop_bounds1 = (d0, d1)[s0] -> (d0 + d1, s0 + 1)

// CHECK: #map{{[0-9]+}} = (d0, d1)[s0] -> (d0 + d1 + s0)
#bound_map1 = (i, j)[s] -> (i + j + s)

// CHECK: #map{{[0-9]+}} = (d0, d1) -> (d0 + d1)
#inline_map_loop_bounds2 = (d0, d1) -> (d0 + d1)

// CHECK: #map{{[0-9]+}} = (d0)[s0] -> (d0 + s0, d0 - s0)
#bound_map2 = (i)[s] -> (i + s, i - s)

// CHECK-DAG: @@set0 = (d0)[s0, s1] : (d0 >= 0, d0 * -1 + s0 >= 0, s0 - 5 == 0, d0 * -1 + s1 + 1 >= 0)
@@set0 = (i)[N, M] : (i >= 0, -i + N >= 0, N - 5 == 0, -i + M + 1 >= 0)

// CHECK-DAG: @@set1 = (d0)[s0] : (d0 - 2 >= 0, d0 * -1 + 4 >= 0)

// CHECK: extfunc @foo(i32, i64) -> f32
extfunc @foo(i32, i64) -> f32

// CHECK: extfunc @bar()
extfunc @bar() -> ()

// CHECK: extfunc @baz() -> (i1, affineint, f32)
extfunc @baz() -> (i1, affineint, f32)

// CHECK: extfunc @missingReturn()
extfunc @missingReturn()

// CHECK: extfunc @int_types(i1, i2, i4, i7, i87) -> (i1, affineint, i19)
extfunc @int_types(i1, i2, i4, i7, i87) -> (i1, affineint, i19)


// CHECK: extfunc @vectors(vector<1xf32>, vector<2x4xf32>)
extfunc @vectors(vector<1 x f32>, vector<2x4xf32>)

// CHECK: extfunc @tensors(tensor<*xf32>, tensor<*xvector<2x4xf32>>, tensor<1x?x4x?x?xi32>, tensor<i8>)
extfunc @tensors(tensor<* x f32>, tensor<* x vector<2x4xf32>>,
                 tensor<1x?x4x?x?xi32>, tensor<i8>)

// CHECK: extfunc @memrefs(memref<1x?x4x?x?xi32, #map{{[0-9]+}}>, memref<i8, #map{{[0-9]+}}>)
extfunc @memrefs(memref<1x?x4x?x?xi32, #map0>, memref<i8, #map1>)

// Test memref affine map compositions.

// CHECK: extfunc @memrefs2(memref<2x4x8xi8, #map{{[0-9]+}}, 1>)
extfunc @memrefs2(memref<2x4x8xi8, #map2, 1>)

// CHECK: extfunc @memrefs23(memref<2x4x8xi8, #map{{[0-9]+}}, #map{{[0-9]+}}>)
extfunc @memrefs23(memref<2x4x8xi8, #map2, #map3, 0>)

// CHECK: extfunc @memrefs234(memref<2x4x8xi8, #map{{[0-9]+}}, #map{{[0-9]+}}, #map{{[0-9]+}}, 3>)
extfunc @memrefs234(memref<2x4x8xi8, #map2, #map3, #map4, 3>)

// Test memref inline affine map compositions.

// CHECK: extfunc @memrefs3(memref<2x4x8xi8, #map{{[0-9]+}}>)
extfunc @memrefs3(memref<2x4x8xi8, (d0, d1, d2) -> (d0, d1, d2)>)

// CHECK: extfunc @memrefs33(memref<2x4x8xi8, #map{{[0-9]+}}, #map{{[0-9]+}}, 1>)
extfunc @memrefs33(memref<2x4x8xi8, (d0, d1, d2) -> (d0, d1, d2), (d0, d1, d2) -> (d1, d0, d2), 1>)

// CHECK: extfunc @functions((memref<1x?x4x?x?xi32, #map0>, memref<i8, #map1>) -> (), () -> ())
extfunc @functions((memref<1x?x4x?x?xi32, #map0, 0>, memref<i8, #map1, 0>) -> (), ()->())

// CHECK-LABEL: cfgfunc @simpleCFG(i32, f32) -> i1 {
cfgfunc @simpleCFG(i32, f32) -> i1 {
// CHECK: bb0(%arg0: i32, %arg1: f32):
bb42 (%arg0: i32, %f: f32):
  // CHECK: %0 = "foo"() : () -> i64
  %1 = "foo"() : ()->i64
  // CHECK: "bar"(%0) : (i64) -> (i1, i1, i1)
  %2 = "bar"(%1) : (i64) -> (i1,i1,i1)
  // CHECK: return %1#1
  return %2#1 : i1
// CHECK: }
}

// CHECK-LABEL: cfgfunc @simpleCFGUsingBBArgs(i32, i64) {
cfgfunc @simpleCFGUsingBBArgs(i32, i64) {
// CHECK: bb0(%arg0: i32, %arg1: i64):
bb42 (%arg0: i32, %f: i64):
  // CHECK: "bar"(%arg1) : (i64) -> (i1, i1, i1)
  %2 = "bar"(%f) : (i64) -> (i1,i1,i1)
  // CHECK: return{{$}}
  return
// CHECK: }
}

// CHECK-LABEL: cfgfunc @multiblock() {
cfgfunc @multiblock() {
bb0:         // CHECK: bb0:
  return     // CHECK:   return
bb1:         // CHECK: bb1:   // no predecessors
  br bb4     // CHECK:   br bb3
bb2:         // CHECK: bb2:   // pred: bb2
  br bb2     // CHECK:   br bb2
bb4:         // CHECK: bb3:   // pred: bb1
  return     // CHECK:   return
}            // CHECK: }

// CHECK-LABEL: mlfunc @emptyMLF() {
mlfunc @emptyMLF() {
  return     // CHECK:  return
}            // CHECK: }

// CHECK-LABEL: mlfunc @mlfunc_with_one_arg(%arg0 : i1) -> i2 {
mlfunc @mlfunc_with_one_arg(%c : i1) -> i2 {
  // CHECK: %0 = "foo"(%arg0) : (i1) -> i2
  %b = "foo"(%c) : (i1) -> (i2)
  return %b : i2   // CHECK: return %0 : i2
} // CHECK: }

// CHECK-LABEL: mlfunc @mlfunc_with_two_args(%arg0 : f16, %arg1 : i8) -> (i1, i32) {
mlfunc @mlfunc_with_two_args(%a : f16, %b : i8) -> (i1, i32) {
  // CHECK: %0 = "foo"(%arg0, %arg1) : (f16, i8) -> (i1, i32)
  %c = "foo"(%a, %b) : (f16, i8)->(i1, i32)
  return %c#0, %c#1 : i1, i32  // CHECK: return %0#0, %0#1 : i1, i32
} // CHECK: }

// CHECK-LABEL: mlfunc @mlfunc_ops_in_loop() {
mlfunc @mlfunc_ops_in_loop() {
  // CHECK: %0 = "foo"() : () -> i64
  %a = "foo"() : ()->i64
  // CHECK: for %i0 = 1 to 10 {
  for %i = 1 to 10 {
    // CHECK: %1 = "doo"() : () -> f32
    %b = "doo"() : ()->f32
    // CHECK: "bar"(%0, %1) : (i64, f32) -> ()
    "bar"(%a, %b) : (i64, f32) -> ()
  // CHECK: }
  }
  // CHECK: return
  return
  // CHECK: }
}


// CHECK-LABEL: mlfunc @loops() {
mlfunc @loops() {
  // CHECK: for %i0 = 1 to 100 step 2 {
  for %i = 1 to 100 step 2 {
    // CHECK: for %i1 = 1 to 200 {
    for %j = 1 to 200 {
    }        // CHECK:     }
  }          // CHECK:   }
  return     // CHECK:   return
}            // CHECK: }

// CHECK-LABEL: mlfunc @complex_loops() {
mlfunc @complex_loops() {
  for %i1 = 1 to 100 {      // CHECK:   for %i0 = 1 to 100 {
    for %j1 = 1 to 100 {    // CHECK:     for %i1 = 1 to 100 {
       // CHECK: "foo"(%i0, %i1) : (affineint, affineint) -> ()
       "foo"(%i1, %j1) : (affineint,affineint) -> ()
    }                       // CHECK:     }
    "boo"() : () -> ()      // CHECK:     "boo"() : () -> ()
    for %j2 = 1 to 10 {     // CHECK:     for %i2 = 1 to 10 {
      for %k2 = 1 to 10 {   // CHECK:       for %i3 = 1 to 10 {
        "goo"() : () -> ()  // CHECK:         "goo"() : () -> ()
      }                     // CHECK:       }
    }                       // CHECK:     }
  }                         // CHECK:   }
  return                    // CHECK:   return
}                           // CHECK: }

// CHECK: mlfunc @triang_loop(%arg0 : affineint, %arg1 : memref<?x?xi32>) {
mlfunc @triang_loop(%arg0 : affineint, %arg1 : memref<?x?xi32>) {
  %c = constant 0 : i32       // CHECK: %c0_i32 = constant 0 : i32
  for %i0 = 1 to %arg0 {      // CHECK: for %i0 = 1 to %arg0 {
    for %i1 = %i0 to %arg0 {  // CHECK:   for %i1 = %i0 to %arg0 {
      store %c, %arg1[%i0, %i1] : memref<?x?xi32>  // CHECK: store %c0_i32, %arg1[%i0, %i1]
    }          // CHECK:     }
  }            // CHECK:   }
  return       // CHECK:   return
}              // CHECK: }

// CHECK: mlfunc @minmax_loop(%arg0 : affineint, %arg1 : affineint, %arg2 : memref<100xf32>) {
mlfunc @minmax_loop(%arg0 : affineint, %arg1 : affineint, %arg2 : memref<100xf32>) {
  // CHECK: for %i0 = max #map{{.*}}()[%arg0] to min #map{{.*}}()[%arg1] {
  for %i0 = max()[s]->(0,s-1)()[%arg0] to min()[s]->(100,s+1)()[%arg1] {
    // CHECK: "foo"(%arg2, %i0) : (memref<100xf32>, affineint) -> ()
    "foo"(%arg2, %i0) : (memref<100xf32>, affineint) -> ()
  }      // CHECK:   }
  return // CHECK:   return
}        // CHECK: }

// CHECK-LABEL: mlfunc @loop_bounds(%arg0 : affineint) {
mlfunc @loop_bounds(%N : affineint) {
  // CHECK: %0 = "foo"(%arg0) : (affineint) -> affineint
  %s = "foo"(%N) : (affineint) -> affineint
  // CHECK: for %i0 = %0 to %arg0
  for %i = %s to %N {
    // CHECK: for %i1 = %i0 to 0 step -1
    for %j = %i to 0 step -1 {
       // CHECK: %1 = affine_apply #map{{.*}}(%i0, %i1)[%0]
       %w = affine_apply(d0, d1)[s0] -> (d0+d1, s0+1) (%i, %j) [%s]
       // CHECK: for %i2 = #map{{.*}}(%1#0, %i0)[%arg0] to #map{{.*}}(%1#1, %i1)[%0] {
       for %k = #bound_map1 (%w#0, %i)[%N] to (i, j)[s] -> (i + j + s) (%w#1, %j)[%s] {
          // CHECK: "foo"(%i0, %i1, %i2) : (affineint, affineint, affineint) -> ()
          "foo"(%i, %j, %k) : (affineint, affineint, affineint)->()
          // CHECK: %c30 = constant 30 : affineint
          %c = constant 30 : affineint
          // CHECK: %2 = affine_apply #map{{.*}}(%arg0, %c30)
          %u = affine_apply (d0, d1)->(d0+d1) (%N, %c)
          // CHECK: for %i3 = max #map{{.*}}(%i0)[%2] to min #map{{.*}}(%i2)[%c30] {
          for %l = max #bound_map2(%i)[%u] to min #bound_map2(%k)[%c] {
            // CHECK: "bar"(%i3) : (affineint) -> ()
            "bar"(%l) : (affineint) -> ()
          } // CHECK:           }
       }    // CHECK:         }
     }      // CHECK:       }
  }         // CHECK:     }
  return    // CHECK:   return
}           // CHECK: }

// CHECK-LABEL: mlfunc @ifstmt(%arg0 : affineint) {
mlfunc @ifstmt(%N: affineint) {
  %c = constant 200 : affineint // CHECK   %c200 = constant 200
  for %i = 1 to 10 {   	        // CHECK   for %i0 = 1 to 10 {
    if @@set0(%i)[%N, %c] {     // CHECK     if @@set0(%i0)[%arg0, %c200] {
      %x = constant 1 : i32
       // CHECK: %c1_i32 = constant 1 : i32
      %y = "add"(%x, %i) : (i32, affineint) -> i32 // CHECK: %0 = "add"(%c1_i32, %i0) : (i32, affineint) -> i32
      %z = "mul"(%y, %y) : (i32, i32) -> i32 // CHECK: %1 = "mul"(%0, %0) : (i32, i32) -> i32
    } else if (i)[N] : (i - 2 >= 0, 4 - i >= 0)(%i)[%N]  {      // CHECK     } else if (@@set1(%i0)[%arg0]) {
      // CHECK: %c1 = constant 1 : affineint
      %u = constant 1 : affineint
      // CHECK: %2 = affine_apply #map{{.*}}(%i0, %i0)[%c1]
      %w = affine_apply (d0,d1)[s0] -> (d0+d1+s0) (%i, %i) [%u]
    } else {            // CHECK     } else {
      %v = constant 3 : i32 // %c3_i32 = constant 3 : i32
    }       // CHECK     }
  }         // CHECK   }
  return    // CHECK   return
}           // CHECK }

// CHECK-LABEL: mlfunc @simple_ifstmt(%arg0 : affineint) {
mlfunc @simple_ifstmt(%N: affineint) {
  %c = constant 200 : affineint // CHECK   %c200 = constant 200
  for %i = 1 to 10 {   	        // CHECK   for %i0 = 1 to 10 {
    if @@set0(%i)[%N, %c] {     // CHECK     if @@set0(%i0)[%arg0, %c200] {
      %x = constant 1 : i32
       // CHECK: %c1_i32 = constant 1 : i32
      %y = "add"(%x, %i) : (i32, affineint) -> i32 // CHECK: %0 = "add"(%c1_i32, %i0) : (i32, affineint) -> i32
      %z = "mul"(%y, %y) : (i32, i32) -> i32 // CHECK: %1 = "mul"(%0, %0) : (i32, i32) -> i32
    }       // CHECK     }
  }         // CHECK   }
  return    // CHECK   return
}           // CHECK }

// CHECK-LABEL: cfgfunc @attributes() {
cfgfunc @attributes() {
bb42:       // CHECK: bb0:

  // CHECK: "foo"()
  "foo"(){} : ()->()

  // CHECK: "foo"() {a: 1, b: -423, c: [true, false], d: 1.600000e+01}  : () -> ()
  "foo"() {a: 1, b: -423, c: [true, false], d: 16.0 } : () -> ()

  // CHECK: "foo"() {map1: #map{{[0-9]+}}}
  "foo"() {map1: #map1} : () -> ()

  // CHECK: "foo"() {map2: #map{{[0-9]+}}}
  "foo"() {map2: (d0, d1, d2) -> (d0, d1, d2)} : () -> ()

  // CHECK: "foo"() {map12: [#map{{[0-9]+}}, #map{{[0-9]+}}]}
  "foo"() {map12: [#map1, #map2]} : () -> ()

  // CHECK: "foo"() {cfgfunc: [], d: 1.000000e-09, i123: 7, if: "foo"} : () -> ()
  "foo"() {if: "foo", cfgfunc: [], i123: 7, d: 1.e-9} : () -> ()

  // CHECK: "foo"() {fn: @attributes : () -> (), if: @ifstmt : (affineint) -> ()} : () -> ()
  "foo"() {fn: @attributes : () -> (), if: @ifstmt : (affineint) -> ()} : () -> ()
  return
}

// CHECK-LABEL: cfgfunc @ssa_values() -> (i16, i8) {
cfgfunc @ssa_values() -> (i16, i8) {
bb0:       // CHECK: bb0:
  // CHECK: %0 = "foo"() : () -> (i1, i17)
  %0 = "foo"() : () -> (i1, i17)
  br bb2

bb1:       // CHECK: bb1: // pred: bb2
  // CHECK: %1 = "baz"(%2#1, %2#0, %0#1) : (f32, i11, i17) -> (i16, i8)
  %1 = "baz"(%2#1, %2#0, %0#1) : (f32, i11, i17) -> (i16, i8)

  // CHECK: return %1#0, %1#1 : i16, i8
  return %1#0, %1#1 : i16, i8

bb2:       // CHECK: bb2:  // pred: bb0
  // CHECK: %2 = "bar"(%0#0, %0#1) : (i1, i17) -> (i11, f32)
  %2 = "bar"(%0#0, %0#1) : (i1, i17) -> (i11, f32)
  br bb1
}

// CHECK-LABEL: cfgfunc @bbargs() -> (i16, i8) {
cfgfunc @bbargs() -> (i16, i8) {
bb0:       // CHECK: bb0:
  // CHECK: %0 = "foo"() : () -> (i1, i17)
  %0 = "foo"() : () -> (i1, i17)
  br bb1(%0#1, %0#0 : i17, i1)

bb1(%x: i17, %y: i1):       // CHECK: bb1(%1: i17, %2: i1):
  // CHECK: %3 = "baz"(%1, %2, %0#1) : (i17, i1, i17) -> (i16, i8)
  %1 = "baz"(%x, %y, %0#1) : (i17, i1, i17) -> (i16, i8)
  return %1#0, %1#1 : i16, i8
}

// CHECK-LABEL: cfgfunc @condbr_simple
cfgfunc @condbr_simple() -> (i32) {
bb0:
  %cond = "foo"() : () -> i1
  %a = "bar"() : () -> i32
  %b = "bar"() : () -> i64
  // CHECK: cond_br %0, bb1(%1 : i32), bb2(%2 : i64)
  cond_br %cond, bb1(%a : i32), bb2(%b : i64)

// CHECK: bb1({{.*}}: i32): // pred: bb0
bb1(%x : i32):
  br bb2(%b: i64)

// CHECK: bb2({{.*}}: i64): // 2 preds: bb0, bb1
bb2(%y : i64):
  %z = "foo"() : () -> i32
  return %z : i32
}

// CHECK-LABEL: cfgfunc @condbr_moarargs
cfgfunc @condbr_moarargs() -> (i32) {
bb0:
  %cond = "foo"() : () -> i1
  %a = "bar"() : () -> i32
  %b = "bar"() : () -> i64
  // CHECK: cond_br %0, bb1(%1, %2 : i32, i64), bb2(%2, %1, %1 : i64, i32, i32)
  cond_br %cond, bb1(%a, %b : i32, i64), bb2(%b, %a, %a : i64, i32, i32)

bb1(%x : i32, %y : i64):
  return %x : i32

bb2(%x2 : i64, %y2 : i32, %z2 : i32):
  %z = "foo"() : () -> i32
  return %z : i32
}


// Test pretty printing of constant names.
// CHECK-LABEL: cfgfunc @constants
cfgfunc @constants() -> (i32, i23, i23, i1, i1) {
bb0:
  // CHECK: %c42_i32 = constant 42 : i32
  %x = constant 42 : i32
  // CHECK: %c17_i23 = constant 17 : i23
  %y = constant 17 : i23

  // This is a redundant definition of 17, the asmprinter gives it a unique name
  // CHECK: %c17_i23_0 = constant 17 : i23
  %z = constant 17 : i23

  // CHECK: %true = constant 1 : i1
  %t = constant 1 : i1
  // CHECK: %false = constant 0 : i1
  %f = constant 0 : i1

  // CHECK: return %c42_i32, %c17_i23, %c17_i23_0, %true, %false
  return %x, %y, %z, %t, %f : i32, i23, i23, i1, i1
}

// CHECK-LABEL: cfgfunc @typeattr
cfgfunc @typeattr() -> () {
bb0:
// CHECK: "foo"() {bar: tensor<*xf32>} : () -> ()
  "foo"(){bar: tensor<*xf32>} : () -> ()
  return
}

// CHECK-LABEL: cfgfunc @stringquote
cfgfunc @stringquote() -> () {
bb0:
  // CHECK: "foo"() {bar: "a\22quoted\22string"} : () -> ()
  "foo"(){bar: "a\"quoted\"string"} : () -> ()
  return
}

// CHECK-LABEL: cfgfunc @floatAttrs
cfgfunc @floatAttrs() -> () {
bb0:
  // CHECK: "foo"() {a: 4.000000e+00, b: 2.000000e+00, c: 7.100000e+00, d: -0.000000e+00} : () -> ()
  "foo"(){a: 4.0, b: 2.0, c: 7.1, d: -0.0} : () -> ()
  return
}

// CHECK-LABEL: extfunc @extfuncattr
extfunc @extfuncattr() -> ()
  // CHECK: attributes {a: "a\22quoted\22string", b: 4.000000e+00, c: tensor<*xf32>}
  attributes {a: "a\"quoted\"string", b: 4.0, c: tensor<*xf32>}

// CHECK-LABEL: extfunc @extfuncattrempty
extfunc @extfuncattrempty() -> ()
  // CHECK-EMPTY
  attributes {}

// CHECK-LABEL: cfgfunc @cfgfuncattr
cfgfunc @cfgfuncattr() -> ()
  // CHECK: attributes {a: "a\22quoted\22string", b: 4.000000e+00, c: tensor<*xf32>}
  attributes {a: "a\"quoted\"string", b: 4.0, c: tensor<*xf32>} {
bb0:
  return
}

// CHECK-LABEL: cfgfunc @cfgfuncattrempty
cfgfunc @cfgfuncattrempty() -> ()
  // CHECK-EMPTY
  attributes {} {
bb0:
  return
}

// CHECK-LABEL: mlfunc @mlfuncattr
mlfunc @mlfuncattr() -> ()
  // CHECK: attributes {a: "a\22quoted\22string", b: 4.000000e+00, c: tensor<*xf32>}
  attributes {a: "a\"quoted\"string", b: 4.0, c: tensor<*xf32>} {
  return
}

// CHECK-LABEL: mlfunc @mlfuncattrempty
mlfunc @mlfuncattrempty() -> ()
  // CHECK-EMPTY
  attributes {} {
  return
}

// CHECK-label mlfunc @mlfuncsimplemap
#mapsimple0 = ()[s0, s1, s2] -> (s0)
#mapsimple1 = (d0)[s0, s1, s2] -> (s1)
mlfunc @mlfuncsimplemap(%arg0 : affineint, %arg1 : affineint, %arg2 : affineint) -> () {
  for %i0 = 0 to #mapsimple0()[%arg0, %arg1, %arg2] { // CHECK: for %i0 = 0 to %arg0 {
    for %i1 = 0 to #mapsimple1(%i0)[%arg0, %arg1, %arg2] { // CHECK: for %i1 = 0 to %arg1 {
      %c42_i32 = constant 42 : i32
    }
  }
  return
}
