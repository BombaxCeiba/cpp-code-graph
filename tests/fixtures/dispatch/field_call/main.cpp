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

I* make() { return new Impl; }

struct S
{
    I*   p = nullptr;
    void bind() { p = make(); }
    int  run() { return p->f(); }
};

int main()
{
    S s;
    s.bind();
    return s.run();
}
