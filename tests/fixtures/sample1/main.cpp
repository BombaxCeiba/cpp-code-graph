// sample1 fixture: free functions calling each other, template
// instantiations with two different types, overload calls, std::vector
// usage. Parse-input DATA.
#include "canvas.hpp"
#include "tmpl.hpp"

namespace app
{

    double area_all(const render::Canvas& canvas)
    {
        return canvas.total_area();
    }

    double render_all(const render::Canvas& canvas) { return area_all(canvas); }

} // namespace app

int main()
{
    shape::Box<int> first;
    first.value = 1;
    shape::Box<double> second;
    second.value = 2.0;

    render::Canvas canvas;
    canvas.add(shape::Circle(2.0));
    canvas.add(shape::Circle(3.0));

    const double total = app::render_all(canvas);
    const int    scaled = static_cast<int>(total);

    shape::Circle unit(1.0);
    unit.scale(3);
    unit.scale(0.5);

    return first.get() + second.get() + scaled > 0 ? 0 : 1;
}
