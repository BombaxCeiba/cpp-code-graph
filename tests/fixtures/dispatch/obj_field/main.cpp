struct I
{
    virtual int f();
    virtual ~I() = default;
};
struct Impl : I
{
    int f() override;
};

int I::f() { return 0; }
int Impl::f() { return 1; }

struct S
{
    I*  p = nullptr;
    int run() { return p->f(); }
};

int main()
{
    S s;
    s.p = new Impl;
    return s.run() + s.p->f();
}
