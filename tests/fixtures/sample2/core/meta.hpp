// core/meta.hpp - CRTP mixins and compile-time naming. Fixture data: two CRTP
// bases, a class template with an explicit specialization, and a function
// template specialization calling back into core::math.
#ifndef SAMPLE2_CORE_META_HPP
#define SAMPLE2_CORE_META_HPP

#include <string>

#include "core/math.hpp"

namespace core {

// CRTP base one: instance bookkeeping for any derived class.
template <typename Derived>
class InstanceCounter {
public:
    InstanceCounter()
    {
        alive_ += 1;
    }

    InstanceCounter(const InstanceCounter&)
    {
        alive_ += 1;
    }

    ~InstanceCounter()
    {
        alive_ -= 1;
    }

    static int alive()
    {
        return alive_;
    }

private:
    static int alive_;
};

template <typename Derived>
int InstanceCounter<Derived>::alive_ = 0;

// CRTP base two: polymorphic-for-free describe() that asks the derived class.
template <typename Derived>
class Describable {
public:
    std::string describe() const
    {
        const Derived& self = static_cast<const Derived&>(*this);
        return self.name() + std::string{"#"} + self.tag();
    }

protected:
    Describable() = default;
    ~Describable() = default;
};

// Primary class template: human-readable name for a type.
template <typename T>
struct TypeName {
    static const char* get()
    {
        return "unknown";
    }
};

// EXPLICIT SPECIALIZATION of the class template.
template <>
struct TypeName<double> {
    static const char* get()
    {
        return "double";
    }
};

// Cross-file friendly bridge: the specialization asks type_tag<double>.
template <>
struct TypeName<long long> {
    static const char* get()
    {
        return type_tag<long long>();
    }
};

}  // namespace core

#endif  // SAMPLE2_CORE_META_HPP
