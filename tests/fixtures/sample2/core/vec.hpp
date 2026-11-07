// core/vec.hpp - small fixed vectors. Fixture data: methods, const methods,
// free-function operator overloads, and a dead helper.
#ifndef SAMPLE2_CORE_VEC_HPP
#define SAMPLE2_CORE_VEC_HPP

#include <cmath>

namespace core {

struct Vec3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;

    double length() const
    {
        return std::sqrt(x * x + y * y + z * z);
    }

    double dot(const Vec3& other) const
    {
        return x * other.x + y * other.y + z * other.z;
    }

    Vec3 scaled(double factor) const;
};

inline Vec3 operator+(const Vec3& a, const Vec3& b)
{
    return Vec3{a.x + b.x, a.y + b.y, a.z + b.z};
}

inline Vec3 operator-(const Vec3& a, const Vec3& b)
{
    return Vec3{a.x - b.x, a.y - b.y, a.z - b.z};
}

inline Vec3 operator*(const Vec3& a, double k)
{
    return Vec3{a.x * k, a.y * k, a.z * k};
}

inline Vec3 operator*(double k, const Vec3& a)
{
    return a * k;
}

// Out-of-line method definition for Vec3::scaled.
inline Vec3 Vec3::scaled(double factor) const
{
    return Vec3{x * factor, y * factor, z * factor};
}

// DEAD CODE: cross product computed nowhere.
inline Vec3 cross(const Vec3& a, const Vec3& b)
{
    return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
                a.x * b.y - a.y * b.x};
}

}  // namespace core

#endif  // SAMPLE2_CORE_VEC_HPP
