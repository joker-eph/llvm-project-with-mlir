// RUN: mlir-opt -lower-if-and-for %s | FileCheck %s

// CHECK-DAG: [[map0:#map[0-9]+]] = () -> (0)
// CHECK-DAG: [[map1:#map[0-9]+]] = () -> (1)
// CHECK-DAG: [[map7:#map[0-9]+]] = () -> (7)
// CHECK-DAG: [[map18:#map[0-9]+]] = () -> (18)
// CHECK-DAG: [[map37:#map[0-9]+]] = () -> (37)
// CHECK-DAG: [[map42:#map[0-9]+]] = () -> (42)
// CHECK-DAG: [[map56:#map[0-9]+]] = () -> (56)
// CHECK-DAG: [[map1Sym:#map[0-9]+]] = ()[s0] -> (s0)
// CHECK-DAG: [[map1Id:#map[0-9]+]] = (d0) -> (d0)
// CHECK-DAG: [[mapAdd1:#map[0-9]+]] = (d0) -> (d0 + 1)
// CHECK-DAG: [[mapAdd2:#map[0-9]+]] = (d0) -> (d0 + 2)
// CHECK-DAG: [[mapAdd3:#map[0-9]+]] = (d0) -> (d0 + 3)
// CHECK-DAG: [[multiMap1:#map[0-9]+]] = (d0)[s0] -> (d0, d0 * -1 + s0)
// CHECK-DAG: [[multiMap2:#map[0-9]+]] = (d0)[s0] -> (s0, d0 + 10)
// CHECK-DAG: [[multi7Map:#map[0-9]+]] = (d0) -> (d0, d0, d0, d0, d0, d0, d0)
// Maps produced from individual affine expressions that appear in "if" conditions.
// CHECK-DAG: [[setMap20:#map[0-9]+]] = (d0) -> (d0 * -1 + 20)
// CHECK-DAG: [[setMap10:#map[0-9]+]] = (d0) -> (d0 - 10)
// CHECK-DAG: [[setMapDiff:#map[0-9]+]] = (d0)[s0, s1, s2, s3] -> (d0 * -1 + s0 + 1)
// CHECK-DAG: [[setMapS0:#map[0-9]+]] = (d0)[s0, s1, s2, s3] -> (s0 - 1)
// CHECK-DAG: [[setMapS1:#map[0-9]+]] = (d0)[s0, s1, s2, s3] -> (s1 - 1)
// CHECK-DAG: [[setMapS2:#map[0-9]+]] = (d0)[s0, s1, s2, s3] -> (s2 - 1)
// CHECK-DAG: [[setMapS3:#map[0-9]+]] = (d0)[s0, s1, s2, s3] -> (s3 - 42)

// CHECK-LABEL: cfgfunc @empty() {
mlfunc @empty() {
  return     // CHECK:  return
}            // CHECK: }

extfunc @body(index) -> ()

// Simple loops are properly converted.
// CHECK-LABEL: cfgfunc @simple_loop() {
// CHECK-NEXT:   %0 = affine_apply [[map1]]()
// CHECK-NEXT:   %1 = affine_apply [[map42]]()
// CHECK-NEXT:   br ^bb1(%0 : index)
// CHECK-NEXT: ^bb1(%2: index):	// 2 preds: ^bb0, ^bb2
// CHECK-NEXT:   %3 = cmpi "slt", %2, %1 : index
// CHECK-NEXT:   cond_br %3, ^bb2, ^bb3
// CHECK-NEXT: ^bb2:	// pred: ^bb1
// CHECK-NEXT:   call @body(%2) : (index) -> ()
// CHECK-NEXT:   %4 = affine_apply [[mapAdd1]](%2)
// CHECK-NEXT:   br ^bb1(%4 : index)
// CHECK-NEXT: ^bb3:	// pred: ^bb1
// CHECK-NEXT:   return
// CHECK-NEXT: }
mlfunc @simple_loop() {
  for %i = 1 to 42 {
    call @body(%i) : (index) -> ()
  }
  return
}

// Direct calls get renamed if asked (IR data structures properly updated) and
// keep the same name otherwise.
cfgfunc @simple_caller() {
^bb0:
// CHECK: call @simple_loop() : () -> ()
  call @simple_loop() : () -> ()
  return
}

// Constant loads get renamed if asked (IR data structure properly updated) and
// keep the same name otherwise.
cfgfunc @simple_indirect_caller() {
^bb0:
// CHECK: %f = constant @simple_loop : () -> ()
  %f = constant @simple_loop : () -> ()
  call_indirect %f() : () -> ()
  return
}

cfgfunc @nested_attributes() {
^bb0:
  %0 = constant 0 : index
// CHECK: call @body(%c0) {attr1: [@simple_loop : () -> (), @simple_loop : () -> ()]} : (index) -> ()
  call @body(%0) {attr1: [@simple_loop : () -> (), @simple_loop : () -> ()]} : (index) -> ()
// Note: the {{\[}} construct is necessary to prevent FileCheck from
// interpreting [[ as the start of its variable in the pattern below.
// CHECK: call @body(%c0) {attr2: {{\[}}{{\[}}{{\[}}@simple_loop : () -> ()]], [@simple_loop : () -> ()]]} : (index) -> ()
  call @body(%0) {attr2: [[[@simple_loop : () -> ()]], [@simple_loop : () -> ()]]} : (index) -> ()
  return
}

// CHECK-LABEL: cfgfunc @ml_caller() {
mlfunc @ml_caller() {
// Direct calls inside ML functions are renamed if asked (given that the
// function itself is also converted).
// CHECK: call @simple_loop() : () -> ()
  call @simple_loop() : () -> ()
// Direct calls to not yet declared ML functions are also renamed.
// CHECK: call @more_imperfectly_nested_loops() : () -> ()
  call @more_imperfectly_nested_loops() : () -> ()
  return
}

/////////////////////////////////////////////////////////////////////

extfunc @body_args(index) -> (index)
extfunc @other(index, i32) -> (i32)

// Arguments and return values of the functions are converted.
// CHECK-LABEL: cfgfunc @mlfunc_args(%arg0: i32, %arg1: i32) -> (i32, i32) {
// CHECK-NEXT:   %c0_i32 = constant 0 : i32
// CHECK-NEXT:   %0 = affine_apply [[map0]]()
// CHECK-NEXT:   %1 = affine_apply [[map42]]()
// CHECK-NEXT:   br ^bb1(%0 : index)
// CHECK-NEXT: ^bb1(%2: index):	// 2 preds: ^bb0, ^bb2
// CHECK-NEXT:   %3 = cmpi "slt", %2, %1 : index
// CHECK-NEXT:   cond_br %3, ^bb2, ^bb3
// CHECK-NEXT: ^bb2:	// pred: ^bb1
// CHECK-NEXT:   %4 = call @body_args(%2) : (index) -> index
// CHECK-NEXT:   %5 = call @other(%4, %arg0) : (index, i32) -> i32
// CHECK-NEXT:   %6 = call @other(%4, %5) : (index, i32) -> i32
// CHECK-NEXT:   %7 = call @other(%4, %arg1) : (index, i32) -> i32
// CHECK-NEXT:   %8 = affine_apply [[mapAdd1]](%2)
// CHECK-NEXT:   br ^bb1(%8 : index)
// CHECK-NEXT: ^bb3:	// pred: ^bb1
// CHECK-NEXT:   %c0 = constant 0 : index
// CHECK-NEXT:   %9 = call @other(%c0, %c0_i32) : (index, i32) -> i32
// CHECK-NEXT:   return %c0_i32, %9 : i32, i32
// CHECK-NEXT: }
mlfunc @mlfunc_args(%a : i32, %b : i32) -> (i32, i32) {
  %r1 = constant 0 : i32
  for %i = 0 to 42 {
    %1 = call @body_args(%i) : (index) -> (index)
    %2 = call @other(%1, %a) : (index, i32) -> (i32)
    %3 = call @other(%1, %2) : (index, i32) -> (i32)
    %4 = call @other(%1, %b) : (index, i32) -> (i32)
  }
  %ri = constant 0 : index
  %r2 = call @other(%ri, %r1) : (index, i32) -> (i32)
  return %r1, %r2 : i32, i32
}

/////////////////////////////////////////////////////////////////////

extfunc @pre(index) -> ()
extfunc @body2(index, index) -> ()
extfunc @post(index) -> ()

// CHECK-LABEL: cfgfunc @imperfectly_nested_loops() {
// CHECK-NEXT:   %0 = affine_apply [[map0]]()
// CHECK-NEXT:   %1 = affine_apply [[map42]]()
// CHECK-NEXT:   br ^bb1(%0 : index)
// CHECK-NEXT: ^bb1(%2: index):	// 2 preds: ^bb0, ^bb5
// CHECK-NEXT:   %3 = cmpi "slt", %2, %1 : index
// CHECK-NEXT:   cond_br %3, ^bb2, ^bb6
// CHECK-NEXT: ^bb2:	// pred: ^bb1
// CHECK-NEXT:   call @pre(%2) : (index) -> ()
// CHECK-NEXT:   %4 = affine_apply [[map7]]()
// CHECK-NEXT:   %5 = affine_apply [[map56]]()
// CHECK-NEXT:   br ^bb3(%4 : index)
// CHECK-NEXT: ^bb3(%6: index):	// 2 preds: ^bb2, ^bb4
// CHECK-NEXT:   %7 = cmpi "slt", %6, %5 : index
// CHECK-NEXT:   cond_br %7, ^bb4, ^bb5
// CHECK-NEXT: ^bb4:	// pred: ^bb3
// CHECK-NEXT:   call @body2(%2, %6) : (index, index) -> ()
// CHECK-NEXT:   %8 = affine_apply [[mapAdd2]](%6)
// CHECK-NEXT:   br ^bb3(%8 : index)
// CHECK-NEXT: ^bb5:	// pred: ^bb3
// CHECK-NEXT:   call @post(%2) : (index) -> ()
// CHECK-NEXT:   %9 = affine_apply [[mapAdd1]](%2)
// CHECK-NEXT:   br ^bb1(%9 : index)
// CHECK-NEXT: ^bb6:	// pred: ^bb1
// CHECK-NEXT:   return
// CHECK-NEXT: }
mlfunc @imperfectly_nested_loops() {
  for %i = 0 to 42 {
    call @pre(%i) : (index) -> ()
    for %j = 7 to 56 step 2 {
      call @body2(%i, %j) : (index, index) -> ()
    }
    call @post(%i) : (index) -> ()
  }
  return
}

/////////////////////////////////////////////////////////////////////

extfunc @mid(index) -> ()
extfunc @body3(index, index) -> ()

// CHECK-LABEL: cfgfunc @more_imperfectly_nested_loops() {
// CHECK-NEXT:   %0 = affine_apply [[map0]]()
// CHECK-NEXT:   %1 = affine_apply [[map42]]()
// CHECK-NEXT:   br ^bb1(%0 : index)
// CHECK-NEXT: ^bb1(%2: index):	// 2 preds: ^bb0, ^bb8
// CHECK-NEXT:   %3 = cmpi "slt", %2, %1 : index
// CHECK-NEXT:   cond_br %3, ^bb2, ^bb9
// CHECK-NEXT: ^bb2:	// pred: ^bb1
// CHECK-NEXT:   call @pre(%2) : (index) -> ()
// CHECK-NEXT:   %4 = affine_apply [[map7]]()
// CHECK-NEXT:   %5 = affine_apply [[map56]]()
// CHECK-NEXT:   br ^bb3(%4 : index)
// CHECK-NEXT: ^bb3(%6: index):	// 2 preds: ^bb2, ^bb4
// CHECK-NEXT:   %7 = cmpi "slt", %6, %5 : index
// CHECK-NEXT:   cond_br %7, ^bb4, ^bb5
// CHECK-NEXT: ^bb4:	// pred: ^bb3
// CHECK-NEXT:   call @body2(%2, %6) : (index, index) -> ()
// CHECK-NEXT:   %8 = affine_apply [[mapAdd2]](%6)
// CHECK-NEXT:   br ^bb3(%8 : index)
// CHECK-NEXT: ^bb5:	// pred: ^bb3
// CHECK-NEXT:   call @mid(%2) : (index) -> ()
// CHECK-NEXT:   %9 = affine_apply [[map18]]()
// CHECK-NEXT:   %10 = affine_apply [[map37]]()
// CHECK-NEXT:   br ^bb6(%9 : index)
// CHECK-NEXT: ^bb6(%11: index):	// 2 preds: ^bb5, ^bb7
// CHECK-NEXT:   %12 = cmpi "slt", %11, %10 : index
// CHECK-NEXT:   cond_br %12, ^bb7, ^bb8
// CHECK-NEXT: ^bb7:	// pred: ^bb6
// CHECK-NEXT:   call @body3(%2, %11) : (index, index) -> ()
// CHECK-NEXT:   %13 = affine_apply [[mapAdd3]](%11)
// CHECK-NEXT:   br ^bb6(%13 : index)
// CHECK-NEXT: ^bb8:	// pred: ^bb6
// CHECK-NEXT:   call @post(%2) : (index) -> ()
// CHECK-NEXT:   %14 = affine_apply [[mapAdd1]](%2)
// CHECK-NEXT:   br ^bb1(%14 : index)
// CHECK-NEXT: ^bb9:	// pred: ^bb1
// CHECK-NEXT:   return
// CHECK-NEXT: }
mlfunc @more_imperfectly_nested_loops() {
  for %i = 0 to 42 {
    call @pre(%i) : (index) -> ()
    for %j = 7 to 56 step 2 {
      call @body2(%i, %j) : (index, index) -> ()
    }
    call @mid(%i) : (index) -> ()
    for %k = 18 to 37 step 3 {
      call @body3(%i, %k) : (index, index) -> ()
    }
    call @post(%i) : (index) -> ()
  }
  return
}

// CHECK-LABEL: cfgfunc @affine_apply_loops_shorthand(%arg0: index) {
// CHECK-NEXT:    %0 = affine_apply #map3()
// CHECK-NEXT:    %1 = affine_apply #map10()[%arg0]
// CHECK-NEXT:    br ^bb1(%0 : index)
// CHECK-NEXT:  ^bb1(%2: index):        // 2 preds: ^bb0, ^bb5
// CHECK-NEXT:    %3 = cmpi "slt", %2, %1 : index
// CHECK-NEXT:    cond_br %3, ^bb2, ^bb6
// CHECK-NEXT:  ^bb2:   // pred: ^bb1
// CHECK-NEXT:    %4 = affine_apply #map11(%2)
// CHECK-NEXT:    %5 = affine_apply #map1()
// CHECK-NEXT:    br ^bb3(%4 : index)
// CHECK-NEXT:  ^bb3(%6: index):        // 2 preds: ^bb2, ^bb4
// CHECK-NEXT:    %7 = cmpi "slt", %6, %5 : index
// CHECK-NEXT:    cond_br %7, ^bb4, ^bb5
// CHECK-NEXT:  ^bb4:   // pred: ^bb3
// CHECK-NEXT:    call @body2(%2, %6) : (index, index) -> ()
// CHECK-NEXT:    %8 = affine_apply #map2(%6)
// CHECK-NEXT:    br ^bb3(%8 : index)
// CHECK-NEXT:  ^bb5:   // pred: ^bb3
// CHECK-NEXT:    %9 = affine_apply #map2(%2)
// CHECK-NEXT:    br ^bb1(%9 : index)
// CHECK-NEXT:  ^bb6:   // pred: ^bb1
// CHECK-NEXT:    return
// CHECK-NEXT:  }
mlfunc @affine_apply_loops_shorthand(%N : index) {
  for %i = 0 to %N {
    for %j = %i to 42 {
      call @body2(%i, %j) : (index, index) -> ()
    }
  }
  return
}

/////////////////////////////////////////////////////////////////////

extfunc @get_idx() -> (index)

#set1 = (d0) : (20 - d0 >= 0)
#set2 = (d0) : (d0 - 10 >= 0)

// CHECK-LABEL: cfgfunc @if_only() {
// CHECK-NEXT:   %0 = call @get_idx() : () -> index
// CHECK-NEXT:   %c0 = constant 0 : index
// CHECK-NEXT:   %1 = affine_apply [[setMap20]](%0)
// CHECK-NEXT:   %2 = cmpi "sge", %1, %c0 : index
// CHECK-NEXT:   cond_br %2, [[thenBB:\^bb[0-9]+]], [[endBB:\^bb[0-9]+]]
// CHECK-NEXT: [[thenBB]]:
// CHECK-NEXT:   call @body(%0) : (index) -> ()
// CHECK-NEXT:   br [[endBB]]
// CHECK-NEXT: [[endBB]]:
// CHECK-NEXT:   return
// CHECK-NEXT: }
mlfunc @if_only() {
  %i = call @get_idx() : () -> (index)
  if #set1(%i) {
    call @body(%i) : (index) -> ()
  }
  return
}

// CHECK-LABEL: cfgfunc @if_else() {
// CHECK-NEXT:   %0 = call @get_idx() : () -> index
// CHECK-NEXT:   %c0 = constant 0 : index
// CHECK-NEXT:   %1 = affine_apply [[setMap20]](%0)
// CHECK-NEXT:   %2 = cmpi "sge", %1, %c0 : index
// CHECK-NEXT:   cond_br %2, [[thenBB:\^bb[0-9]+]], [[elseBB:\^bb[0-9]+]]
// CHECK-NEXT: [[thenBB]]:
// CHECK-NEXT:   call @body(%0) : (index) -> ()
// CHECK-NEXT:   br [[endBB:\^bb[0-9]+]]
// CHECK-NEXT: [[elseBB]]:
// CHECK-NEXT:   call @mid(%0) : (index) -> ()
// CHECK-NEXT:   br [[endBB]]
// CHECK-NEXT: [[endBB]]:
// CHECK-NEXT:   return
// CHECK-NEXT: }
mlfunc @if_else() {
  %i = call @get_idx() : () -> (index)
  if #set1(%i) {
    call @body(%i) : (index) -> ()
  } else {
    call @mid(%i) : (index) -> ()
  }
  return
}

// CHECK-LABEL: cfgfunc @nested_ifs() {
// CHECK-NEXT:   %0 = call @get_idx() : () -> index
// CHECK-NEXT:   %c0 = constant 0 : index
// CHECK-NEXT:   %1 = affine_apply #map12(%0)
// CHECK-NEXT:   %2 = cmpi "sge", %1, %c0 : index
// CHECK-NEXT:   cond_br %2, ^bb1, ^bb4
// CHECK-NEXT: ^bb1:   // pred: ^bb0
// CHECK-NEXT:   %c0_0 = constant 0 : index
// CHECK-NEXT:   %3 = affine_apply #map13(%0)
// CHECK-NEXT:   %4 = cmpi "sge", %3, %c0_0 : index
// CHECK-NEXT:   cond_br %4, ^bb2, ^bb3
// CHECK-NEXT: ^bb2:   // pred: ^bb1
// CHECK-NEXT:   call @body(%0) : (index) -> ()
// CHECK-NEXT:   br ^bb3
// CHECK-NEXT: ^bb3:   // 2 preds: ^bb1, ^bb2
// CHECK-NEXT:   br ^bb7
// CHECK-NEXT: ^bb4:   // pred: ^bb0
// CHECK-NEXT:   %c0_1 = constant 0 : index
// CHECK-NEXT:   %5 = affine_apply #map13(%0)
// CHECK-NEXT:   %6 = cmpi "sge", %5, %c0_1 : index
// CHECK-NEXT:   cond_br %6, ^bb5, ^bb6
// CHECK-NEXT: ^bb5:   // pred: ^bb4
// CHECK-NEXT:   call @mid(%0) : (index) -> ()
// CHECK-NEXT:   br ^bb6
// CHECK-NEXT: ^bb6:   // 2 preds: ^bb4, ^bb5
// CHECK-NEXT:   br ^bb7
// CHECK-NEXT: ^bb7:   // 2 preds: ^bb3, ^bb6
// CHECK-NEXT:   return
// CHECK-NEXT: }
mlfunc @nested_ifs() {
  %i = call @get_idx() : () -> (index)
  if #set1(%i) {
    if #set2(%i) {
      call @body(%i) : (index) -> ()
    }
  } else {
    if #set2(%i) {
      call @mid(%i) : (index) -> ()
    }
  }
  return
}

#setN = (d0)[N,M,K,L] : (N - d0 + 1 >= 0, N - 1 >= 0, M - 1 >= 0, K - 1 >= 0, L - 42 == 0)

// CHECK-LABEL: cfgfunc @multi_cond(%arg0: index, %arg1: index, %arg2: index, %arg3: index) {
// CHECK-NEXT:   %0 = call @get_idx() : () -> index
// CHECK-NEXT:   %c0 = constant 0 : index
// CHECK-NEXT:   %1 = affine_apply [[setMapDiff]](%0)[%arg0, %arg1, %arg2, %arg3]
// CHECK-NEXT:   %2 = cmpi "sge", %1, %c0 : index
// CHECK-NEXT:   cond_br %2, [[cond2BB:\^bb[0-9]+]], [[elseBB:\^bb[0-9]+]]
// CHECK-NEXT: [[cond2BB]]:
// CHECK-NEXT:   %3 = affine_apply [[setMapS0]](%0)[%arg0, %arg1, %arg2, %arg3]
// CHECK-NEXT:   %4 = cmpi "sge", %3, %c0 : index
// CHECK-NEXT:   cond_br %4, [[cond3BB:\^bb[0-9]+]], [[elseBB]]
// CHECK-NEXT: [[cond3BB]]:
// CHECK-NEXT:   %5 = affine_apply [[setMapS1]](%0)[%arg0, %arg1, %arg2, %arg3]
// CHECK-NEXT:   %6 = cmpi "sge", %5, %c0 : index
// CHECK-NEXT:   cond_br %6, [[cond4BB:\^bb[0-9]+]], [[elseBB]]
// CHECK-NEXT: [[cond4BB]]:
// CHECK-NEXT:   %7 = affine_apply [[setMapS2]](%0)[%arg0, %arg1, %arg2, %arg3]
// CHECK-NEXT:   %8 = cmpi "sge", %7, %c0 : index
// CHECK-NEXT:   cond_br %8, [[cond5BB:\^bb[0-9]+]], [[elseBB]]
// CHECK-NEXT: [[cond5BB]]:
// CHECK-NEXT:   %9 = affine_apply [[setMapS3]](%0)[%arg0, %arg1, %arg2, %arg3]
// CHECK-NEXT:   %10 = cmpi "eq", %9, %c0 : index
// CHECK-NEXT:   cond_br %10, [[thenBB:\^bb[0-9]+]], [[elseBB]]
// CHECK-NEXT: [[thenBB]]:
// CHECK-NEXT:   call @body(%0) : (index) -> ()
// CHECK-NEXT:   br [[endBB:\^bb[0-9]+]]
// CHECK-NEXT: [[elseBB]]:
// CHECK-NEXT:   call @mid(%0) : (index) -> ()
// CHECK-NEXT:   br [[endBB]]
// CHECK-NEXT: [[endBB]]:
// CHECK-NEXT:   return
// CHECK-NEXT: }
mlfunc @multi_cond(%N : index, %M : index, %K : index, %L : index) {
  %i = call @get_idx() : () -> (index)
  if #setN(%i)[%N,%M,%K,%L] {
    call @body(%i) : (index) -> ()
  } else {
    call @mid(%i) : (index) -> ()
  }
  return
}

// CHECK-LABEL: cfgfunc @if_for() {
mlfunc @if_for() {
// CHECK-NEXT:   %0 = call @get_idx() : () -> index
  %i = call @get_idx() : () -> (index)
// CHECK-NEXT:   %c0 = constant 0 : index
// CHECK-NEXT:   %1 = affine_apply [[setMap20]](%0)
// CHECK-NEXT:   %2 = cmpi "sge", %1, %c0 : index
// CHECK-NEXT:   cond_br %2, [[midLoopInitBB:\^bb[0-9]+]], [[outerEndBB:\^bb[0-9]+]]
// CHECK-NEXT: [[midLoopInitBB]]:
// CHECK-NEXT:   %3 = affine_apply [[map0]]()
// CHECK-NEXT:   %4 = affine_apply [[map42]]()
// CHECK-NEXT:   br [[midLoopCondBB:\^bb[0-9]+]](%3 : index)
// CHECK-NEXT: [[midLoopCondBB]](%5: index):
// CHECK-NEXT:   %6 = cmpi "slt", %5, %4 : index
// CHECK-NEXT:   cond_br %6, [[midLoopBodyBB:\^bb[0-9]+]], [[outerEndBB:\^bb[0-9]+]]
// CHECK-NEXT: [[midLoopBodyBB]]:
// CHECK-NEXT:   %c0_0 = constant 0 : index
// CHECK-NEXT:   %7 = affine_apply [[setMap10]](%5)
// CHECK-NEXT:   %8 = cmpi "sge", %7, %c0_0 : index
// CHECK-NEXT:   cond_br %8, [[innerThenBB:\^bb[0-9]+]], [[innerEndBB:\^bb[0-9]+]]
// CHECK-NEXT: [[innerThenBB:\^bb[0-9]+]]:
// CHECK-NEXT:   call @body2(%0, %5) : (index, index) -> ()
// CHECK-NEXT:   br [[innerEndBB]]
// CHECK-NEXT: [[innerEndBB]]:
// CHECK-NEXT:   %9 = affine_apply [[mapAdd1]](%5)
// CHECK-NEXT:   br [[midLoopCondBB]](%9 : index)
// CHECK-NEXT: [[outerEndBB]]:
// CHECK-NEXT:   br [[outerLoopInit:\^bb[0-9]+]]
  if #set1(%i) {
    for %j = 0 to 42 {
      if #set2(%j) {
        call @body2(%i, %j) : (index, index) -> ()
      }
    }
  }
// CHECK-NEXT: [[outerLoopInit]]:
// CHECK-NEXT:   %10 = affine_apply [[map0]]()
// CHECK-NEXT:   %11 = affine_apply [[map42]]()
// CHECK-NEXT:   br [[outerLoopCond:\^bb[0-9]+]](%10 : index)
// CHECK-NEXT: [[outerLoopCond]](%12: index):
// CHECK-NEXT:   %13 = cmpi "slt", %12, %11 : index
// CHECK-NEXT:   cond_br %13, [[outerLoopBody:\^bb[0-9]+]], [[outerLoopEnd:\^bb[0-9]+]]
// CHECK-NEXT: [[outerLoopBody]]:
// CHECK-NEXT:   %c0_1 = constant 0 : index
// CHECK-NEXT:   %14 = affine_apply [[setMap10]](%12)
// CHECK-NEXT:   %15 = cmpi "sge", %14, %c0_1 : index
// CHECK-NEXT:   cond_br %15, [[innerLoopInitBB:\^bb[0-9]+]], [[midEndBB:\^bb[0-9]+]]
// CHECK-NEXT: [[innerLoopInitBB:\^bb[0-9]+]]:
// CHECK-NEXT:   %16 = affine_apply [[map0]]()
// CHECK-NEXT:   %17 = affine_apply [[map42]]()
// CHECK-NEXT:   br [[innerLoopCondBB:\^bb[0-9]+]](%16 : index)
// CHECK-NEXT: [[innerLoopCondBB]](%18: index):
// CHECK-NEXT:   %19 = cmpi "slt", %18, %17 : index
// CHECK-NEXT:   cond_br %19, [[innerLoopBodyBB:\^bb[0-9]+]], [[innerLoopEndBB:\^bb[0-9]+]]
// CHECK-NEXT: [[innerLoopBodyBB]]:
// CHECK-NEXT:   call @body3(%12, %18) : (index, index) -> ()
// CHECK-NEXT:   %20 = affine_apply [[mapAdd1]](%18)
// CHECK-NEXT:   br [[innerLoopCondBB]](%20 : index)
// CHECK-NEXT: [[innerLoopEndBB]]:
// CHECK-NEXT:   br [[midEndBB]]
// CHECK-NEXT: [[midEndBB]]:
// CHECK-NEXT:   %21 = affine_apply [[mapAdd1]](%12)
// CHECK-NEXT:   br [[outerLoopCond]](%21 : index)
  for %k = 0 to 42 {
    if #set2(%k) {
      for %l = 0 to 42 {
        call @body3(%k, %l) : (index, index) -> ()
      }
    }
  }
// CHECK-NEXT: [[outerLoopEnd]]:
// CHECK-NEXT:   return
  return
}

#lbMultiMap = (d0)[s0] -> (d0, s0 - d0)
#ubMultiMap = (d0)[s0] -> (s0, d0 + 10)

// CHECK-LABEL: cfgfunc @loop_min_max(%arg0: index) {
// CHECK-NEXT:   %{{[0-9]+}} = affine_apply [[map0]]()
// CHECK-NEXT:   %{{[0-9]+}} = affine_apply [[map42]]()
// CHECK-NEXT:   br ^bb1(%{{[0-9]+}} : index)
// CHECK-NEXT: ^bb1(%{{[0-9]+}}: index):	// 2 preds: ^bb0, ^bb5
// CHECK-NEXT:   %{{[0-9]+}} = cmpi "slt", %{{[0-9]+}}, %{{[0-9]+}} : index
// CHECK-NEXT:   cond_br %{{[0-9]+}}, ^bb2, ^bb6
// CHECK-NEXT: ^bb2:	// pred: ^bb1
// CHECK-NEXT:   %[[lb:[0-9]+]] = affine_apply [[multiMap1]](%{{[0-9]+}})[%arg0]
// CHECK-NEXT:   %[[lbc:[0-9]+]] = cmpi "sgt", %[[lb]]#0, %[[lb]]#1 : index
// CHECK-NEXT:   %[[lbv:[0-9]+]] = select %[[lbc]], %[[lb]]#0, %[[lb]]#1 : index
// CHECK-NEXT:   %[[ub:[0-9]+]] = affine_apply [[multiMap2]](%{{[0-9]+}})[%arg0]
// CHECK-NEXT:   %[[ubc:[0-9]+]] = cmpi "slt", %[[ub]]#0, %[[ub]]#1 : index
// CHECK-NEXT:   %[[ubv:[0-9]+]] = select %[[ubc]], %[[ub]]#0, %[[ub]]#1 : index
// CHECK-NEXT:   br ^bb3(%[[lbv]] : index)
// CHECK-NEXT: ^bb3(%{{[0-9]+}}: index):	// 2 preds: ^bb2, ^bb4
// CHECK-NEXT:   %{{[0-9]+}} = cmpi "slt", %{{[0-9]+}}, %[[ubv]] : index
// CHECK-NEXT:   cond_br %{{[0-9]+}}, ^bb4, ^bb5
// CHECK-NEXT: ^bb4:	// pred: ^bb3
// CHECK-NEXT:   call @body2(%{{[0-9]+}}, %{{[0-9]+}}) : (index, index) -> ()
// CHECK-NEXT:   %{{[0-9]+}} = affine_apply [[mapAdd1]](%{{[0-9]+}})
// CHECK-NEXT:   br ^bb3(%{{[0-9]+}} : index)
// CHECK-NEXT: ^bb5:	// pred: ^bb3
// CHECK-NEXT:   %{{[0-9]+}} = affine_apply [[mapAdd1]](%{{[0-9]+}})
// CHECK-NEXT:   br ^bb1(%{{[0-9]+}} : index)
// CHECK-NEXT: ^bb6:	// pred: ^bb1
// CHECK-NEXT:   return
// CHECK-NEXT: }
mlfunc @loop_min_max(%N : index) {
  for %i = 0 to 42 {
    for %j = max #lbMultiMap(%i)[%N] to min #ubMultiMap(%i)[%N] {
      call @body2(%i, %j) : (index, index) -> ()
    }
  }
  return
}

#map_7_values = (i) -> (i, i, i, i, i, i, i)

// Check that the "min" (cmpi "slt" + select) reduction sequence is emitted
// correctly for a an affine map with 7 results.

// CHECK-LABEL: cfgfunc @min_reduction_tree(%arg0: index) {
// CHECK-NEXT:   %{{[0-9]+}} = affine_apply [[map0]]()
// CHECK-NEXT:   %[[applr:[0-9]+]] = affine_apply [[multi7Map]](%arg0)
// CHECK-NEXT:   %[[c01:.+]] = cmpi "slt", %[[applr]]#0, %[[applr]]#1 : index
// CHECK-NEXT:   %[[r01:.+]] = select %[[c01]], %[[applr]]#0, %[[applr]]#1 : index
// CHECK-NEXT:   %[[c012:.+]] = cmpi "slt", %[[r01]], %[[applr]]#2 : index
// CHECK-NEXT:   %[[r012:.+]] = select %[[c012]], %[[r01]], %[[applr]]#2 : index
// CHECK-NEXT:   %[[c0123:.+]] = cmpi "slt", %[[r012]], %[[applr]]#3 : index
// CHECK-NEXT:   %[[r0123:.+]] = select %[[c0123]], %[[r012]], %[[applr]]#3 : index
// CHECK-NEXT:   %[[c01234:.+]] = cmpi "slt", %[[r0123]], %[[applr]]#4 : index
// CHECK-NEXT:   %[[r01234:.+]] = select %[[c01234]], %[[r0123]], %[[applr]]#4 : index
// CHECK-NEXT:   %[[c012345:.+]] = cmpi "slt", %[[r01234]], %[[applr]]#5 : index
// CHECK-NEXT:   %[[r012345:.+]] = select %[[c012345]], %[[r01234]], %[[applr]]#5 : index
// CHECK-NEXT:   %[[c0123456:.+]] = cmpi "slt", %[[r012345]], %[[applr]]#6 : index
// CHECK-NEXT:   %[[r0123456:.+]] = select %[[c0123456]], %[[r012345]], %[[applr]]#6 : index
// CHECK-NEXT:   br ^bb1(%0 : index)
// CHECK-NEXT: ^bb1(%{{[0-9]+}}: index):	// 2 preds: ^bb0, ^bb2
// CHECK-NEXT:   %{{[0-9]+}} = cmpi "slt", %{{[0-9]+}}, %[[r0123456]] : index
// CHECK-NEXT:   cond_br %{{[0-9]+}}, ^bb2, ^bb3
// CHECK-NEXT: ^bb2:	// pred: ^bb1
// CHECK-NEXT:   call @body(%{{[0-9]+}}) : (index) -> ()
// CHECK-NEXT:   %{{[0-9]+}} = affine_apply [[mapAdd1]](%{{[0-9]+}})
// CHECK-NEXT:   br ^bb1(%{{[0-9]+}} : index)
// CHECK-NEXT: ^bb3:	// pred: ^bb1
// CHECK-NEXT:   return
// CHECK-NEXT: }
mlfunc @min_reduction_tree(%v : index) {
  for %i = 0 to min #map_7_values(%v)[] {
    call @body(%i) : (index) -> ()
  }
  return
}
