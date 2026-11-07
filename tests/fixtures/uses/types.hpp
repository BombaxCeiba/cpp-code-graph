// uses-fixture: record targets for USES edges - a plain record, a primary
// class template, an abstract family chain (A->B->C) for override merging,
// one record nothing references (impact "before USES" shape), and an
// opaque forward declaration resolved across TUs. Parse-input DATA, never
// compiled.
#ifndef USES_TYPES_HPP
#define USES_TYPES_HPP

#include <vector>

namespace uses {

class Payload {
public:
    int value = 0;
};

template <typename T>
struct Holder {
    T slot;
};

class NothingReferences {
public:
    int idle = 0;
};

class Opaque;  // 定义在 main.cpp；此处仅经指针引用

struct A {
    virtual ~A() = default;
    virtual int step() const = 0;
};

struct B : A {
    int step() const override;
};

struct C : B {
    int step() const override;
};

}  // namespace uses

#endif
