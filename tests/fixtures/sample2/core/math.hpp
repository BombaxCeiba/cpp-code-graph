// core/math.hpp - numeric helpers. Fixture data: function templates with
// explicit specializations, an overload set, and recursion.
#ifndef SAMPLE2_CORE_MATH_HPP
#define SAMPLE2_CORE_MATH_HPP

#include <array>

namespace core {

// Overload set: same name, three parameter types. Call sites resolve through
// static overloading, so each lerp call edge lands on its own symbol.
inline double lerp(double a, double b, double t)
{
    return a + (b - a) * t;
}

inline float lerp(float a, float b, float t)
{
    return a + (b - a) * t;
}

inline long double lerp(long double a, long double b, long double t)
{
    return a + (b - a) * t;
}

// Recursive Euclid gcd on integers.
inline unsigned long long gcd(unsigned long long a, unsigned long long b)
{
    if (b == 0)
        return a;
    return gcd(b, a % b);
}

// Recursive integer power used by the fee model.
inline double ipow(double base, int exp)
{
    if (exp == 0)
        return 1.0;
    if (exp < 0)
        return 1.0 / ipow(base, -exp);
    const double half = ipow(base, exp / 2);
    if (exp % 2 == 0)
        return half * half;
    return half * half * base;
}

// Primary function template with an EXPLICIT SPECIALIZATION below.
template <typename T>
const char* type_tag()
{
    return "opaque";
}

template <>
inline const char* type_tag<double>()
{
    return "f64";
}

template <>
inline const char* type_tag<long long>()
{
    return "i64";
}

// Clamp through a template; the specializations pick faster paths.
template <typename T>
T clamp_value(T value, T low, T high)
{
    if (value < low)
        return low;
    if (high < value)
        return high;
    return value;
}

template <>
inline float clamp_value(float value, float low, float high)
{
    // Branch-free variant for floats.
    return value < low ? low : (high < value ? high : value);
}

// Weighted mean over a fixed array; calls into lerp(double,...).
template <std::size_t N>
double weighted_avg(const std::array<double, N>& values,
                    const std::array<double, N>& weights)
{
    double acc = 0.0;
    double total = 0.0;
    for (std::size_t i = 0; i < N; ++i) {
        acc = lerp(acc, values[i], weights[i] / (weights[i] + 1.0));
        total += weights[i];
    }
    return total > 0.0 ? acc / total : 0.0;
}

// DEAD CODE: never referenced anywhere in the fixture.
inline double sigmoid(double x)
{
    return 1.0 / (1.0 + ipow(2.718281828459045, -static_cast<int>(x * 4)));
}

}  // namespace core

#endif  // SAMPLE2_CORE_MATH_HPP
