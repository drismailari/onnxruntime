#include "hip/hip_runtime.h"
#include <hip/hip_fp16.h>
#include "core/providers/rocm/cu_inc/common.cuh"
#include "mixed_precision_scale.h"

namespace onnxruntime {
namespace rocm {

template <typename SrcT, typename DstT>
__global__ void _MixedPrecisionScale(
    const SrcT* input_data,
    const float* scale_data,
    DstT* output_data,
    HIP_LONG N) {
  CALCULATE_ELEMENTWISE_INDEX_OR_EXIT(id, N);
  output_data[id] = static_cast<DstT>(*scale_data * static_cast<float>(input_data[id]));
}

template <typename SrcT, typename DstT>
void Impl_MixedPrecisionScale(
    const SrcT* input_data,
    const float* scale_data,
    DstT* output_data,
    size_t count){
  int blocksPerGrid = CeilDiv(count, GridDim::maxThreadsPerBlock);
  HIP_LONG N = static_cast<HIP_LONG>(count);
  hipLaunchKernelGGL(HIP_KERNEL_NAME(_MixedPrecisionScale<SrcT, DstT>), dim3(blocksPerGrid), dim3(GridDim::maxThreadsPerBlock), 0, 0, 
      input_data,
      scale_data,
      output_data,
      N);
}

#define SPECIALIZE_MIXEDPRECISIONSCALE_IMPL(SrcT, DstT) \
template void Impl_MixedPrecisionScale<SrcT, DstT>(     \
    const SrcT* input_data,                             \
    const float* scale_data,                            \
    DstT* output_data,                                  \
    size_t count);

SPECIALIZE_MIXEDPRECISIONSCALE_IMPL(half, half)
SPECIALIZE_MIXEDPRECISIONSCALE_IMPL(half, float)
SPECIALIZE_MIXEDPRECISIONSCALE_IMPL(float, half)
SPECIALIZE_MIXEDPRECISIONSCALE_IMPL(float, float)

}  // namespace rocm
}  // namespace onnxruntime