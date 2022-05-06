// for non-quantized implementation of sigmoid, it appears to be in the UnaryOps.cpp file rather than the Activation.cpp; here, we place it in the
// Activation.cpp file as it seems more appropriate

#include <ATen/ATen.h>
#include <ATen/NativeFunctions.h>
#include <ATen/quantized/Quantizer.h>
#include <ATen/native/TensorIterator.h>
#include <ATen/native/cpu/Loops.h>
#include <ATen/native/quantized/AffineQuantizer.h>
#include <ATen/native/quantized/cpu/QuantizedOps.h>
#include <ATen/native/quantized/cpu/InitQnnpack.h>
#include <ATen/native/quantized/cpu/QnnpackUtils.h>
#include <caffe2/utils/threadpool/pthreadpool-cpp.h>
#include <c10/util/irange.h>
#include <torch/library.h>

#include <algorithm>

namespace at {
namespace native {

DEFINE_DISPATCH(qsigmoid_stub);
DEFINE_DISPATCH(qhardsigmoid_stub);
DEFINE_DISPATCH(qhardswish_stub);
DEFINE_DISPATCH(qrelu_stub);
DEFINE_DISPATCH(qrelu_leaky_stub);

namespace {

#ifdef USE_PYTORCH_QNNPACK
Tensor qnnpack_sigmoid(
    Tensor input, double output_scale, int64_t output_zero_point) {
  TORCH_CHECK(input.ndimension() > 0, "qnnpack_sigmoid(): Got empty input tensor");
  TORCH_CHECK(input.scalar_type() == c10::kQUInt8,
               "qnnpack_sigmoid(): Expected input data type ",
               toString(c10::kQUInt8),
               " but got ",
               toString(input.scalar_type()));

  Tensor qy;
  initQNNPACK();

  Tensor input_contig = input.contiguous(input.suggest_memory_format());
  size_t num_elems = 1;
  for (const auto i : c10::irange(1, input_contig.ndimension())) {
    num_elems *= input_contig.size(i);
  }

  const auto zero_point = input_contig.q_zero_point();
  const auto scale = input_contig.q_scale();

  pytorch_qnnp_operator_t sigmoid_op{nullptr};
  const pytorch_qnnp_status createStatus = pytorch_qnnp_create_sigmoid_nc_q8(
    num_elems /* channels */,
    zero_point /* input zero point */,
    scale /* input scale */,
    output_zero_point /* output zero point */,
    // NOLINTNEXTLINE(cppcoreguidelines-narrowing-conversions,bugprone-narrowing-conversions)
    output_scale /* output scale */,
    std::numeric_limits<uint8_t>::min() /* output min */,
    std::numeric_limits<uint8_t>::max() /* output max */,
    0 /* flags */,
    &sigmoid_op);

  std::unique_ptr<pytorch_qnnp_operator, QnnpackOperatorDeleter>
      qnnpack_uniq_ptr(sigmoid_op);

  TORCH_INTERNAL_ASSERT(createStatus == pytorch_qnnp_status_success,
                        "failed to create QNNPACK sigmoid operator");
  qy = at::_empty_affine_quantized(
    input_contig.sizes(),
    at::device(kCPU).dtype(input_contig.dtype()),
    output_scale,
    output_zero_point,
    input_contig.suggest_memory_format());

  const pytorch_qnnp_status setupStatus = pytorch_qnnp_setup_sigmoid_nc_q8(
    sigmoid_op,
    input_contig.size(0) /* batch size */,
    (uint8_t*)input_contig.data_ptr<c10::quint8>() /* input data */,
    num_elems /* input stride */,
    (uint8_t*)qy.data_ptr<c10::quint8>() /* output data */,
    num_elems /* output stride */);
  TORCH_INTERNAL_ASSERT(setupStatus == pytorch_qnnp_status_success,
                        "failed to setup QNNPACK sigmoid operator");

  pthreadpool_t threadpool = caffe2::pthreadpool_();

  const pytorch_qnnp_status runStatus =
    pytorch_qnnp_run_operator(sigmoid_op, threadpool);

  TORCH_INTERNAL_ASSERT(
    runStatus == pytorch_qnnp_status_success,
    "failed to run QNNPACK sigmoid operator");
  return qy;
}

Tensor qnnpack_relu(Tensor input) {
  Tensor qy;
  TORCH_CHECK(
      input.ndimension() > 0, "qnnpack_relu(): Got empty input tensor");
  TORCH_CHECK(input.scalar_type() == c10::kQUInt8,
               "qnnpack_relu(): Expected input data type ",
               toString(c10::kQUInt8),
               " but got ",
               toString(input.scalar_type()));

  Tensor input_contig = input.contiguous(input.suggest_memory_format());

  const auto zero_point = input_contig.q_zero_point();

  initQNNPACK();

  size_t num_elems = 1;
  for (const auto i : c10::irange(1, input_contig.ndimension())) {
    num_elems *= input_contig.size(i);
  }

  pytorch_qnnp_operator_t qnnpack_operator{nullptr};

  const pytorch_qnnp_status createStatus = pytorch_qnnp_create_clamp_nc_u8(
      num_elems /* channels */,
      zero_point /* output min */,
      std::numeric_limits<uint8_t>::max() /* output max */,
      0 /* flags */,
      &qnnpack_operator);

  std::unique_ptr<pytorch_qnnp_operator, QnnpackOperatorDeleter>
      qnnpack_uniq_ptr(qnnpack_operator);

  TORCH_INTERNAL_ASSERT(
      createStatus == pytorch_qnnp_status_success,
      "failed to create QNNPACK Relu operator");

  qy = at::_empty_affine_quantized(
      input_contig.sizes(),
      at::device(kCPU).dtype(input.scalar_type()),
      input_contig.q_scale(),
      input_contig.q_zero_point(),
      input.suggest_memory_format());

  const pytorch_qnnp_status setupStatus = pytorch_qnnp_setup_clamp_nc_u8(
      qnnpack_operator, /* clamp */
      input_contig.size(0) /* batch size */,
      (uint8_t*)input_contig.data_ptr<c10::quint8>() /* input data */,
      num_elems /* input stride */,
      (uint8_t*)qy.data_ptr<c10::quint8>() /* output data */,
      num_elems /* output stride */);
  TORCH_INTERNAL_ASSERT(
      setupStatus == pytorch_qnnp_status_success,
      "failed to setup QNNPACK Relu operator");

  pthreadpool_t threadpool = caffe2::pthreadpool_();

  const pytorch_qnnp_status runStatus =
      pytorch_qnnp_run_operator(qnnpack_operator, threadpool);

  TORCH_INTERNAL_ASSERT(
      runStatus == pytorch_qnnp_status_success,
      "failed to run QNNPACK Relu operator");
  return qy;
}

Tensor qnnpack_hardsigmoid(Tensor input) {
  TORCH_CHECK(input.ndimension() > 0, "qnnpack_hardsigmoid(): Got empty input tensor");
  TORCH_CHECK(input.scalar_type() == c10::kQUInt8,
                "qnnpack_hardsigmoid(): Expected input data type ",
                toString(c10::kQUInt8),
                " but got ",
                toString(input.scalar_type()));
  initQNNPACK();

  Tensor input_contig = input.contiguous(input.suggest_memory_format());
  size_t num_elems = input_contig.numel() / input_contig.size(0);
  const auto i_zero_point = input_contig.q_zero_point();
  const auto i_scale = input_contig.q_scale();
  constexpr float o_scale = 1.0f / 256.0f;
  constexpr int32_t o_zero_point = 0;

  pytorch_qnnp_operator_t hardsigmoid_op{nullptr};
  const pytorch_qnnp_status createStatus = pytorch_qnnp_create_hardsigmoid_nc_q8(
    num_elems, // channels
    i_zero_point,
    i_scale,
    o_zero_point,
    o_scale,
    std::numeric_limits<uint8_t>::min(), // output min
    std::numeric_limits<uint8_t>::max(), // output max
    0, // flags
    &hardsigmoid_op);

  std::unique_ptr<pytorch_qnnp_operator, QnnpackOperatorDeleter>
      qnnpack_uniq_ptr(hardsigmoid_op);

  TORCH_INTERNAL_ASSERT(createStatus == pytorch_qnnp_status_success,
                        "failed to create QNNPACK Hardsigmoid operator");
  Tensor qy = at::_empty_affine_quantized(
    input_contig.sizes(),
    at::device(kCPU).dtype(input_contig.dtype()),
    o_scale,
    o_zero_point,
    input_contig.suggest_memory_format());

  const pytorch_qnnp_status setupStatus = pytorch_qnnp_setup_hardsigmoid_nc_q8(
    hardsigmoid_op,
    input_contig.size(0), // batch size
    (uint8_t*)input_contig.data_ptr<c10::quint8>(), // input data
    num_elems, // input stride
    (uint8_t*)qy.data_ptr<c10::quint8>(), // output data
    num_elems); // output stride
  TORCH_INTERNAL_ASSERT(setupStatus == pytorch_qnnp_status_success,
                        "failed to setup QNNPACK Hardsigmoid operator");

  pthreadpool_t threadpool = caffe2::pthreadpool_();

  const pytorch_qnnp_status runStatus =
    pytorch_qnnp_run_operator(hardsigmoid_op, threadpool);

  TORCH_INTERNAL_ASSERT(
    runStatus == pytorch_qnnp_status_success,
    "failed to run QNNPACK Hardsigmoid operator");
  return qy;
}

Tensor qnnpack_hardswish(const Tensor& qx, Tensor& qy) {
  TORCH_CHECK(qx.ndimension() > 0, "qnnpack_hardswish(): Got empty input tensor");
  TORCH_CHECK(qx.scalar_type() == c10::kQUInt8,
                "qnnpack_hardswish(): Expected input data type to be ",
                toString(c10::kQUInt8),
                " but got ",
                toString(qx.scalar_type()));
  initQNNPACK();

  size_t num_elems = qx.numel() / qx.size(0);
  const auto i_zero_point = qx.q_zero_point();
  const auto i_scale = qx.q_scale();
  const auto o_zero_point = qy.q_zero_point();
  const auto o_scale = qy.q_scale();

  pytorch_qnnp_operator_t hardswish_op{nullptr};
  const pytorch_qnnp_status createStatus = pytorch_qnnp_create_hardswish_nc_q8(
    num_elems, // channels
    i_zero_point,
    i_scale,
    o_zero_point,
    o_scale,
    std::numeric_limits<uint8_t>::min(), // output min
    std::numeric_limits<uint8_t>::max(), // output max
    0, // flags
    &hardswish_op);

  std::unique_ptr<pytorch_qnnp_operator, QnnpackOperatorDeleter>
      qnnpack_uniq_ptr(hardswish_op);

  TORCH_INTERNAL_ASSERT(createStatus == pytorch_qnnp_status_success,
                        "failed to create QNNPACK Hardswish operator");

  const pytorch_qnnp_status setupStatus = pytorch_qnnp_setup_hardswish_nc_q8(
    hardswish_op,
    qx.size(0), // batch size
    (uint8_t*)qx.data_ptr<c10::quint8>(), // input data
    num_elems, // input stride
    (uint8_t*)qy.data_ptr<c10::quint8>(), // output data
    num_elems); // output stride
  TORCH_INTERNAL_ASSERT(setupStatus == pytorch_qnnp_status_success,
                        "failed to setup QNNPACK Hardswish operator");

  pthreadpool_t threadpool = caffe2::pthreadpool_();

  const pytorch_qnnp_status runStatus =
    pytorch_qnnp_run_operator(hardswish_op, threadpool);

  TORCH_INTERNAL_ASSERT(
    runStatus == pytorch_qnnp_status_success,
    "failed to run QNNPACK Hardswish operator");
  return qy;
}
#endif // USE_PYTORCH_QNNPACK

} // namespace

// This ALWAYS outputs scale=1.0/256, dtype=quint8
// The zero_point is 0 for qint32 and quint8, but -128 for qint8.
Tensor sigmoid_quantized_cpu(const Tensor& qx) {
#ifdef USE_PYTORCH_QNNPACK
  if (at::globalContext().qEngine() == at::QEngine::QNNPACK &&
      qx.scalar_type() == kQUInt8) {
    constexpr double output_scale = 1.0f / 256.0f;
    constexpr int64_t output_zero_point = 0;
    return qnnpack_sigmoid(qx, output_scale, output_zero_point);
  }
#endif  // USE_PYTORCH_QNNPACK
  Tensor qy;
  AT_DISPATCH_QINT_TYPES(qx.scalar_type(), "qsigmoid", [&]() {
    // Naive implemenentation: uses dequantize/execute/quantize routine
    // - Output scale is set to 1.0 / 2^(BIT_NUM)
    // - For signed types output zero point is set to 0
    // - For unsigned types output zero point is set to (qmax + qmin) / 2.0
    // See https://stackoverflow.com/a/34448562/3606192 for potential
    // optimizations
    double output_scale = 0.00390625;  // 1.0 / 2^8
    int64_t output_zero_point = 0;
    // NOLINTNEXTLINE(clang-analyzer-core.NullDereference)
    if (SCALAR_TYPE == at::kQInt32) {
      output_scale = 2.3283064365386963e-10;  // 1.0 / 2^32
    } else if (SCALAR_TYPE == at::kQInt8) {
      output_zero_point = -128;
    }
    qsigmoid_stub(qx.device().type(), qx, qy, output_scale, output_zero_point);
  });
  return qy;
}

Tensor relu_quantized_cpu(const Tensor& qx) {
  #ifdef USE_PYTORCH_QNNPACK
  if (at::globalContext().qEngine() == at::QEngine::QNNPACK && qx.scalar_type() == kQUInt8) {
    return qnnpack_relu(qx);
  }
  #endif
  Tensor qy;
  qrelu_stub(qx.device().type(), qx, qy);
  return qy;
}
Tensor& relu_quantized_cpu_(Tensor& qx) {
  const auto zero_point = qx.q_zero_point();
  AT_DISPATCH_QINT_TYPES(qx.scalar_type(), "qrelu", [&]() {
    using Vec = Vectorized<scalar_t>;
    auto iter = TensorIterator::unary_op(qx, qx);
    auto zero_point_vec = Vec(scalar_t(zero_point));
    cpu_kernel_vec(
        iter,
        [&](scalar_t value) -> scalar_t {
          return scalar_t(std::max<underlying_t>(value.val_, zero_point));
        },
        [&](Vec value) -> Vec { return value.relu(zero_point_vec); });
  });
  return qx;
}

Tensor& leaky_relu_out_quantized_cpu(const Tensor& self,
                                 const Scalar& negval, Tensor& result) {
  qrelu_leaky_stub(self.device().type(), result, self, negval);
  return result;
}

Tensor leaky_relu_quantized_cpu(const Tensor& self, const Scalar& negval) {
  const auto qx = self.contiguous(self.suggest_memory_format());
  auto qy = at::_empty_affine_quantized(qx.sizes(),
      at::device(kCPU).dtype(self.scalar_type()),
      qx.q_scale(),
      qx.q_zero_point(),
      self.suggest_memory_format());
  qrelu_leaky_stub(self.device().type(), qy, qx, negval);
  return qy;
}

Tensor& leaky_relu_quantized_cpu_(Tensor& self, const Scalar& negval) {
  qrelu_leaky_stub(self.device().type(), self, self, negval);
  return self;
}

Tensor hardsigmoid_quantized_cpu(const Tensor& qx) {
#ifdef USE_PYTORCH_QNNPACK
  if (at::globalContext().qEngine() == at::QEngine::QNNPACK &&
      qx.scalar_type() == kQUInt8) {
    return qnnpack_hardsigmoid(qx);
  }
#endif  // USE_PYTORCH_QNNPACK
  Tensor qy;
  qhardsigmoid_stub(qx.device().type(), qx, qy);
  return qy;
}

Tensor& hardsigmoid_out_quantized_cpu(const Tensor& qx, Tensor& result) {
  // Note: we create a new temporary tensor because the output of hardsigmoid
  // usually has different quantization parameters from the input, and
  // quantization are currently only supported per entire tensor or per entire
  // channel of a tensor.
  Tensor qy = hardsigmoid_quantized_cpu(qx);
  result.copy_(qy);
  return result;
}

Tensor quantized_hardswish(const Tensor& qx, double output_scale, int64_t output_zero_point) {
  Tensor qy = at::_empty_affine_quantized(
      qx.sizes(),
      at::device(kCPU).dtype(qx.scalar_type()),
      output_scale,
      output_zero_point,
      qx.suggest_memory_format());
#ifdef USE_PYTORCH_QNNPACK
  if (at::globalContext().qEngine() == at::QEngine::QNNPACK &&
      qx.scalar_type() == kQUInt8) {
    Tensor qx_contig = qx.contiguous(qx.suggest_memory_format());
    qnnpack_hardswish(qx_contig, qy);
    return qy;
  }
#endif  // USE_PYTORCH_QNNPACK
  qhardswish_stub(qx.device().type(), qx, qy);
  return qy;
}

namespace {

class QSigmoid final {
 public:
  static Tensor run(Tensor qx, double output_scale, int64_t output_zero_point) {
#ifdef USE_PYTORCH_QNNPACK
  if (at::globalContext().qEngine() == at::QEngine::QNNPACK &&
      qx.scalar_type() == kQUInt8) {
    return qnnpack_sigmoid(qx, output_scale, output_zero_point);
  }
#endif  // USE_PYTORCH_QNNPACK
  Tensor qy;
  qsigmoid_stub(qx.device().type(), qx, qy, output_scale, output_zero_point);
  return qy;
  }
};

Tensor quantized_relu6(const Tensor& qx) {
  Tensor qy;
  qy = hardtanh_quantized_cpu(qx, 0.0f, 6.0f);
  return qy;
}

Tensor quantized_relu6_(Tensor& qx) {
  hardtanh_quantized_cpu_(qx, 0.0f, 6.0f);
  return qx;
}

class QRelu6 final {
 public:
  static Tensor run(Tensor qx, bool inplace) {
    if (inplace) {
      return quantized_relu6_(qx);
    } else {
      return quantized_relu6(qx);
    }
  }
};

class QLeakyRelu final {
 public:
  static Tensor run(Tensor self, const Scalar& negative_slope, bool inplace, double output_scale, int64_t output_zero_point) {
    // inplace argument is ignored now, TODO:support inplace
    if (inplace) {
      TORCH_WARN("inplace=True is not supported for quantized::leaky_relu yet");
    }
    const auto qx = self.contiguous(self.suggest_memory_format());
    auto qy = at::_empty_affine_quantized(qx.sizes(),
      at::device(kCPU).dtype(self.scalar_type()),
      output_scale,
      output_zero_point,
      self.suggest_memory_format());
    qrelu_leaky_stub(self.device().type(), qy, qx, negative_slope);
    return qy;
  }
};

TORCH_LIBRARY_IMPL(quantized, QuantizedCPU, m) {
  m.impl(TORCH_SELECTIVE_NAME("quantized::hardswish"), TORCH_FN(quantized_hardswish));
  m.impl(TORCH_SELECTIVE_NAME("quantized::relu6"), TORCH_FN(QRelu6::run));
  m.impl(TORCH_SELECTIVE_NAME("quantized::leaky_relu"), TORCH_FN(QLeakyRelu::run));
  m.impl(TORCH_SELECTIVE_NAME("quantized::sigmoid"), TORCH_FN(QSigmoid::run));
}
} // anonyous namespace

}}  // namespace at::native
