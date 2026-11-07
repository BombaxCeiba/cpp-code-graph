#include <array>
#include <vector>

int g() { return 0; }
int (*get())() { return &g; }

int use_array()
{
    std::array<int (*)(), 1> a{&g};
    return a[0]();
}

int use_vector()
{
    std::vector<int (*)()> v{&g};
    return v[0]();
}

int use_nested() { return get()(); }

std::vector<int> make_vec() { return {1, 2}; }

int use_operator()
{
    // 真正的运算符调用（对象表达式是一次调用）不得被误判为间接调用。
    return make_vec()[0];
}
