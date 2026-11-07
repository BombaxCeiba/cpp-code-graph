// sample1 fixture: cross-file caller with std::vector usage (stdlib hygiene
// case) and a namespace-scope constexpr variable. Parse-input DATA.
#ifndef SAMPLE1_CANVAS_HPP
#define SAMPLE1_CANVAS_HPP

#include <vector>

#include "circle.hpp"

namespace render {

constexpr int kMaxCircles = 16;

class Canvas {
public:
    void add(const shape::Circle& circle);
    double total_area() const;

private:
    std::vector<shape::Circle> circles_;
};

}  // namespace render

#endif
