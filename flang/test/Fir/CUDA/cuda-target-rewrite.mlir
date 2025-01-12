// RUN: fir-opt --target-rewrite %s | FileCheck %s

gpu.module @testmod {
  gpu.func @_QPvcpowdk(%arg0: !fir.ref<complex<f64>> {cuf.data_attr = #cuf.cuda<device>, fir.bindc_name = "a"}) attributes {cuf.proc_attr = #cuf.cuda_proc<global>} {
    %0 = fir.alloca i64
    %1 = fir.load %0 : !fir.ref<i64>
    %2 = fir.load %arg0 : !fir.ref<complex<f64>>
    %3 = fir.call @_FortranAzpowk(%2, %1) fastmath<contract> : (complex<f64>, i64) -> complex<f64>
    gpu.return
  }
  func.func private @_FortranAzpowk(complex<f64>, i64) -> complex<f64> attributes {fir.bindc_name = "_FortranAzpowk", fir.runtime}
}

// CHECK-LABEL: gpu.func @_QPvcpowdk
// CHECK: %{{.*}} = fir.call @_FortranAzpowk(%{{.*}}, %{{.*}}, %{{.*}}) : (f64, f64, i64) -> tuple<f64, f64>
// CHECK: func.func private @_FortranAzpowk(f64, f64, i64) -> tuple<f64, f64> attributes {fir.bindc_name = "_FortranAzpowk", fir.runtime}
