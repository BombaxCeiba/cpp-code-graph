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
    I*   p = nullptr;
    void bind() { this->p = new Impl; }
    int  run() { return this->p->f(); }
};

int main()
{
    S s;
    s.bind();
    return s.run();
}
