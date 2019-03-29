// RUN: mlir-opt %s -simplify-affine-structures | FileCheck %s

// CHECK-DAG: #map{{[0-9]+}} = (d0, d1) -> (0, 0)
#map0 = (d0, d1) -> ((d0 - d0 mod 4) mod 4, (d0 - d0 mod 128 - 64) mod 64)
// CHECK-DAG: #map{{[0-9]+}} = (d0, d1) -> (d0 + 1, d1 * 5 + 3)
#map1 = (d0, d1) -> (d1 - d0 + (d0 - d1 + 1) * 2 + d1 - 1, 1 + 2*d1 + d1 + d1 + d1 + 2)
// CHECK-DAG: #map{{[0-9]+}} = (d0, d1) -> (0, 0, 0)
#map2 = (d0, d1) -> (((d0 - d0 mod 2) * 2) mod 4, (5*d1 + 8 - (5*d1 + 4) mod 4) mod 4, 0)
// CHECK-DAG: #map{{[0-9]+}} = (d0, d1) -> (d0 ceildiv 2, d0 + 1, (d1 * 3 + 1) ceildiv 2)
#map3 = (d0, d1) -> (d0 ceildiv 2, (2*d0 + 4 + 2*d0) ceildiv 4, (8*d1 + 3 + d1) ceildiv 6)
// CHECK-DAG: #map{{[0-9]+}} = (d0, d1) -> (d0 floordiv 2, d0 * 2 + d1, (d1 + 2) floordiv 2)
#map4 = (d0, d1) -> (d0 floordiv 2, (3*d0 + 2*d1 + d0) floordiv 2, (50*d1 + 100) floordiv 100)
// CHECK-DAG: #map{{[0-9]+}} = (d0, d1) -> (0, d0 * 5 + 3)
#map5 = (d0, d1) -> ((4*d0 + 8*d1) ceildiv 2 mod 2, (2 + d0 + (8*d0 + 2) floordiv 2))
// The flattening based simplification is currently regressive on modulo
// expression simplification in the simple case (d0 mod 8 would be turn into d0
// - 8 * (d0 floordiv 8); however, in other cases like d1 - d1 mod 8, it
// would be simplified to an arithmetically simpler and more intuitive 8 * (d1
// floordiv 8).  In general, we have a choice of using either mod or floordiv
// to express the same expression in mathematically equivalent ways, and making that
// choice to minimize the number of terms or to simplify arithmetic is a TODO. 
// CHECK-DAG: #map{{[0-9]+}} = (d0, d1) -> (d0 - (d0 floordiv 8) * 8, (d1 floordiv 8) * 8)
#map6 = (d0, d1) -> (d0 mod 8, d1 - d1 mod 8)

// Test map with nested floordiv/mod. Simply should scale by GCD.
// CHECK-DAG: #map{{[0-9]+}} = (d0, d1) -> ((d0 * 72 + d1) floordiv 2304, (d0 * 72 + d1 - ((d0 * 9216 + d1 * 128) floordiv 294912) * 2304) floordiv 1152)
#map7 = (d0, d1) -> ((d0 * 9216 + d1 * 128) floordiv 294912, ((d0 * 9216 + d1 * 128) mod 294912) floordiv 147456)

// CHECK-DAG: [[SET_EMPTY_2D:#set[0-9]+]] = (d0, d1) : (1 == 0)
// CHECK-DAG: #set1 = (d0, d1) : (d0 - 100 == 0, d1 - 10 == 0, d0 * -1 + 100 >= 0, d1 >= 0, d1 + 101 >= 0)
// CHECK-DAG: #set2 = (d0, d1)[s0, s1] : (1 == 0)
// CHECK-DAG: #set3 = (d0, d1)[s0, s1] : (d0 * 7 + d1 * 5 + s0 * 11 + s1 == 0, d0 * 5 - d1 * 11 + s0 * 7 + s1 == 0, d0 * 11 + d1 * 7 - s0 * 5 + s1 == 0, d0 * 7 + d1 * 5 + s0 * 11 + s1 == 0)
// CHECK-DAG: [[SET_EMPTY_1D:#set[0-9]+]] = (d0) : (1 == 0)
// CHECK-DAG: [[SET_EMPTY_1D_2S:#set[0-9]+]] = (d0)[s0, s1] : (1 == 0)
// CHECK-DAG: [[SET_EMPTY_3D:#set[0-9]+]] = (d0, d1, d2) : (1 == 0)

// Set for test case: test_gaussian_elimination_non_empty_set2
// #set2 = (d0, d1) : (d0 - 100 == 0, d1 - 10 == 0, d0 * -1 + 100 >= 0, d1 >= 0, d1 + 101 >= 0)
#set2 = (d0, d1) : (d0 - 100 == 0, d1 - 10 == 0, -d0 + 100 >= 0, d1 >= 0, d1 + 101 >= 0)

// Set for test case: test_gaussian_elimination_empty_set3
// #set3 = (d0, d1)[s0, s1] : (1 == 0)
#set3 = (d0, d1)[s0, s1] : (d0 - s0 == 0, d0 + s0 == 0, s0 - 1 == 0)

// Set for test case: test_gaussian_elimination_non_empty_set4
#set4 = (d0, d1)[s0, s1] : (d0 * 7 + d1 * 5 + s0 * 11 + s1 == 0,
                            d0 * 5 - d1 * 11 + s0 * 7 + s1 == 0,
                            d0 * 11 + d1 * 7 - s0 * 5 + s1 == 0,
                            d0 * 7 + d1 * 5 + s0 * 11 + s1 == 0)

// Add invalid constraints to previous non-empty set to make it empty.
// Set for test case: test_gaussian_elimination_empty_set5
#set5 = (d0, d1)[s0, s1] : (d0 * 7 + d1 * 5 + s0 * 11 + s1 == 0,
                             d0 * 5 - d1 * 11 + s0 * 7 + s1 == 0,
                             d0 * 11 + d1 * 7 - s0 * 5 + s1 == 0,
                             d0 * 7 + d1 * 5 + s0 * 11 + s1 == 0,
                             d0 - 1 == 0, d0 + 2 == 0)

mlfunc @test() {
  for %n0 = 0 to 127 {
    for %n1 = 0 to 7 {
      %x  = affine_apply #map0(%n0, %n1)
      %y  = affine_apply #map1(%n0, %n1)
      %z  = affine_apply #map2(%n0, %n1)
      %w  = affine_apply #map3(%n0, %n1)
      %u  = affine_apply #map4(%n0, %n1)
      %v  = affine_apply #map5(%n0, %n1)
      %t  = affine_apply #map6(%n0, %n1)
      %s  = affine_apply #map7(%n0, %n1)
    }
  }
  return
}

// CHECK-LABEL: mlfunc @test_gaussian_elimination_empty_set0() {
mlfunc @test_gaussian_elimination_empty_set0() {
  for %i0 = 1 to 10 {
    for %i1 = 1 to 100 {
      // CHECK: [[SET_EMPTY_2D]](%i0, %i1)
      if (d0, d1) : (2 == 0)(%i0, %i1) {
      }
    }
  }
  return
}

// CHECK-LABEL: mlfunc @test_gaussian_elimination_empty_set1() {
mlfunc @test_gaussian_elimination_empty_set1() {
  for %i0 = 1 to 10 {
    for %i1 = 1 to 100 {
      // CHECK: [[SET_EMPTY_2D]](%i0, %i1)
      if (d0, d1) : (1 >= 0, -1 >= 0) (%i0, %i1) {
      }
    }
  }
  return
}

// CHECK-LABEL: mlfunc @test_gaussian_elimination_non_empty_set2() {
mlfunc @test_gaussian_elimination_non_empty_set2() {
  for %i0 = 1 to 10 {
    for %i1 = 1 to 100 {
      // CHECK: #set1(%i0, %i1)
      if #set2(%i0, %i1) {
      }
    }
  }
  return
}

// CHECK-LABEL: mlfunc @test_gaussian_elimination_empty_set3() {
mlfunc @test_gaussian_elimination_empty_set3() {
  %c7 = constant 7 : index
  %c11 = constant 11 : index
  for %i0 = 1 to 10 {
    for %i1 = 1 to 100 {
      // CHECK: #set2(%i0, %i1)[%c7, %c11]
      if #set3(%i0, %i1)[%c7, %c11] {
      }
    }
  }
  return
}

// CHECK-LABEL: mlfunc @test_gaussian_elimination_non_empty_set4() {
mlfunc @test_gaussian_elimination_non_empty_set4() {
  %c7 = constant 7 : index
  %c11 = constant 11 : index
  for %i0 = 1 to 10 {
    for %i1 = 1 to 100 {
      // CHECK: #set3(%i0, %i1)[%c7, %c11]
      if #set4(%i0, %i1)[%c7, %c11] {
      }
    }
  }
  return
}

// CHECK-LABEL: mlfunc @test_gaussian_elimination_empty_set5() {
mlfunc @test_gaussian_elimination_empty_set5() {
  %c7 = constant 7 : index
  %c11 = constant 11 : index
  for %i0 = 1 to 10 {
    for %i1 = 1 to 100 {
      // CHECK: #set2(%i0, %i1)[%c7, %c11]
      if #set5(%i0, %i1)[%c7, %c11] {
      }
    }
  }
  return
}

// CHECK-LABEL: mlfunc @test_empty_set(%arg0: index) {
mlfunc @test_empty_set(%N : index) {
  for %i = 0 to 10 {
    for %j = 0 to 10 {
      // CHECK: if [[SET_EMPTY_2D]](%i0, %i1)
      if (d0, d1) : (d0 - d1 >= 0, d1 - d0 - 1 >= 0)(%i, %j) {
        "foo"() : () -> ()
      }
      // CHECK: if [[SET_EMPTY_1D]](%i0)
      if (d0) : (d0 >= 0, -d0 - 1 >= 0)(%i) {
        "bar"() : () -> ()
      }
      // CHECK: if [[SET_EMPTY_1D]](%i0)
      if (d0) : (d0 >= 0, -d0 - 1 >= 0)(%i) {
        "foo"() : () -> ()
      }
      // CHECK: if [[SET_EMPTY_1D_2S]](%i0)[%arg0, %arg0]
      if (d0)[s0, s1] : (d0 >= 0, -d0 + s0 - 1 >= 0, -s0 >= 0)(%i)[%N, %N] {
        "bar"() : () -> ()
      }
      // CHECK: if [[SET_EMPTY_3D]](%i0, %i1, %arg0)
      // The set below implies d0 = d1; so d1 >= d0, but d0 >= d1 + 1.
      if (d0, d1, d2) : (d0 - d1 == 0, d2 - d0 >= 0, d0 - d1 - 1 >= 0)(%i, %j, %N) {
        "foo"() : () -> ()
      }
      // CHECK: if [[SET_EMPTY_2D]](%i0, %i1)
      // The set below has rational solutions but no integer solutions; GCD test catches it.
      if (d0, d1) : (d0*2 -d1*2 - 1 == 0, d0 >= 0, -d0 + 100 >= 0, d1 >= 0, -d1 + 100 >= 0)(%i, %j) {
        "foo"() : () -> ()
      }
      // CHECK: if [[SET_EMPTY_2D]](%i0, %i1)
      if (d0, d1) : (d1 == 0, d0 - 1 >= 0, - d0 - 1 >= 0)(%i, %j) {
        "foo"() : () -> ()
      }
    }
  }
  // The tests below test GCDTightenInequalities().
  for %k = 0 to 10 {
    for %l = 0 to 10 {
      // Empty because no multiple of 8 lies between 4 and 7.
      // CHECK: if [[SET_EMPTY_1D]](%i2)
      if (d0) : (8*d0 - 4 >= 0, -8*d0 + 7 >= 0)(%k) {
        "foo"() : () -> ()
      }
      // Same as above but with equalities and inequalities.
      // CHECK: if [[SET_EMPTY_2D]](%i2, %i3)
      if (d0, d1) : (d0 - 4*d1 == 0, 4*d1 - 5 >= 0, -4*d1 + 7 >= 0)(%k, %l) {
        "foo"() : () -> ()
      }
      // Same as above but with a combination of multiple identifiers. 4*d0 +
      // 8*d1 here is a multiple of 4, and so can't lie between 9 and 11. GCD
      // tightening will tighten constraints to 4*d0 + 8*d1 >= 12 and 4*d0 +
      // 8*d1 <= 8; hence infeasible.
      // CHECK: if [[SET_EMPTY_2D]](%i2, %i3)
      if (d0, d1) : (4*d0 + 8*d1 - 9 >= 0, -4*d0 - 8*d1 + 11 >=  0)(%k, %l) {
        "foo"() : () -> ()
      }
      // Same as above but with equalities added into the mix.
      // CHECK: if [[SET_EMPTY_3D]](%i2, %i2, %i3)
      if (d0, d1, d2) : (d0 - 4*d2 == 0, d0 + 8*d1 - 9 >= 0, -d0 - 8*d1 + 11 >=  0)(%k, %k, %l) {
        "foo"() : () -> ()
      }
    }
  }

  for %m = 0 to 10 {
    // CHECK: if [[SET_EMPTY_1D]](%i{{[0-9]+}})
    if (d0) : (d0 mod 2 - 3 == 0) (%m) {
      "foo"() : () -> ()
    }
  }

  return
}
