// define constants like M_PI and C keywords for MSVC
#ifdef _MSC_VER
#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES
#endif
#include <math.h>
#endif

#include <ATen/ATen.h>
#include <ATen/Dispatch.h>
#include <ATen/ExpandUtils.h>
#include <ATen/NativeFunctions.h>
#include <ATen/LegacyTHFunctionsCPU.h>
#include <ATen/MemoryOverlap.h>
#include <ATen/WrapDimUtils.h>

#include <ATen/CPUApplyUtils.h>
#include <ATen/Parallel.h>
#include <ATen/native/UnaryOps.h>
#include <ATen/native/TensorIterator.h>
#include <ATen/NamedTensorUtils.h>
#include <ATen/native/ComplexHelper.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <numeric>
#include <vector>

#include <map>

namespace at {
namespace native {

// NOTE: These are helper functions that reduce redundant code in implementing the most typical kind of unary operators.
// YOU ARE NOT OBLIGED TO USE THESE HELPERS---if you're writing something more specialized, please don't try to make
// them work for your case, but just write something new instead. Here we use helper functions instead of a flat fat
// macro that implements everything, because the former allows some simple preprocessing that are unique to some
// operators (more is foreseeable) and is more flexible and elegant than the latter.
template <typename Stub>
static inline Tensor& unary_op_impl_out(Tensor& result, const Tensor& self, Stub& stub) {
  auto iter = TensorIterator::unary_op(result, self,
    /*check_mem_overlap=*/true);
  stub(iter.device_type(), iter);
  return result;
}

// An alternate version of unary_op_impl_out that follows the same pattern
// for non-complex inputs, but returns a floating point tensor
// for complex inputs by default.
// Note: This is done by running the operation as usual and then copying the
// operation's result to the expected result type.
template <typename Stub>
static inline Tensor& unary_op_impl_with_complex_to_float_out(Tensor& result, const Tensor& self, Stub& stub) {
    if (self.is_complex() && !result.is_complex()) {
      // Checks if the corresponding float type can be cast to the desired dtype
      const auto float_type = c10::toValueType(self.scalar_type());
      TORCH_CHECK(canCast(float_type, result.scalar_type()),
            "result type ", float_type, " can't be cast to the desired output type ",
            result.scalar_type());

      // Runs the function complex->complex, as TensorIterator expects
      Tensor complex_result = at::empty({0}, self.options());
      auto iter = TensorIterator::unary_op(complex_result, self,
        /*check_mem_overlap=*/true);
      stub(iter.device_type(), iter);

      // Copies the complex result to the actual result and returns it
      result.resize_(complex_result.sizes());
      result.copy_(complex_result);
      return result;
    }

    return unary_op_impl_out(result, self, stub);
}

// out_impl passed into unary_op_impl and unary_op_impl_  must go through at:: device dispatch
// otherwise it won't dispatch to out-of-source devices like XLA.
// For example it must be at::bitwise_not_out instead of bitwise_not_out(which is at::native!).
template <typename OutImpl>
static inline Tensor unary_op_impl(const Tensor& self, OutImpl& out_impl) {
  Tensor result = at::empty({0}, self.options());
  return out_impl(result, self);
}

// An alternate version of unary_op_impl that follows the same pattern
// for non-complex inputs, but returns a floating point tensor
// for complex inputs by default.
template <typename OutImpl>
static inline Tensor unary_op_impl_with_complex_to_float(const Tensor& self, OutImpl& out_impl) {
  if (self.is_complex()) {
    const auto float_type = c10::toValueType(self.scalar_type());
    Tensor result = at::empty({0}, self.options().dtype(float_type));
    return out_impl(result, self);
  }

  Tensor result = at::empty({0}, self.options());
  return out_impl(result, self);
}

template <typename OutImpl>
static inline Tensor& unary_op_impl_(Tensor& self, OutImpl& out_impl) {
  return out_impl(self, self);
}

Tensor& acos_out(Tensor& result, const Tensor& self) { return unary_op_impl_out(result, self, acos_stub); }
Tensor acos(const Tensor& self) { return unary_op_impl(self, at::acos_out); }
Tensor& acos_(Tensor& self) { return unary_op_impl_(self, at::acos_out); }

static Tensor wrapped_scalar_tensor(Scalar scalar) {
  auto tensor = scalar_to_tensor(scalar);
  tensor.unsafeGetTensorImpl()->set_wrapped_number(true);
  return tensor;
}

Tensor& rad2deg_out(Tensor& result, const Tensor& self) {
  TORCH_CHECK(!self.is_complex(), "rad2deg is not supported for complex tensors.");
  constexpr double M_180_PI = 57.295779513082320876798154814105170332405472466564;
  return at::mul_out(result, self, wrapped_scalar_tensor(Scalar(M_180_PI)));
}

Tensor rad2deg(const Tensor& self) { return unary_op_impl(self, at::rad2deg_out); }
Tensor& rad2deg_(Tensor& self) { return unary_op_impl_(self, at::rad2deg_out); }

Tensor& deg2rad_out(Tensor& result, const Tensor& self) {
  TORCH_CHECK(!self.is_complex(), "deg2rad is not supported for complex tensors.");
  constexpr double M_PI_180 = 0.017453292519943295769236907684886127134428718885417;
  return at::mul_out(result, self, wrapped_scalar_tensor(Scalar(M_PI_180)));
}
Tensor deg2rad(const Tensor& self) { return unary_op_impl(self, at::deg2rad_out); }
Tensor& deg2rad_(Tensor& self) { return unary_op_impl_(self, at::deg2rad_out); }

Tensor& asin_out(Tensor& result, const Tensor& self) { return unary_op_impl_out(result, self, asin_stub); }
Tensor asin(const Tensor& self) { return unary_op_impl(self, at::asin_out); }
Tensor& asin_(Tensor& self) { return unary_op_impl_(self, at::asin_out); }

Tensor& atan_out(Tensor& result, const Tensor& self) { return unary_op_impl_out(result, self, atan_stub); }
Tensor atan(const Tensor& self) { return unary_op_impl(self, at::atan_out); }
Tensor& atan_(Tensor& self) { return unary_op_impl_(self, at::atan_out); }

// Note [Complex abs and angle]
// Complex inputs to abs and angle return float results by default.
// abs and angle, in both NumPy and C++, returns a float result when given a
// complex input. This makes sense mathematically since the absolute value
// and angle of a complex number has no imaginary part.
Tensor& abs_out(Tensor& result, const Tensor& self) {
  return unary_op_impl_with_complex_to_float_out(result, self, abs_stub);
}
Tensor abs(const Tensor& self) {
  return unary_op_impl_with_complex_to_float(self, at::abs_out);
}
Tensor& abs_(Tensor& self) { return unary_op_impl_(self, at::abs_out); }

Tensor& angle_out(Tensor& result, const Tensor& self) {
  return unary_op_impl_with_complex_to_float_out(result, self, angle_stub);
}
Tensor angle(const Tensor& self) {
  return unary_op_impl_with_complex_to_float(self, at::angle_out);
}

Tensor real(const Tensor& self) {
  if (self.is_complex()) {
    auto float_tensor = at::native::view_complex_as_float(self);
    return at::select(float_tensor, float_tensor.dim() - 1, 0);
  } else {
    TORCH_CHECK(false, "real is not implemented for tensors with non-complex dtypes.");
  }
}

Tensor imag(const Tensor& self) {
  if (self.is_complex()) {
    auto float_tensor = at::native::view_complex_as_float(self);
    return at::select(float_tensor, float_tensor.dim() - 1, 1);
  } else {
    TORCH_CHECK(false, "imag is not implemented for tensors with non-complex dtypes.");
  }
}

Tensor& conj_out(Tensor& result, const Tensor& self) { return unary_op_impl_out(result, self, conj_stub); }
Tensor conj(const Tensor& self) { return unary_op_impl(self, at::conj_out); }

Tensor& bitwise_not_out(Tensor& result, const Tensor& self) { return unary_op_impl_out(result, self, bitwise_not_stub); }
Tensor bitwise_not(const Tensor& self) { return unary_op_impl(self, at::bitwise_not_out); }
Tensor& bitwise_not_(Tensor& self) { return unary_op_impl_(self, at::bitwise_not_out); }

Tensor& ceil_out(Tensor& result, const Tensor& self) {
  // Note: this is consistent with NumPy
  TORCH_CHECK(!self.is_complex(),
    "ceil is not supported for complex inputs");

  return unary_op_impl_out(result, self, ceil_stub);
}
Tensor ceil(const Tensor& self) { return unary_op_impl(self, at::ceil_out); }
Tensor& ceil_(Tensor& self) { return unary_op_impl_(self, at::ceil_out); }

Tensor& exp_out(Tensor& result, const Tensor& self) { return unary_op_impl_out(result, self, exp_stub); }
Tensor exp(const Tensor& self) { return unary_op_impl(self, at::exp_out); }
Tensor& exp_(Tensor& self) { return unary_op_impl_(self, at::exp_out); }

Tensor& expm1_out(Tensor& result, const Tensor& self) { return unary_op_impl_out(result, self, expm1_stub); }
Tensor expm1(const Tensor& self) { return unary_op_impl(self, at::expm1_out); }
Tensor& expm1_(Tensor& self) { return unary_op_impl_(self, at::expm1_out); }

Tensor& erf_out(Tensor& result, const Tensor& self) { return unary_op_impl_out(result, self, erf_stub); }
Tensor erf(const Tensor& self) { return unary_op_impl(self, at::erf_out); }
Tensor& erf_(Tensor& self) { return unary_op_impl_(self, at::erf_out); }

Tensor& erfc_out(Tensor& result, const Tensor& self) { return unary_op_impl_out(result, self, erfc_stub); }
Tensor erfc(const Tensor& self) { return unary_op_impl(self, at::erfc_out); }
Tensor& erfc_(Tensor& self) { return unary_op_impl_(self, at::erfc_out); }

Tensor& frac_out(Tensor& result, const Tensor& self) { return unary_op_impl_out(result, self, frac_stub); }
Tensor frac(const Tensor& self) { return unary_op_impl(self, at::frac_out); }
Tensor& frac_(Tensor& self) { return unary_op_impl_(self, at::frac_out); }

Tensor& floor_out(Tensor& result, const Tensor& self) {
  // Note: this is consistent with NumPy
  TORCH_CHECK(!self.is_complex(),
    "floor is not supported for complex inputs");

  return unary_op_impl_out(result, self, floor_stub);
}
Tensor floor(const Tensor& self) { return unary_op_impl(self, at::floor_out); }
Tensor& floor_(Tensor& self) { return unary_op_impl_(self, at::floor_out); }

Tensor& log_out(Tensor& result, const Tensor& self) { return unary_op_impl_out(result, self, log_stub); }
Tensor log(const Tensor& self) { return unary_op_impl(self, at::log_out); }
Tensor& log_(Tensor& self) { return unary_op_impl_(self, at::log_out); }

Tensor& log10_out(Tensor& result, const Tensor& self) { return unary_op_impl_out(result, self, log10_stub); }
Tensor log10(const Tensor& self) { return unary_op_impl(self, at::log10_out); }
Tensor& log10_(Tensor& self) { return unary_op_impl_(self, at::log10_out); }

Tensor& log1p_out(Tensor& result, const Tensor& self) { return unary_op_impl_out(result, self, log1p_stub); }
Tensor log1p(const Tensor& self) { return unary_op_impl(self, at::log1p_out); }
Tensor& log1p_(Tensor& self) { return unary_op_impl_(self, at::log1p_out); }

Tensor& log2_out(Tensor& result, const Tensor& self) { return unary_op_impl_out(result, self, log2_stub); }
Tensor log2(const Tensor& self) { return unary_op_impl(self, at::log2_out); }
Tensor& log2_(Tensor& self) { return unary_op_impl_(self, at::log2_out); }

Tensor& round_out(Tensor& result, const Tensor& self) { return unary_op_impl_out(result, self, round_stub); }
Tensor round(const Tensor& self) { return unary_op_impl(self, at::round_out); }
Tensor& round_(Tensor& self) { return unary_op_impl_(self, at::round_out); }

Tensor& digamma_out(Tensor& result, const Tensor& self) { return unary_op_impl_out(result, self, digamma_stub); }
Tensor digamma(const Tensor& self) { return unary_op_impl(self, digamma_out); }
Tensor& digamma_(Tensor& self) { return unary_op_impl_(self, digamma_out); }

Tensor& reciprocal_out(Tensor& result, const Tensor& self) { return unary_op_impl_out(result, self, reciprocal_stub); }
Tensor reciprocal(const Tensor& self) { return unary_op_impl(self, at::reciprocal_out); }
Tensor& reciprocal_(Tensor& self) { return unary_op_impl_(self, at::reciprocal_out); }

Tensor& rsqrt_out(Tensor& result, const Tensor& self) { return unary_op_impl_out(result, self, rsqrt_stub); }
Tensor rsqrt(const Tensor& self) { return unary_op_impl(self, at::rsqrt_out); }
Tensor& rsqrt_(Tensor& self) { return unary_op_impl_(self, at::rsqrt_out); }

Tensor& sign_out(Tensor& result, const Tensor& self) { return unary_op_impl_out(result, self, sign_stub); }
Tensor sign(const Tensor& self) { return unary_op_impl(self, at::sign_out); }
Tensor& sign_(Tensor& self) { return unary_op_impl_(self, at::sign_out); }

Tensor& sin_out(Tensor& result, const Tensor& self) { return unary_op_impl_out(result, self, sin_stub); }
Tensor sin(const Tensor& self) { return unary_op_impl(self, at::sin_out); }
Tensor& sin_(Tensor& self) { return unary_op_impl_(self, at::sin_out); }

Tensor& cos_out(Tensor& result, const Tensor& self) { return unary_op_impl_out(result, self, cos_stub); }
Tensor cos(const Tensor& self) { return unary_op_impl(self, at::cos_out); }
Tensor& cos_(Tensor& self) { return unary_op_impl_(self, at::cos_out); }

Tensor& sinh_out(Tensor& result, const Tensor& self) { return unary_op_impl_out(result, self, sinh_stub); }
Tensor sinh(const Tensor& self) { return unary_op_impl(self, at::sinh_out); }
Tensor& sinh_(Tensor& self) { return unary_op_impl_(self, at::sinh_out); }

Tensor& cosh_out(Tensor& result, const Tensor& self) { return unary_op_impl_out(result, self, cosh_stub); }
Tensor cosh(const Tensor& self) { return unary_op_impl(self, at::cosh_out); }
Tensor& cosh_(Tensor& self) { return unary_op_impl_(self, at::cosh_out); }

Tensor& acosh_out(Tensor& result, const Tensor& self) { return unary_op_impl_out(result, self, acosh_stub); }
Tensor acosh(const Tensor& self) { return unary_op_impl(self, at::acosh_out); }
Tensor& acosh_(Tensor& self) { return unary_op_impl_(self, at::acosh_out); }

Tensor& asinh_out(Tensor& result, const Tensor& self) { return unary_op_impl_out(result, self, asinh_stub); }
Tensor asinh(const Tensor& self) { return unary_op_impl(self, at::asinh_out); }
Tensor& asinh_(Tensor& self) { return unary_op_impl_(self, at::asinh_out); }

Tensor& atanh_out(Tensor& result, const Tensor& self) { return unary_op_impl_out(result, self, atanh_stub); }
Tensor atanh(const Tensor& self) { return unary_op_impl(self, at::atanh_out); }
Tensor& atanh_(Tensor& self) { return unary_op_impl_(self, at::atanh_out); }

Tensor& sqrt_out(Tensor& result, const Tensor& self) { return unary_op_impl_out(result, self, sqrt_stub); }
Tensor sqrt(const Tensor& self) { return unary_op_impl(self, at::sqrt_out); }
Tensor& sqrt_(Tensor& self) { return unary_op_impl_(self, at::sqrt_out); }

Tensor square(const Tensor& self) { return at::pow(self, 2); }
Tensor& square_(Tensor& self) { return at::pow_out(self, self, 2); }

Tensor& sigmoid_out(Tensor& result, const Tensor& self) { return unary_op_impl_out(result, self, sigmoid_stub);  }
Tensor sigmoid(const Tensor& self) { return unary_op_impl(self, at::sigmoid_out);  }
Tensor& sigmoid_(Tensor& self) { return unary_op_impl_(self, at::sigmoid_out);  }

Tensor& tanh_out(Tensor& result, const Tensor& self) { return unary_op_impl_out(result, self, tanh_stub); }
Tensor tanh(const Tensor& self) { return unary_op_impl(self, at::tanh_out); }
Tensor& tanh_(Tensor& self) { return unary_op_impl_(self, at::tanh_out); }

Tensor& tan_out(Tensor& result, const Tensor& self) { return unary_op_impl_out(result, self, tan_stub);  }
Tensor tan(const Tensor& self) { return unary_op_impl(self, at::tan_out);  }
Tensor& tan_(Tensor& self) { return unary_op_impl_(self, at::tan_out);  }

Tensor& trunc_out(Tensor& result, const Tensor& self) {
  // Note: this is consistent with NumPy
  TORCH_CHECK(!self.is_complex(),
    "trunc is not supported for complex inputs");

  return unary_op_impl_out(result, self, trunc_stub);
}
Tensor trunc(const Tensor& self) { return unary_op_impl(self, at::trunc_out); }
Tensor& trunc_(Tensor& self) { return unary_op_impl_(self, at::trunc_out); }

Tensor& neg_out(Tensor& result, const Tensor& self) {
  TORCH_CHECK(self.scalar_type() != kBool,
              "Negation, the `-` operator, on a bool tensor is not supported. "
              "If you are trying to invert a mask, use the `~` or `logical_not()` operator instead.");
  return unary_op_impl_out(result, self, neg_stub);
}
Tensor neg(const Tensor& self) { return unary_op_impl(self, at::neg_out); }
Tensor& neg_(Tensor& self) { return unary_op_impl_(self, at::neg_out); }

Tensor logical_not(const Tensor& self) {
  Tensor result = at::empty({0}, self.options().dtype(kBool));
  return at::logical_not_out(result, self);
}

Tensor& logical_not_(Tensor& self) {
  return at::logical_not_out(self, self);
}

Tensor& logical_not_out(Tensor& result, const Tensor& self) {
  TensorIterator iter = TensorIteratorConfig()
    .check_all_same_dtype(false)
    .set_check_mem_overlap(true)
    .add_output(result)
    .add_input(self)
    .build();
  logical_not_stub(iter.device_type(), iter);
  return result;
}

Tensor& clamp_out(Tensor& result, const Tensor& self, optional<Scalar> min, optional<Scalar> max) {
  TORCH_CHECK(!self.is_complex(), "clamp is not yet implemented for complex tensors.");
  if (min && max) {
    TORCH_CHECK(self.layout() == Layout::Strided,
                "clamp only supports strided layout, got: ", self.layout());
    auto iter = TensorIterator::unary_op(result, self,
        /*check_mem_overlap=*/true);
    clamp_stub(iter.device_type(), iter, *min, *max);
  } else if (max) {
    at::clamp_max_out(result, self, *max);
  } else if (min) {
    at::clamp_min_out(result, self, *min);
  } else {
    AT_ERROR("At least one of 'min' or 'max' must not be None");
  }
  return result;
}

Tensor clamp(const Tensor& self, optional<Scalar> min, optional<Scalar> max) {
  Tensor result = at::empty({0}, self.options());
  return at::clamp_out(result, self, min, max);
}

Tensor& clamp_(Tensor& self, optional<Scalar> min, optional<Scalar> max) {
  return at::clamp_out(self, self, min, max);
}

Tensor& clamp_max_out(Tensor& result, const Tensor& self, Scalar max) {
  TORCH_CHECK(!self.is_complex(), "clamp is not yet implemented for complex tensors.");
  TORCH_CHECK(self.layout() == Layout::Strided,
              "clamp_max only supports strided layout, got: ", self.layout());
  auto iter = TensorIterator::unary_op(result, self,
      /*check_mem_overlap=*/true);
  clamp_max_stub(iter.device_type(), iter, max);
  return result;
}

Tensor clamp_max(const Tensor& self, Scalar max) {
  Tensor result = at::empty({0}, self.options());
  return at::clamp_max_out(result, self, max);
}

Tensor& clamp_max_(Tensor& self, Scalar max) {
  return at::clamp_max_out(self, self, max);
}

Tensor& clamp_min_out(Tensor& result, const Tensor& self, Scalar min) {
  TORCH_CHECK(!self.is_complex(), "clamp is not yet implemented for complex tensors.");
  TORCH_CHECK(self.layout() == Layout::Strided,
              "clamp_min only supports strided layout, got: ", self.layout());
  auto iter = TensorIterator::unary_op(result, self,
      /*check_mem_overlap=*/true);
  clamp_min_stub(iter.device_type(), iter, min);
  return result;
}

Tensor clamp_min(const Tensor& self, Scalar min) {
  Tensor result = at::empty({0}, self.options());
  return at::clamp_min_out(result, self, min);
}

Tensor& clamp_min_(Tensor& self, Scalar min) {
  return at::clamp_min_out(self, self, min);
}

Tensor polygamma(int64_t n, const Tensor& self) {
  Tensor result = at::empty({0}, self.options());
  at::polygamma_out(result, n, self);
  return result;
}
Tensor& polygamma_(Tensor& self, int64_t n) {
  return at::polygamma_out(self, n, self);
}
Tensor& polygamma_out(Tensor& result, int64_t n, const Tensor& self) {
  TORCH_CHECK(n >= 0, "polygamma(n, x) does not support negative n.");
  auto iter = TensorIterator::unary_op(result, self,
    /*check_mem_overlap=*/true);
  polygamma_stub(iter.device_type(), iter, n);
  return result;
}

static inline void mvlgamma_check(const Tensor& self, int64_t p) {
  TORCH_CHECK(at::isFloatingType(self.scalar_type()),
              "mvlgamma is not implemented for ", self.scalar_type());
  TORCH_CHECK((self > 0.5f * (p - 1)).all().item<bool>(),
              "All elements must be greater than (p-1)/2");
  TORCH_CHECK(p >= 1, "p has to be greater than or equal to 1");
}

Tensor mvlgamma(const Tensor& self, int64_t p) {
  mvlgamma_check(self, p);
  Tensor args = native::arange(-p / 2. + 0.5, 0.5, 0.5, self.options());
  args = args.add(self.unsqueeze(-1));
  return args.lgamma_().sum(-1).add_(p * (p - 1) * std::log(M_PI) / 4.);
}

Tensor& mvlgamma_(Tensor& self, int64_t p) {
  mvlgamma_check(self, p);
  Tensor args = native::arange(-p / 2. + 0.5, 0.5, 0.5, self.options());
  args = args.add(self.unsqueeze(-1));
  return self.copy_(args.lgamma_().sum(-1).add_(p * (p - 1) * std::log(M_PI) / 4.));
}

// NB: If you use this macro, you may also need to add a CUDA forwarding
// stub in CUDAUnaryOps

#define IMPLEMENT_UNARY_OP_CORE(op)                                    \
  Tensor op(const Tensor& self) {                                      \
    Tensor result = at::empty({0}, self.options());                    \
    at::op##_out(result, self);                                        \
    return result;                                                     \
  }

#define IMPLEMENT_UNARY_OP_OUT_INPLACE(op, prefix, device)             \
  Tensor& _##op##__##prefix(Tensor& self) {                            \
    return at::op##_out(self, self);                                   \
  }                                                                    \
  Tensor& _##op##_out_##prefix(Tensor& result, const Tensor& self) {   \
    checkDeviceType(#op, result, DeviceType::device);                  \
    checkLayout(#op, result, Layout::Strided);                         \
    auto iter = TensorIterator::unary_op(result, self,                 \
      /*check_mem_overlap=*/true);                                     \
    op##_stub(iter.device_type(), iter);                               \
    return result;                                                     \
  }

#define IMPLEMENT_UNARY_OP_VEC(op)                                     \
  IMPLEMENT_UNARY_OP_CORE(op)                                          \
  IMPLEMENT_UNARY_OP_OUT_INPLACE(op, cpu, CPU)

#define IMPLEMENT_UNARY_OP_VEC_CUDA(op)                                \
  IMPLEMENT_UNARY_OP_CORE(op)                                          \
  IMPLEMENT_UNARY_OP_OUT_INPLACE(op, cpu, CPU)                         \
  IMPLEMENT_UNARY_OP_OUT_INPLACE(op, cuda, CUDA)

IMPLEMENT_UNARY_OP_VEC_CUDA(erfinv)
IMPLEMENT_UNARY_OP_VEC_CUDA(lgamma)

DEFINE_DISPATCH(abs_stub);
DEFINE_DISPATCH(angle_stub);
DEFINE_DISPATCH(real_stub);
DEFINE_DISPATCH(imag_stub);
DEFINE_DISPATCH(conj_stub);
DEFINE_DISPATCH(acos_stub);
DEFINE_DISPATCH(acosh_stub);
DEFINE_DISPATCH(asinh_stub);
DEFINE_DISPATCH(atanh_stub);
DEFINE_DISPATCH(asin_stub);
DEFINE_DISPATCH(atan_stub);
DEFINE_DISPATCH(bitwise_not_stub);
DEFINE_DISPATCH(ceil_stub);
DEFINE_DISPATCH(clamp_stub);
DEFINE_DISPATCH(clamp_max_stub);
DEFINE_DISPATCH(clamp_min_stub);
DEFINE_DISPATCH(cos_stub);
DEFINE_DISPATCH(cosh_stub);
DEFINE_DISPATCH(digamma_stub);
DEFINE_DISPATCH(erf_stub);
DEFINE_DISPATCH(erfc_stub);
DEFINE_DISPATCH(erfinv_stub);
DEFINE_DISPATCH(exp_stub);
DEFINE_DISPATCH(expm1_stub);
DEFINE_DISPATCH(floor_stub);
DEFINE_DISPATCH(frac_stub);
DEFINE_DISPATCH(log_stub);
DEFINE_DISPATCH(log10_stub);
DEFINE_DISPATCH(log1p_stub);
DEFINE_DISPATCH(log2_stub);
DEFINE_DISPATCH(logical_not_stub);
DEFINE_DISPATCH(neg_stub);
DEFINE_DISPATCH(polygamma_stub);
DEFINE_DISPATCH(reciprocal_stub);
DEFINE_DISPATCH(round_stub);
DEFINE_DISPATCH(rsqrt_stub);
DEFINE_DISPATCH(sigmoid_stub);
DEFINE_DISPATCH(sign_stub);
DEFINE_DISPATCH(sin_stub);
DEFINE_DISPATCH(sinh_stub);
DEFINE_DISPATCH(sqrt_stub);
DEFINE_DISPATCH(tan_stub);
DEFINE_DISPATCH(tanh_stub);
DEFINE_DISPATCH(trigamma_stub);
DEFINE_DISPATCH(trunc_stub);
DEFINE_DISPATCH(lgamma_stub);
}
} // namespace at
