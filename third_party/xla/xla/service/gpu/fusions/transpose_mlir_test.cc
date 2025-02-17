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
#include "xla/service/gpu/fusions/transpose_mlir.h"

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

using MlirTransposeFusionTest = MlirEmitterTestBase<MlirTransposeFusion>;

TEST_F(MlirTransposeFusionTest, ThreadIndexing021) {
  TF_ASSERT_OK_AND_ASSIGN(auto module, ParseAndReturnVerifiedModule(R"(
    HloModule module

    fusion {
      %input = f32[100,32,64] parameter(0)
      ROOT transpose = f32[100,64,32] transpose(%input), dimensions={0,2,1}
    }
    ENTRY entry {
      %input = f32[100,32,64] parameter(0)
      ROOT %fusion = f32[100,64,32] fusion(%input), kind=kInput, calls=fusion
    }
  )"));

  auto* root = module->entry_computation()->root_instruction();
  auto analysis = AnalyzeFusion(*root, device_info_);

  MlirTransposeFusion fusion(analysis);
  EXPECT_THAT(
      fusion.ComputeThreadIdToInputIndexing(0, 0, &mlir_context_)->ToString(),
      MatchIndexingString(R"(
        (d0, d1, d2, d3, d4, d5)[s0, s1, s2] -> (
          d3 floordiv 2,
          d0 floordiv 32 + s1 * 4,
          (d3 mod 2) * 32 + d0 mod 32
        )
        domain:
        d0 in [0, 127]
        d1 in [0, 0]
        d2 in [0, 0]
        d3 in [0, 199]
        d4 in [0, 0]
        d5 in [0, 0]

        s0 in [0, 0]
        s1 in [0, 7]
        s2 in [0, 0]

        (d3 mod 2) * 32 + d0 mod 32 in [0, 63]
        d0 floordiv 32 + s1 * 4 in [0, 31]
      )"));
  EXPECT_THAT(
      fusion.ComputeThreadIdToOutputIndexing(0, &mlir_context_)->ToString(),
      MatchIndexingString(R"(
        (d0, d1, d2, d3, d4, d5)[s0, s1, s2] -> (
          d3 floordiv 2,
          (d3 mod 2) * 32 + d0 floordiv 32 + s1 * 4,
          d0 mod 32
        )
        domain:
        d0 in [0, 127]
        d1 in [0, 0]
        d2 in [0, 0]
        d3 in [0, 199]
        d4 in [0, 0]
        d5 in [0, 0]

        s0 in [0, 0]
        s1 in [0, 7]
        s2 in [0, 0]

        (d3 mod 2) * 32 + d0 floordiv 32 + s1 * 4 in [0, 63]
        d0 mod 32 in [0, 31]
      )"));
}

TEST_F(MlirTransposeFusionTest, ThreadIndexing201) {
  TF_ASSERT_OK_AND_ASSIGN(auto module, ParseAndReturnVerifiedModule(R"(
    HloModule module

    fusion {
      %input = f32[100,64,32] parameter(0)
      ROOT transpose = f32[32,100,64] transpose(%input), dimensions={2,0,1}
    }
    ENTRY entry {
      %input = f32[100,64,32] parameter(0)
      ROOT %fusion = f32[32,100,64] fusion(%input), kind=kInput, calls=fusion
    })"));

  auto* root = module->entry_computation()->root_instruction();
  auto analysis = AnalyzeFusion(*root, device_info_);
  MlirTransposeFusion fusion(analysis);

  EXPECT_THAT(
      fusion.ComputeThreadIdToInputIndexing(0, 0, &mlir_context_)->ToString(),
      MatchIndexingString(R"(
        (d0, d1, d2, d3, d4, d5)[s0, s1, s2] -> (
          (d3 * 32 + d0 floordiv 32 + s1 * 4) floordiv 64,
          (d3 * 32 + d0 floordiv 32 + s1 * 4) mod 64,
          d0 mod 32
        )
        domain:
        d0 in [0, 127]
        d1 in [0, 0]
        d2 in [0, 0]
        d3 in [0, 199]
        d4 in [0, 0]
        d5 in [0, 0]

        s0 in [0, 0]
        s1 in [0, 7]
        s2 in [0, 0]

        0 in [0, 0]
        d0 mod 32 in [0, 31]
        d3 * 32 + d0 floordiv 32 + s1 * 4 in [0, 6399]
      )"));
  EXPECT_THAT(
      fusion.ComputeThreadIdToOutputIndexing(0, &mlir_context_)->ToString(),
      MatchIndexingString(R"(
        (d0, d1, d2, d3, d4, d5)[s0, s1, s2] -> (
          d0 floordiv 32 + s1 * 4,
          (d3 * 32 + d0 mod 32) floordiv 64,
          (d3 * 32 + d0 mod 32) mod 64
        )
        domain:
        d0 in [0, 127]
        d1 in [0, 0]
        d2 in [0, 0]
        d3 in [0, 199]
        d4 in [0, 0]
        d5 in [0, 0]

        s0 in [0, 0]
        s1 in [0, 7]
        s2 in [0, 0]

        0 in [0, 0]
        d0 floordiv 32 + s1 * 4 in [0, 31]
        d3 * 32 + d0 mod 32 in [0, 6399]
      )"));
}

TEST_F(MlirTransposeFusionTest, FusedTranspose021) {
  auto kHloString = R"(
    HloModule Transpose

    %fused_computation {
      %p0 = f32[20,160,170] parameter(0)
      %exp = f32[20,160,170] exponential(%p0)
      %transpose = f32[20,170,160] transpose(%exp), dimensions={0,2,1}
      ROOT %abs = f32[20,170,160] abs(%transpose)
    }
    ENTRY main {
      %param = f32[20,160,170] parameter(0)
      ROOT %fusion = f32[20,170,160] fusion(%param), kind=kInput,
        calls=%fused_computation
    }
  )";
  TF_ASSERT_OK(EmitAndCheckIR(kHloString, R"(
    // CHECK-LABEL: func.func @fused_computation(
    // CHECK-SAME:   }, %[[OUT:.*]]: tensor<20x170x160xf32>
    //
    // CHECK-DAG:  %[[C0:.*]] = arith.constant 0 : index
    // CHECK-DAG:  %[[C1:.*]] = arith.constant 1 : index
    // CHECK-DAG:  %[[C8:.*]] = arith.constant 8 : index

    // CHECK:      %[[SHMEM:.*]] = xla_gpu.allocate_shared : tensor<1x32x32xf32>
    // CHECK:      %[[SHMEM_WITH_VALS:.*]] = scf.for
    // CHECK-SAME:     %[[C0]] to %[[C8]] step %[[C1]]
    // CHECK-SAME:     iter_args(%[[SHMEM_:.*]] = %[[SHMEM]])
    // CHECK:        %[[EXP:.*]] = xla_gpu.pure_call @fused_computation_exp
    // CHECK:        tensor.insert %[[EXP]] into %[[SHMEM_]]

    // CHECK:      %[[SYNC:.*]] = xla_gpu.sync_threads %[[SHMEM_WITH_VALS]]

    // CHECK:      scf.for
    // CHECK-SAME:    %[[C0]] to %[[C8]] step %[[C1]]
    // CHECK-SAME:    iter_args(%[[OUT_:.*]] = %[[OUT]])
    // CHECK:       %[[ABS:.*]] = xla_gpu.pure_call @fused_computation_abs
    // CHECK:       tensor.insert %[[ABS]] into %[[OUT_]]
  )"));
  EXPECT_TRUE(RunAndCompareNoHloPasses(kHloString, ErrorSpec{1e-3}));
}

TEST_F(MlirTransposeFusionTest, Transpose021_Parameter) {
  auto kHloString = R"(
    HloModule Transpose

    %fused_computation {
      %p0 = f32[20,160,170] parameter(0)
      %transpose = f32[20,170,160] transpose(%p0), dimensions={0,2,1}
      ROOT %abs = f32[20,170,160] abs(%transpose)
    }
    ENTRY main {
      %param = f32[20,160,170] parameter(0)
      ROOT %fusion = f32[20,170,160] fusion(%param), kind=kInput,
        calls=%fused_computation
    }
  )";
  TF_ASSERT_OK(EmitAndCheckIR(kHloString, R"(
    // CHECK-LABEL: func.func @fused_computation(
    // CHECK-SAME:   }, %[[OUT:.*]]: tensor<20x170x160xf32>
    //
    // CHECK-DAG:  %[[C0:.*]] = arith.constant 0 : index
    // CHECK-DAG:  %[[C1:.*]] = arith.constant 1 : index
    // CHECK-DAG:  %[[C8:.*]] = arith.constant 8 : index

    // CHECK:      %[[SHMEM:.*]] = xla_gpu.allocate_shared : tensor<1x32x32xf32>
    // CHECK:      %[[SHMEM_WITH_VALS:.*]] = scf.for
    // CHECK-SAME:     %[[C0]] to %[[C8]] step %[[C1]]
    // CHECK-SAME:     iter_args(%[[SHMEM_:.*]] = %[[SHMEM]])
    // CHECK:        %[[EXP:.*]] = xla_gpu.pure_call @fused_computation_p0
    // CHECK:        tensor.insert %[[EXP]] into %[[SHMEM_]]

    // CHECK:      %[[SYNC:.*]] = xla_gpu.sync_threads %[[SHMEM_WITH_VALS]]

    // CHECK:      scf.for
    // CHECK-SAME:    %[[C0]] to %[[C8]] step %[[C1]]
    // CHECK-SAME:    iter_args(%[[OUT_:.*]] = %[[OUT]])
    // CHECK:       %[[ABS:.*]] = xla_gpu.pure_call @fused_computation_abs
    // CHECK:       tensor.insert %[[ABS]] into %[[OUT_]]
  )"));
  EXPECT_TRUE(RunAndCompareNoHloPasses(kHloString, ErrorSpec{1e-3}));
}

TEST_F(MlirTransposeFusionTest, Transpose021_NoEpilogue) {
  auto kHloString = R"(
    HloModule Transpose

    %fused_computation {
      %p0 = f32[20,160,170] parameter(0)
      ROOT %transpose = f32[20,170,160] transpose(%p0), dimensions={0,2,1}
    }
    ENTRY main {
      %param = f32[20,160,170] parameter(0)
      ROOT %fusion = f32[20,170,160] fusion(%param), kind=kInput,
        calls=%fused_computation
    }
  )";
  TF_ASSERT_OK(EmitAndCheckIR(kHloString, R"(
    // CHECK-LABEL: func.func @fused_computation(
    // CHECK-SAME:   }, %[[OUT:.*]]: tensor<20x170x160xf32>
    //
    // CHECK-DAG:  %[[C0:.*]] = arith.constant 0 : index
    // CHECK-DAG:  %[[C1:.*]] = arith.constant 1 : index
    // CHECK-DAG:  %[[C8:.*]] = arith.constant 8 : index

    // CHECK:      %[[SHMEM:.*]] = xla_gpu.allocate_shared : tensor<1x32x32xf32>
    // CHECK:      %[[SHMEM_WITH_VALS:.*]] = scf.for
    // CHECK-SAME:     %[[C0]] to %[[C8]] step %[[C1]]
    // CHECK-SAME:     iter_args(%[[SHMEM_:.*]] = %[[SHMEM]])
    // CHECK:        %[[EXP:.*]] = xla_gpu.pure_call @fused_computation_p0
    // CHECK:        tensor.insert %[[EXP]] into %[[SHMEM_]]

    // CHECK:      %[[SYNC:.*]] = xla_gpu.sync_threads %[[SHMEM_WITH_VALS]]

    // CHECK:      scf.for
    // CHECK-SAME:    %[[C0]] to %[[C8]] step %[[C1]]
    // CHECK-SAME:    iter_args(%[[OUT_:.*]] = %[[OUT]])
    // CHECK:       %[[SHMEM_ELEM:.*]] = tensor.extract %[[SYNC]]
    // CHECK:       tensor.insert %[[SHMEM_ELEM]] into %[[OUT_]]
  )"));
  EXPECT_TRUE(RunAndCompareNoHloPasses(kHloString, ErrorSpec{1e-3}));
}

}  // namespace
}  // namespace gpu
}  // namespace xla
