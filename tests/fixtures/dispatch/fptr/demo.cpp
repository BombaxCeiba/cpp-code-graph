struct I
{
    virtual int f();
    virtual ~I() = default;
};

int I::f() { return 0; }

int (I::*pmf)() = &I::f;

int g(I* p)
{
    (p->*pmf)();
    return 0;
}

int main()
{
    I* p = nullptr;
    return g(p);
}
