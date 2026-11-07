// sample1 fixture: out-of-line definitions (the decl/def merge case) and a
// free-function call inside a method body. Parse-input DATA.
#include "circle.hpp"

namespace shape
{

    double pi() { return 3.14159265358979; }

    Circle::Circle(double radius) : radius_(radius) {}

    Circle::~Circle() = default;

    double Circle::area() const { return pi() * radius_ * radius_; }

    void Circle::describe() const {}

    void Circle::scale(int factor) { radius_ *= factor; }

    void Circle::scale(double factor) { radius_ *= factor; }

    double Circle::radius() const { return radius_; }

    double unused_helper() { return 0.0; }

} // namespace shape
