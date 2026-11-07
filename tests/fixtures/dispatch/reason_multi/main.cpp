struct I
{
    virtual int f();
    virtual ~I() = default;
};
struct Impl : I
{
    int f() override;
};
struct Other : I
{
    int f() override;
};

int I::f() { return 0; }
int Impl::f() { return 1; }
int Other::f() { return 2; }

struct S
{
    I*   p = nullptr;
    void set_impl() { p = new Impl; }
    void set_other() { p = new Other; }
    int  run() { return p->f(); }
};

I* pick(int c)
{
    return c ? static_cast<I*>(new Impl) : static_cast<I*>(new Other);
}

int use_return(int c)
{
    I* q = pick(c);
    return q->f();
}

int main()
{
    S s;
    s.set_impl();
    s.set_other();
    return s.run() + use_return(1);
}
