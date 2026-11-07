// 函数体字段工厂赋值臂夹具：标准库智能指针成员在成员函数体内被
// make_unique/make_shared 赋值（gtest 夹具 SetUp 同款形态），另一成员
// 函数消费。负例覆盖同字段多实现并集不单选、非工厂 RHS 零证据不猜、
// 局部变量先声明后赋值的同款 operator= 形态。
#include <memory>

struct I
{
    virtual int f();
    virtual ~I() = default;
};
struct Impl : I
{
    int f() override;
};
struct Impl2 : I
{
    int f() override;
};

int I::f() { return 0; }
int Impl::f() { return 1; }
int Impl2::f() { return 2; }

// 正例：函数体字段工厂赋值，另一成员函数消费。
struct S
{
    void setup() { p = std::make_unique<Impl>(); }
    int  run() { return p->f(); }
    std::unique_ptr<I> p;
};

// 多字段互不串染：两个壳字段各走各的工厂。
struct Multi
{
    void setup()
    {
        p = std::make_unique<Impl>();
        q = std::make_unique<Impl2>();
    }
    int run() { return p->f() + q->f(); }
    std::unique_ptr<I> p;
    std::unique_ptr<I> q;
};

// 负例：同一字段两次工厂赋值——槽级并集，站点不单选。
struct Ambi
{
    void setup(bool alt)
    {
        p = std::make_unique<Impl>();
        if (alt)
        {
            p = std::make_unique<Impl2>();
        }
    }
    int run() { return p->f(); }
    std::unique_ptr<I> p;
};

// 负例：非工厂 RHS（nullptr）零类型证据，站点不得凭空解析。
struct Null
{
    void setup() { p = nullptr; }
    int  run() { return p->f(); }
    std::unique_ptr<I> p;
};

// 局部变量先声明后赋值（operator= 形态，非声明处初始化）。
int local_late()
{
    std::unique_ptr<I> p;
    p = std::make_unique<Impl>();
    return p->f();
}

// make_shared 工厂同款形态。
struct Shared
{
    void setup() { p = std::make_shared<Impl>(); }
    int  run() { return p->f(); }
    std::shared_ptr<I> p;
};

int main()
{
    S s;
    s.setup();
    int a = s.run();
    Multi m;
    m.setup();
    int b = m.run();
    Ambi am;
    am.setup(false);
    int c = am.run();
    Null n;
    n.setup();
    int d = n.run();
    Shared sh;
    sh.setup();
    int e = sh.run();
    return a + b + c + d + e + local_late();
}
