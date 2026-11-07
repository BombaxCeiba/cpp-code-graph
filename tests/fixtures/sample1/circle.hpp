// sample1 fixture: derived class with overrides, an overload pair, a field,
// and a typedef. Parse-input DATA, never compiled.
#ifndef SAMPLE1_CIRCLE_HPP
#define SAMPLE1_CIRCLE_HPP

#include "shape.hpp"

namespace shape {

double pi();

class Circle : public Shape {
public:
    Circle(double radius);
    ~Circle() override;

    double area() const override;
    void describe() const override;

    void scale(int factor);
    void scale(double factor);

    double radius() const;

private:
    double radius_;
};

typedef Shape ShapeHandle;

double unused_helper();

}  // namespace shape

#endif
