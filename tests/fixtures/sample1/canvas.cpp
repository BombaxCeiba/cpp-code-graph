// sample1 fixture: the cross-file tier-1 call target (Canvas::total_area
// calls Circle::area whose definition lives in another TU). Parse-input
// DATA.
#include "canvas.hpp"

namespace render
{

    void Canvas::add(const shape::Circle& circle)
    {
        circles_.push_back(circle);
    }

    double Canvas::total_area() const
    {
        double total = 0.0;
        for (const shape::Circle& c : circles_)
            total += c.area();
        return total;
    }

} // namespace render
