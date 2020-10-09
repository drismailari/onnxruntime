// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "matmul.h"
#include "core/providers/cpu/math/matmul_helper.h"
#include "core/providers/hip/shared_inc/fpgeneric.h"
#include "core/providers/hip/hip_allocator.h"

namespace onnxruntime {
namespace hip {

#define REGISTER_KERNEL_TYPED(T)                                  \
  ONNX_OPERATOR_VERSIONED_TYPED_KERNEL_EX(                        \
      MatMul,                                                     \
      kOnnxDomain,                                                \
      1, 8,                                                       \
      T,                                                          \
      kHipExecutionProvider,                                     \
      KernelDefBuilder()                                          \
          .TypeConstraint("T", DataTypeImpl::GetTensorType<T>()), \
      MatMul<T>);                                                 \
  ONNX_OPERATOR_TYPED_KERNEL_EX(                                  \
      MatMul,                                                     \
      kOnnxDomain,                                                \
      9,                                                          \
      T,                                                          \
      kHipExecutionProvider,                                     \
      KernelDefBuilder()                                          \
          .TypeConstraint("T", DataTypeImpl::GetTensorType<T>()), \
      MatMul<T>);

REGISTER_KERNEL_TYPED(float)
REGISTER_KERNEL_TYPED(double)
REGISTER_KERNEL_TYPED(MLFloat16)

// StridedBatchedGemm can be used for the following GEMM computation
// C[pnm] = A[pnk]*B[km] or C[pnm] = A[pnk]*B[pkm]
static bool CanUseStridedBatchedGemm(const TensorShape& left_shape, const TensorShape& right_shape,
                                     bool transa, bool transb,
                                     int64_t& stride_A, int64_t& stride_B, int64_t& stride_C, int64_t& batch_count) {
  size_t left_num_dims = left_shape.NumDimensions();
  size_t right_num_dims = right_shape.NumDimensions();

  if (!(left_num_dims >= 3 && right_num_dims >= 2)) {
    return false;
  }

  int64_t left_p = left_shape.SizeToDimension(left_num_dims - 2);
  int64_t left_k = transa ? left_shape[left_num_dims - 2] : left_shape[left_num_dims - 1];

  if (right_num_dims >= 3) {
    int64_t right_p = right_shape.SizeToDimension(right_num_dims - 2);
    if (left_p != right_p) {
      return false;
    }
  }

  int64_t right_k = transb ? right_shape[right_num_dims - 1] : right_shape[right_num_dims - 2];
  if (left_k != right_k) {
    return false;
  }

  int64_t n = transa ? left_shape[left_num_dims - 1] : left_shape[left_num_dims - 2];
  int64_t m = transb ? right_shape[right_num_dims - 2] : right_shape[right_num_dims - 1];
  stride_A = n * left_k;
  stride_B = right_num_dims == 2 ? 0 : right_k * m;
  stride_C = n * m;
  batch_count = left_p;
  return true;
}

template <typename T>
Status MatMul<T>::ComputeInternal(OpKernelContext* ctx) const {
  typedef typename ToHipType<T>::MappedType HipT;

  const Tensor* left_X = ctx->Input<Tensor>(0);
  const Tensor* right_X = ctx->Input<Tensor>(1);

  // Ignore the transpose flag if rank of input being 1.
  // Be noted: numpy.transpose on vector does not change anything.
  bool transa = trans_A_;
  bool transb = trans_B_;
  if (left_X->Shape().NumDimensions() == 1) {
    transa = false;
  }
  if (right_X->Shape().NumDimensions() == 1) {
    transb = false;
  }

  MatMulComputeHelper helper;
  ORT_RETURN_IF_ERROR(helper.Compute(left_X->Shape(), right_X->Shape(), transa, transb));

  Tensor* Y = ctx->Output(0, helper.OutputShape());

  // Bail out early if the output is going to be empty
  if (Y->Shape().Size() == 0)
    return Status::OK();

  const HipT alpha = ToHipType<T>::FromFloat(alpha_);
  const HipT zero = ToHipType<T>::FromFloat(0.0f);

  // hipblasOperation_t transA = transa ? HIPBLAS_OP_T : HIPBLAS_OP_N;
  // hipblasOperation_t transB = transb ? HIPBLAS_OP_T : HIPBLAS_OP_N;
  rocblas_operation transA = transa ? rocblas_operation_transpose : rocblas_operation_none;
  rocblas_operation transB = transb ? rocblas_operation_transpose : rocblas_operation_none;
  const int lda = transa ? static_cast<int>(helper.M()) : static_cast<int>(helper.K());
  const int ldb = transb ? static_cast<int>(helper.K()) : static_cast<int>(helper.N());
  const int ldc = static_cast<int>(helper.N());
  int64_t stride_A, stride_B, stride_C, batch_count;

  if (helper.OutputOffsets().size() == 1) {
    // HIPBLAS_RETURN_IF_ERROR(hipblasGemmHelper(
    //     Base::HipblasHandle(),
    //     transB,
    //     transA,
    //     static_cast<int>(helper.N()),
    //     static_cast<int>(helper.M()),
    //     static_cast<int>(helper.K()),
    //     &one,
    //     reinterpret_cast<const HipT*>(right_X->template Data<T>()),
    //     ldb,
    //     reinterpret_cast<const HipT*>(left_X->template Data<T>()),
    //     lda,
    //     &zero,
    //     reinterpret_cast<HipT*>(Y->template MutableData<T>()),
    //     ldc));
    ROCBLAS_RETURN_IF_ERROR(rocblasGemmHelper(
        Base::RocblasHandle(),
        transB,
        transA,
        static_cast<int>(helper.N()),
        static_cast<int>(helper.M()),
        static_cast<int>(helper.K()),
        &alpha,
        reinterpret_cast<const HipT*>(right_X->template Data<T>()),
        ldb,
        reinterpret_cast<const HipT*>(left_X->template Data<T>()),
        lda,
        &zero,
        reinterpret_cast<HipT*>(Y->template MutableData<T>()),
        ldc));
    return Status::OK();
  } else if (CanUseStridedBatchedGemm(left_X->Shape(), right_X->Shape(),
                                      transa, transb, stride_A, stride_B, stride_C, batch_count)) {
    // HIPBLAS_RETURN_IF_ERROR(hipblasGemmStridedBatchedHelper(Base::HipblasHandle(),
    //                                                       transB,
    //                                                       transA,
    //                                                       static_cast<int>(helper.N()),
    //                                                       static_cast<int>(helper.M()),
    //                                                       static_cast<int>(helper.K()),
    //                                                       &one,
    //                                                       reinterpret_cast<const HipT*>(right_X->template Data<T>()),
    //                                                       ldb,
    //                                                       stride_B,
    //                                                       reinterpret_cast<const HipT*>(left_X->template Data<T>()),
    //                                                       lda,
    //                                                       stride_A,
    //                                                       &zero,
    //                                                       reinterpret_cast<HipT*>(Y->template MutableData<T>()),
    //                                                       ldc,
    //                                                       stride_C,
    //                                                       static_cast<int>(batch_count)));
    ROCBLAS_RETURN_IF_ERROR(rocblasGemmStridedBatchedHelper(Base::RocblasHandle(),
                                                          transB,
                                                          transA,
                                                          static_cast<int>(helper.N()),
                                                          static_cast<int>(helper.M()),
                                                          static_cast<int>(helper.K()),
                                                          &alpha,
                                                          reinterpret_cast<const HipT*>(right_X->template Data<T>()),
                                                          ldb,
                                                          stride_B,
                                                          reinterpret_cast<const HipT*>(left_X->template Data<T>()),
                                                          lda,
                                                          stride_A,
                                                          &zero,
                                                          reinterpret_cast<HipT*>(Y->template MutableData<T>()),
                                                          ldc,
                                                          stride_C,
                                                          static_cast<int>(batch_count)));
    return Status::OK();
  }

  HipAsyncBuffer<const HipT*> left_arrays(this, helper.LeftOffsets().size());
  HipAsyncBuffer<const HipT*> right_arrays(this, helper.RightOffsets().size());
  HipAsyncBuffer<HipT*> output_arrays(this, helper.OutputOffsets().size());
  MatMulComputeHelper::OffsetToArrays(reinterpret_cast<const HipT*>(left_X->template Data<T>()), helper.LeftOffsets(), left_arrays.CpuSpan());
  MatMulComputeHelper::OffsetToArrays(reinterpret_cast<const HipT*>(right_X->template Data<T>()), helper.RightOffsets(), right_arrays.CpuSpan());
  MatMulComputeHelper::OffsetToArrays(reinterpret_cast<HipT*>(Y->template MutableData<T>()), helper.OutputOffsets(), output_arrays.CpuSpan());
  ORT_RETURN_IF_ERROR(left_arrays.CopyToGpu());
  ORT_RETURN_IF_ERROR(right_arrays.CopyToGpu());
  ORT_RETURN_IF_ERROR(output_arrays.CopyToGpu());

  // note that onnxruntime OrtValue is row major, while hipblas is column major,
  // so swap left/right operands
  // HIPBLAS_RETURN_IF_ERROR(hipblasGemmBatchedHelper(
  //     Base::HipblasHandle(),
  //     transB,
  //     transA,
  //     static_cast<int>(helper.N()),
  //     static_cast<int>(helper.M()),
  //     static_cast<int>(helper.K()),
  //     &one,
  //     right_arrays.GpuPtr(),
  //     ldb,
  //     left_arrays.GpuPtr(),
  //     lda,
  //     &zero,
  //     output_arrays.GpuPtr(),
  //     ldc,
  //     static_cast<int>(helper.OutputOffsets().size())));
  ROCBLAS_RETURN_IF_ERROR(rocblasGemmBatchedHelper(
      Base::RocblasHandle(),
      transB,
      transA,
      static_cast<int>(helper.N()),
      static_cast<int>(helper.M()),
      static_cast<int>(helper.K()),
      &alpha,
      right_arrays.GpuPtr(),
      ldb,
      left_arrays.GpuPtr(),
      lda,
      &zero,
      output_arrays.GpuPtr(),
      ldc,
      static_cast<int>(helper.OutputOffsets().size())));
  return Status::OK();
}

}  // namespace hip
}  // namespace onnxruntime