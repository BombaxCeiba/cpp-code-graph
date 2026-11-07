// Fixture header with known declarations, exercised by cg_test_clang.
// Classic include-based C++ on purpose: fixtures are libclang parse-input
// DATA, never compiled by the project build, so
// the all-.ixx rule does not apply here. Deliberately free of standard
// library includes so replay works with any argument set.
#ifndef CCG_FIXTURES_CLANG_BASELINE_HPP
#define CCG_FIXTURES_CLANG_BASELINE_HPP

namespace baseline {

class WidgetBase {
public:
    virtual ~WidgetBase();
    virtual int weight() const;
    virtual void describe() const = 0;
};

class Widget : public WidgetBase {
public:
    Widget();
    int weight() const;
    void describe() const;
    void set_cached(int value);
    static int reserve();
    bool heavier_than(const Widget& other) const;

private:
    int cached_;
};

typedef WidgetBase BaseWidget;

int weigh(Widget& w);
int run();

}  // namespace baseline

#endif
