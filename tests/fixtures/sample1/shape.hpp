// sample1 fixture: abstract base with a pure-virtual and a plain virtual
// method, plus an enum. Parse-input DATA, never compiled.
#ifndef SAMPLE1_SHAPE_HPP
#define SAMPLE1_SHAPE_HPP

namespace shape {

enum class Color { red, green, blue };

class Shape {
public:
    virtual ~Shape() = default;
    virtual double area() const = 0;
    virtual void describe() const {}
};

}  // namespace shape

#endif
