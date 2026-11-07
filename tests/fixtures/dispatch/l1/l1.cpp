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

I* make_impl() { return new Impl; }

int local_alias()
{
    I* a = new Impl;
    I* b = a;
    return b->f();
}

int conditional(int c)
{
    I* a = c ? static_cast<I*>(new Impl) : static_cast<I*>(new Other);
    return a->f();
}

int from_return()
{
    I* p = make_impl();
    return p->f();
}
