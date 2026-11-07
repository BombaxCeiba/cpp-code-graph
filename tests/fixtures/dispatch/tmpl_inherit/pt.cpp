// 模板实例继承边夹具：三形态 + dependent 基类 + 非模板红线。
// 形态移植自探针实验，键名断言以实测为准。
template <class T>
struct Base
{
    void ping() {}
};
struct Plain
{
    void plain_fn() {}
};

struct Derived : Base<Derived>
{
}; // 隐式实例化基：extends 直指 Base<Derived>

template <class T>
struct Multi
{
    void m() {}
};
template <>
struct Multi<int>
{
    void boom() {}
}; // 显式全特化：符号+specializes+成员
struct UsesSpec : Multi<int>
{
}; // extends 直指显式全特化实例

template <bool B, class I>
struct traits
{
    static void go() {}
};
template <class I>
struct traits<true, I>
{
    static void go2() {}
}; // 显式偏特化
struct User : traits<true, int>
{
}; // extends 直指偏特化实例

template <class T>
struct Host : Base<T>
{
}; // dependent 基类：指本尊 + is_template_base

struct P2 : Plain
{
}; // 非模板基：既有路径零变化（红线）

void use()
{
    Derived d;
    d.ping();
    UsesSpec u;
    (void)u;
    User x;
    (void)x;
    Host<int> h;
    (void)h;
    P2 p;
    (void)p;
}
