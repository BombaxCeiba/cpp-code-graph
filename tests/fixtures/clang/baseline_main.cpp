// Fixture main file for cg_test_clang: definitions for baseline.hpp plus a
// free function that CALLS a method (referenced-resolution target) and a
// static/const method mix for the facade's predicate queries.
// Parse-input DATA, not project source.
#include "baseline.hpp"

namespace baseline
{

    WidgetBase::~WidgetBase() = default;

    int WidgetBase::weight() const { return 0; }

    Widget::Widget() : cached_(0) {}

    int Widget::weight() const { return cached_; }

    void Widget::describe() const {}

    void Widget::set_cached(int value) { cached_ = value; }

    int Widget::reserve() { return 16; }

    bool Widget::heavier_than(const Widget& other) const
    {
        return weight() > other.weight();
    }

    int weigh(Widget& w) { return w.weight(); }

    int run()
    {
        Widget w;
        w.set_cached(3);
        return weigh(w) + Widget::reserve();
    }

} // namespace baseline
