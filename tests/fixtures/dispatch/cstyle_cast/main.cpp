// C-style cast evidence fixture (fac6 SWIG diagnosis): generated code
// writes `(Impl*)x` uniformly; expr_type_keys must keep operand evidence
// and never adopt the cast target as dynamic evidence.
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
// Impl target must NOT be adopted - no positive edge may arise from here.
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
