// C-style cast evidence fixture (fac6 SWIG diagnosis): generated code
// writes `(Impl*)x` uniformly; expr_type_keys must keep operand evidence.
// Origin-unknown casts adopt the target as a claim (filled only when the
// slot ends up empty); use_downcast below keeps its
// operand-side evidence path intact.
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

// Assignment form: the RHS is a CStyleCastExpr wrapping `new Impl` - the
// operand evidence must survive the cast (local slot gets Impl, medium).
int use_new_assign()
{
    I* p = nullptr;
    p = (I*)(new Impl);
    return p->f();
}

// C-style downcast of a param: operand carries no dynamic evidence and the
// Impl target enters as a cast claim (filled only into an empty slot).
int use_downcast(I* q)
{
    Impl* d = (Impl*)q;
    return d->f();
}

int main()
{
    int r = use_new_assign();
    r += use_downcast(make());
    return r;
}
