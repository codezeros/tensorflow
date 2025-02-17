/* Copyright 2024 The OpenXLA Authors.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/
#include "xla/service/gpu/fusions/loop_mlir.h"

#include <gtest/gtest.h>
#include "xla/error_spec.h"
#include "xla/service/gpu/fusions/mlir_emitter_test_base.h"
#include "xla/service/gpu/hlo_fusion_analysis.h"
#include "xla/service/gpu/model/indexing_test_utils.h"
#include "tsl/lib/core/status_test_util.h"
#include "tsl/platform/statusor.h"

namespace xla {
namespace gpu {
namespace {

using MlirLoopFusionTest = MlirEmitterTestBase<MlirLoopFusion>;

TEST_F(MlirLoopFusionTest, ThreadId_IndexingUnrolled) {
  TF_ASSERT_OK_AND_ASSIGN(auto module, ParseAndReturnVerifiedModule(R"(
    HloModule module

    neg {
      %input = f32[100,200,300] parameter(0)
      ROOT neg = f32[100,200,300] negate(%input)
    }
    ENTRY entry {
      %input = f32[100,200,300] parameter(0)
      ROOT %fusion = f32[100,200,300] fusion(%input), kind=kLoop, calls=neg
    }
  )"));
  thread_id_printer_.SetSymbolName(0, "chunk_id");
  thread_id_printer_.SetSymbolName(1, "unroll_id");

  auto* root = module->entry_computation()->root_instruction();
  auto analysis = AnalyzeFusion(*root, device_info_);
  MlirLoopFusion fusion(analysis);
  auto thread_id_to_output_indexing =
      fusion.ComputeThreadIdToOutputIndexing(/*root_index=*/0, &mlir_context_);

  EXPECT_THAT(thread_id_to_output_indexing->ToString(thread_id_printer_),
              MatchIndexingString(R"(
  (th_x, th_y, th_z, bl_x, bl_y, bl_z)[chunk_id, unroll_id] -> (
   (((bl_x * 16 + th_x floordiv 8) floordiv 3 + chunk_id * 5376) floordiv 625) mod 100,
   (((th_x + bl_x * 128) floordiv 3 + chunk_id * 43008) floordiv 25) mod 200,
   th_x * 4 + bl_x * 512 + chunk_id * 516096 + unroll_id -
     (((th_x + bl_x * 128) floordiv 3 + chunk_id * 43008) floordiv 25) * 300
  )
  domain:
  th_x in [0, 127]
  th_y in [0, 0]
  th_z in [0, 0]
  bl_x in [0, 1007]
  bl_y in [0, 0]
  bl_z in [0, 0]
  chunk_id in [0, 11]
  unroll_id in [0, 3]
  (th_x + bl_x * 128) * 4 + chunk_id * 516096 in [0, 5999996]
)"));
}

TEST_F(MlirLoopFusionTest, ThreadId_IndexingNotUnrolled) {
  TF_ASSERT_OK_AND_ASSIGN(auto module, ParseAndReturnVerifiedModule(R"(
    HloModule module

    neg {
      %input = f32[20] parameter(0)
      ROOT neg = f32[20] negate(%input)
    }
    ENTRY entry {
      %input = f32[20] parameter(0)
      ROOT %fusion = f32[20] fusion(%input), kind=kLoop, calls=neg
    }
  )"));
  thread_id_printer_.SetSymbolName(0, "chunk_id");
  thread_id_printer_.SetSymbolName(1, "unroll_id");

  auto* root = module->entry_computation()->root_instruction();
  auto analysis = AnalyzeFusion(*root, device_info_);

  MlirLoopFusion fusion(analysis);
  auto thread_id_to_output_indexing =
      fusion.ComputeThreadIdToOutputIndexing(/*root_index=*/0, &mlir_context_);
  EXPECT_THAT(thread_id_to_output_indexing->ToString(thread_id_printer_),
              MatchIndexingString(R"(
              (th_x, th_y, th_z, bl_x, bl_y, bl_z)[chunk_id, unroll_id] -> (th_x)
              domain:
              th_x in [0, 19]
              th_y in [0, 0]
              th_z in [0, 0]
              bl_x in [0, 0]
              bl_y in [0, 0]
              bl_z in [0, 0]
              chunk_id in [0, 0]
              unroll_id in [0, 0]
            )"));
  auto thread_id_to_input_indexing = fusion.ComputeThreadIdToInputIndexing(
      /*root_index=*/0, /*hero_operand_index=*/0, &mlir_context_);
  EXPECT_THAT(thread_id_to_input_indexing->ToString(thread_id_printer_),
              MatchIndexingString(R"(
              (th_x, th_y, th_z, bl_x, bl_y, bl_z)[chunk_id, unroll_id] -> (th_x)
              domain:
              th_x in [0, 19]
              th_y in [0, 0]
              th_z in [0, 0]
              bl_x in [0, 0]
              bl_y in [0, 0]
              bl_z in [0, 0]
              chunk_id in [0, 0]
              unroll_id in [0, 0]
            )"));
}

TEST_F(MlirLoopFusionTest, ThreadId_Broadcast) {
  TF_ASSERT_OK_AND_ASSIGN(auto module, ParseAndReturnVerifiedModule(R"(
    HloModule module

    bcast {
      %input = f32[20] parameter(0)
      ROOT bcast = f32[10, 20, 30] broadcast(%input), dimensions={1}
    }
    ENTRY entry {
      %input = f32[20] parameter(0)
      ROOT %fusion = f32[10, 20, 30] fusion(%input), kind=kLoop, calls=bcast
    }
  )"));
  thread_id_printer_.SetSymbolName(0, "chunk_id");
  thread_id_printer_.SetSymbolName(1, "unroll_id");

  auto* root = module->entry_computation()->root_instruction();
  auto analysis = AnalyzeFusion(*root, device_info_);

  MlirLoopFusion fusion(analysis);
  auto thread_id_to_output_indexing =
      fusion.ComputeThreadIdToOutputIndexing(/*root_index=*/0, &mlir_context_);
  EXPECT_THAT(thread_id_to_output_indexing->ToString(thread_id_printer_),
              MatchIndexingString(R"(
              (th_x, th_y, th_z, bl_x, bl_y, bl_z)[chunk_id, unroll_id] -> (
                ((bl_x * 16 + th_x floordiv 8) floordiv 75) mod 10,
                ((bl_x * 64 + th_x floordiv 2) floordiv 15) mod 20,
                (th_x + bl_x * 128) mod 30)
                domain:
                th_x in [0, 127]
                th_y in [0, 0]
                th_z in [0, 0]
                bl_x in [0, 46]
                bl_y in [0, 0]
                bl_z in [0, 0]
                chunk_id in [0, 0]
                unroll_id in [0, 0]
                th_x + bl_x * 128 in [0, 5999]
            )"));
  auto thread_id_to_input_indexing = fusion.ComputeThreadIdToInputIndexing(
      /*root_index=*/0, /*hero_operand_index=*/0, &mlir_context_);
  EXPECT_THAT(thread_id_to_input_indexing->ToString(thread_id_printer_),
              MatchIndexingString(R"(
              (th_x, th_y, th_z, bl_x, bl_y, bl_z)[chunk_id, unroll_id] -> (
                ((bl_x * 64 + th_x floordiv 2) floordiv 15) mod 20)
                domain:
                th_x in [0, 127]
                th_y in [0, 0]
                th_z in [0, 0]
                bl_x in [0, 46]
                bl_y in [0, 0]
                bl_z in [0, 0]
                chunk_id in [0, 0]
                unroll_id in [0, 0]
                th_x + bl_x * 128 in [0, 5999]
            )"));
}

TEST_F(MlirLoopFusionTest, NoCodeDuplication) {
  // This test HLO is copied from
  // xla/service/fusion_node_indexing_evaluation_test.cc.
  auto kHloString = R"(
    HloModule test_module

    %fused_computation (param: f32[6]) -> f32[2] {
      %param = f32[6]{0} parameter(0)
      %slice0.1 = f32[5]{0} slice(f32[6]{0} %param), slice={[0:5]}
      %slice0.2 = f32[5]{0} slice(f32[6]{0} %param), slice={[1:6]}
      %add0 = f32[5]{0} add(f32[5]{0} %slice0.1, f32[5]{0} %slice0.2)
      %slice1.1 = f32[4]{0} slice(f32[5]{0} %add0), slice={[0:4]}
      %slice1.2 = f32[4]{0} slice(f32[5]{0} %add0), slice={[1:5]}
      %add1 = f32[4]{0} add(f32[4]{0} %slice1.1, f32[4]{0} %slice1.2)
      %slice2.1 = f32[3]{0} slice(f32[4]{0} %add1), slice={[0:3]}
      %slice2.2 = f32[3]{0} slice(f32[4]{0} %add1), slice={[1:4]}
      %add2 = f32[3]{0} add(f32[3]{0} %slice2.1, f32[3]{0} %slice2.2)
      %slice3.1 = f32[2]{0} slice(f32[3]{0} %add2), slice={[0:2]}
      %slice3.2 = f32[2]{0} slice(f32[3]{0} %add2), slice={[1:3]}
      ROOT %add3 = f32[2]{0} add(f32[2]{0} %slice3.1, f32[2]{0} %slice3.2)
    }
    ENTRY entry_computation {
      p0 = f32[] parameter(0)
      add = f32[] add(p0, p0)
      broadcast = f32[6]{0} broadcast(add), dimensions={}
      ROOT %fusion = f32[2]{0} fusion(broadcast), kind=kLoop, calls=%fused_computation
    }
  )";
  TF_ASSERT_OK(EmitAndCheckIR(kHloString, R"(
    // CHECK-COUNT-4: arith.add
    // CHECK-NOT: arith.add
  )"));
  EXPECT_TRUE(RunAndCompareNoHloPasses(kHloString, ErrorSpec{1e-3}));
}

TEST_F(MlirLoopFusionTest, TwoUsersConsistentIndexing) {
  auto kHloString = R"(
    HloModule test_module

    %fused_computation (param: f32[6]) -> f32[2] {
      %p0 = f32[2]{0} parameter(0)
      %p1 = f32[2]{0} parameter(1)
      %add = f32[2] add(%p0, %p1)
      %sub = f32[2] subtract(%p0, %p1)
      %mul = f32[2] multiply(%add, %sub)
      %div = f32[2] divide(%add, %sub)
      ROOT %atan2 = f32[2] atan2(%mul, %div)
    }
    ENTRY entry_computation {
      p0 = f32[2] parameter(0)
      p1 = f32[2] parameter(1)
      ROOT %fusion = f32[2] fusion(p0, p1), kind=kLoop, calls=%fused_computation
    }
  )";
  TF_ASSERT_OK(EmitAndCheckIR(kHloString, R"(
    // CHECK: func.func @fused_computation
    // CHECK-NEXT: gpu.thread_id
    // CHECK-NEXT: pure_call @fused_computation_atan2
    // CHECK-NEXT: tensor.insert
    // CHECK-NEXT: return

    // CHECK: func.func private @fused_computation_atan2
    // CHECK-NEXT: xla_gpu.pure_call
    // CHECK-NEXT: xla_gpu.pure_call
    // CHECK-NEXT: addf
    // CHECK-NEXT: subf
    // CHECK-NEXT: mulf
    // CHECK-NEXT: divf
    // CHECK-NEXT: atan2
    // CHECK-NEXT: return
  )"));
  EXPECT_TRUE(RunAndCompareNoHloPasses(kHloString, ErrorSpec{1e-3}));
}

TEST_F(MlirLoopFusionTest, ComplexOps) {
  auto kHloString = R"(
    HloModule test_module

    %fused_computation {
      %p0 = f32[2]{0} parameter(0)
      %p1 = f32[2]{0} parameter(1)
      %p2 = c64[2]{0} parameter(2)
      %complex = c64[2] complex(%p0, %p1)
      ROOT %add = c64[2] add(%complex, %p2)
    }
    ENTRY entry_computation {
      p0 = f32[2] parameter(0)
      p1 = f32[2] parameter(1)
      p2 = c64[2] parameter(2)
      ROOT %fusion = c64[2] fusion(p0, p1, p2), kind=kLoop, calls=%fused_computation
    }
  )";
  TF_ASSERT_OK(EmitAndCheckIR(kHloString, R"(
    // CHECK: func.func @fused_computation
    // CHECK-NEXT: gpu.thread_id
    // CHECK-NEXT: pure_call @fused_computation_add
    // CHECK-NEXT: tensor.insert
    // CHECK-NEXT: return

    // CHECK: func.func private @fused_computation_add
    // CHECK-NEXT: tensor.extract
    // CHECK-NEXT: tensor.extract
    // CHECK-NEXT: complex.create
    // CHECK-NEXT: tensor.extract
    // CHECK-NEXT: complex.add
  )"));
  EXPECT_TRUE(RunAndCompareNoHloPasses(kHloString, ErrorSpec{1e-3}));
}

TEST_F(MlirLoopFusionTest, IotaCopyBitcastBroadcastReshapeReverseTranspose) {
  auto kHloString = R"(
    HloModule test_module

    %fused_computation {
      %iota = f32[10,20,30] iota(), iota_dimension=2
      %copy = f32[10,20,30] copy(%iota)
      %bitcast = s32[10,20,30] bitcast(%copy)
      %broadcast = s32[2,10,3,20,5,30,7] broadcast(%bitcast),
        dimensions={1,3,5}
      %reshape = s32[20,60,150,7] reshape(%broadcast)
      %reverse = s32[20,60,150,7] reverse(%reshape), dimensions={2,3}
      ROOT %transpose = s32[60,20,7,150] transpose(%reverse),
        dimensions={1,0,3,2}
    }
    ENTRY entry_computation {
      ROOT %fusion = s32[60,20,7,150] fusion(),
        kind=kLoop, calls=%fused_computation
    }
  )";
  TF_ASSERT_OK(EmitAndCheckIR(kHloString, R"(
    // CHECK-COUNT-2: func.func
    // CHECK-NOT:     func.func
  )"));
  EXPECT_TRUE(RunAndCompareNoHloPasses(kHloString, ErrorSpec{1e-3}));
}

TEST_F(MlirLoopFusionTest, VariadicReduce) {
  auto kHloString = R"(
    HloModule Test, is_scheduled=true

    Add {
      scalar_lhs.0 = f32[] parameter(0)
      scalar_rhs.0 = f32[] parameter(1)
      scalar_lhs.1 = f32[] parameter(2)
      scalar_rhs.1 = f32[] parameter(3)
      add.0 = f32[] add(scalar_lhs.0, scalar_lhs.1)
      add.1 = f32[] add(scalar_rhs.0, scalar_rhs.1)
      ROOT t = (f32[], f32[]) tuple(add.0, add.1)
    }
    fused_computation {
      param_0 = f32[5,200,300]{2,1,0} parameter(0)
      param_1 = f32[5,200,300]{2,1,0} parameter(1)
      param_2 = f32[] parameter(2)
      ROOT d.1 = (f32[200], f32[200]) reduce(f32[5,200,300]{2,1,0} param_0,
          f32[5,200,300]{2,1,0} %param_1, f32[] param_2, f32[] param_2),
          dimensions={0,2}, to_apply=Add
    }
    ENTRY main {
      a = f32[5, 200, 300]{2,1,0} parameter(0)
      b = f32[5, 200, 300]{2,1,0} parameter(1)
      c = f32[] constant(0)
      ROOT fusion = (f32[200]{0}, f32[200]{0}) fusion(f32[5,200,300]{2,1,0} a,
        f32[5,200,300]{2,1,0} b, f32[] c), kind=kLoop, calls=fused_computation
    }
  )";
  TF_ASSERT_OK(EmitAndCheckIR(kHloString, R"(
    // CHECK: #[[MAP:.*]] = affine_map<()[s0, s1] -> ((s0 + s1 * 128) mod 200)>
    // CHECK: func @fused_computation(
    // CHECK:   %[[TID_X:.*]] = gpu.thread_id x
    // CHECK:   %[[BID_X:.*]] = gpu.block_id x
    // CHECK:   %[[IDX:.*]] = affine.apply #[[MAP]]()[%[[TID_X]], %[[BID_X]]]
    // CHECK:   %[[SCALARS_0:.*]], %[[SCALARS_1:.*]] = xla_gpu.pure_call @fused_computation_d_1
    // CHECK:   %[[INSERTED_1:.*]] = tensor.insert %[[SCALARS_0]] into %{{.*}}[%[[IDX]]]
    // CHECK:   %[[INSERTED_2:.*]] = tensor.insert %[[SCALARS_1]] into %{{.*}}[%[[IDX]]]
    // CHECK:   yield %[[INSERTED_1]], %[[INSERTED_2]]

    // CHECK: func private @fused_computation_d_1
    // CHECK:   %[[RET:.*]]:2 = func.call @Add_t
    // CHECK:   yield %[[RET]]#0, %[[RET]]#1
  )"));
  EXPECT_TRUE(RunAndCompareNoHloPasses(kHloString, ErrorSpec{1e-3}));
}

TEST_F(MlirLoopFusionTest, MinimumMaximum) {
  auto kHloString = R"(
    HloModule Test

    fused_computation {
      param0 = f64[] parameter(0)
      param1 = f64[] parameter(1)

      minimum = f64[] minimum(f64[] param0, f64[] param1)
      maximum = f64[] maximum(f64[] param0, f64[] param1)
      ROOT tuple = (f64[], f64[]) tuple(minimum, maximum)
    }

    ENTRY main {
      param0 = f64[] parameter(0)
      param1 = f64[] parameter(1)
      ROOT fusion = (f64[], f64[]) fusion(f64[] param0, f64[] param1), kind=kLoop, calls=fused_computation
    }
  )";
  TF_ASSERT_OK(EmitAndCheckIR(kHloString, R"(
    // CHECK-LABEL: fused_computation_tuple
    // CHECK:   arith.minimumf
    // CHECK:   arith.maximumf
  )"));
  EXPECT_TRUE(RunAndCompareNoHloPasses(kHloString, ErrorSpec{1e-3}));
}

}  // namespace
}  // namespace gpu
}  // namespace xla
