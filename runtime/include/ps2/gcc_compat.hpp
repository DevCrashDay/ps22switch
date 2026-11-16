#pragma once

// GCC builds (the Switch: devkitA64 is GCC) of code that uses clang's vector builtins. The project is built with clang everywhere else,
// and the code keeps using the clang names; for GCC, which lacks them, this header (force-included by CMakeLists.txt for GCC only)
// defines them as small templates with the same results. GCC has __builtin_shufflevector and __builtin_convertvector itself.
// __builtin_elementwise_ceil and __builtin_elementwise_floor are left alone: their one user asks __has_builtin first.
#if defined(__GNUC__) && !defined(__clang__) && defined(__cplusplus)

#include <cmath>
#include <cstddef>
#include <type_traits>

namespace ps2_gcc_compat {

template <typename V>
constexpr std::size_t lanes() {
    return sizeof(V) / sizeof(V{}[0]);
}

// the OR of every lane, as a scalar of the lane type (clang: __builtin_reduce_or)
template <typename V>
inline auto reduce_or(V v) {
    auto r = v[0];
    for (std::size_t i = 1; i < lanes<V>(); ++i) r |= v[i];
    return r;
}

// the smaller of each pair of lanes (clang: __builtin_elementwise_min); integer lanes compare as their own type, signed or not
template <typename V>
inline V elementwise_min(V a, V b) {
    V r = a;
    for (std::size_t i = 0; i < lanes<V>(); ++i) r[i] = b[i] < a[i] ? b[i] : a[i];
    return r;
}

// the magnitude of each lane (clang: __builtin_elementwise_abs): fabs for floating lanes (the sign bit cleared, -0 becomes +0)
template <typename V>
inline V elementwise_abs(V a) {
    V r = a;
    for (std::size_t i = 0; i < lanes<V>(); ++i) {
        using Lane = std::remove_cv_t<std::remove_reference_t<decltype(a[0])>>;
        if constexpr (std::is_floating_point_v<Lane>) r[i] = std::fabs(a[i]);
        else r[i] = a[i] < 0 ? static_cast<Lane>(-a[i]) : a[i];
    }
    return r;
}

}  // namespace ps2_gcc_compat

#define __builtin_reduce_or(v) ::ps2_gcc_compat::reduce_or(v)
#define __builtin_elementwise_min(a, b) ::ps2_gcc_compat::elementwise_min(a, b)
#define __builtin_elementwise_abs(a) ::ps2_gcc_compat::elementwise_abs(a)

#endif
