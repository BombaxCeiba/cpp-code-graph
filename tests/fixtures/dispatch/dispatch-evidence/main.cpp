// Dispatch-evidence fixture: implicit-this resolution, transparent
// forwarder paradigm, write-back union, shell-move field transport.
// Every negative form carries a distinct probe function so the tests can
// assert refusals (prefer missing edges over guessed ones).

#include <utility>

// ---- Item 1: bare-this (implicit receiver) virtual call resolution ------
struct IQi
{
    virtual int AddRef() = 0;
    virtual int Release() = 0;
    virtual ~IQi() = default;
};
struct DirectorLeaf : IQi
{
    int QueryInterface();
    int AddRef() override { return 1; }
    int Release() override { return 2; }
};
// Bare AddRef() inside QueryInterface has no receiver expression: the
// receiver static type falls back to the caller's record (a leaf), so the
// multi-impl family still resolves via nearest-override at 1.0
// (reason=implicit_this_unique).
int DirectorLeaf::QueryInterface()
{
    AddRef();
    return 0;
}

// Negative: template record caller - instantiations disagree on the
// dynamic type, the implicit-this fallback must refuse (no 1.0 edge).
struct TplHost
{
    virtual int virt();
    virtual ~TplHost() = default;
};
struct TplDerived : TplHost
{
    int virt() override;
};
template <typename T>
struct TplCaller : TplHost
{
    int call() { return virt(); } // implicit this, template record
};
int tpl_instantiation()
{
    TplCaller<TplDerived> t;
    return t.call();
}

// ---- Item 3: transparent forwarder paradigm (zero name whitelist) ------
struct IFwd
{
    virtual int f();
    virtual ~IFwd() = default;
};
struct FwdImpl : IFwd
{
    int f() override;
};
int IFwd::f() { return 0; }
int FwdImpl::f() { return 1; }

// Custom non-std forwarders: body is exactly one static_cast-to-reference
// of the parameter and the return type is a reference -> every call site
// is the argument itself, regardless of the name.
int& hand_off(int& v) { return static_cast<int&>(v); }
template <typename T>
T&& pipe(T&& t)
{
    return static_cast<T&&>(t);
}

// Negative: value-returning conversion is NOT a transparent forwarder
// (returns a copy, not the object) - must be refused.
double by_value(double v) { return static_cast<double>(v); }

void take_pipe(IFwd* q) { q->f(); }

// Argument transport via the custom rvalue forwarder: take_pipe(pipe(p))
// must see p's slot (transparent), exactly like std::move.
int rvalue_forwarder_probe()
{
    FwdImpl* p = new FwdImpl;
    take_pipe(pipe(p));
    take_pipe(std::move(p));
    return 0;
}

// Lvalue forwarder: the call site resolves to the argument variable.
int lvalue_forwarder_probe()
{
    int  x = 0;
    int& r = hand_off(x);
    return r;
}

// Negative site: by_value(v) nested as an argument keeps the
// call-expression form (the outer site's arg_vars stays empty - a
// value-returning conversion is not a forwarder, no argument extraction).
double consume_by_value(double w);
double value_cast_negative()
{
    double v = 1.5;
    return consume_by_value(by_value(v));
}

// ---- Item 2: write-back value union into the out-param slot -----------
// Two write-back sites (*pp = a; *pp = b;) feed one out-param slot: the
// union must keep BOTH dynamic types (the first site no longer swallows
// the second one).
struct IOut
{
    virtual int g();
    virtual ~IOut() = default;
};
struct OutA : IOut
{
    int g() override;
};
struct OutB : IOut
{
    int g() override;
};
int IOut::g() { return 0; }
int OutA::g() { return 1; }
int OutB::g() { return 2; }

void fill_two_writes(IOut** pp, bool which)
{
    IOut* a = new OutA;
    IOut* b = new OutB;
    if (which)
    {
        *pp = a;
    }
    else
    {
        *pp = b;
    }
}

int consume_two_writes()
{
    IOut* r = nullptr;
    fill_two_writes(&r, true);
    return r->g();
}

// ---- Item 4: shell-move field transport (field slot keeps pointee) ----
template <typename T>
struct PtrShell
{
    T* p_ = nullptr;
    PtrShell() = default;
    explicit PtrShell(T* p) : p_(p) {}
    PtrShell(PtrShell&& other) noexcept : p_(other.p_) { other.p_ = nullptr; }
    PtrShell& operator=(PtrShell&& other) noexcept
    {
        if (this != &other)
        {
            p_ = other.p_;
            other.p_ = nullptr;
        }
        return *this;
    }
    T* operator->() { return p_; }
    T* Get() const { return p_; }
};

struct HolderMoveField
{
    PtrShell<IFwd> f_{};
    int            use() { return f_->f(); }
};

int field_move_transport_probe()
{
    HolderMoveField h;
    PtrShell<IFwd>  s{new FwdImpl};
    h.f_ = std::move(s); // item 4: move-wrapped shell -> field slot
    return h.use();
}

// Negative for item 4: non-field LHS (local variable) is NOT a field
// transport - nothing may leak into the field slot from here; the local
// slot path (LocalCopy) is the correct channel.
int local_move_probe()
{
    PtrShell<IFwd> a{new FwdImpl};
    PtrShell<IFwd> b;
    b = std::move(a);
    return b.Get() != nullptr ? 0 : 1;
}

int main()
{
    DirectorLeaf d;
    (void)d.QueryInterface();
    (void)tpl_instantiation();
    (void)rvalue_forwarder_probe();
    (void)lvalue_forwarder_probe();
    (void)value_cast_negative();
    (void)consume_two_writes();
    (void)field_move_transport_probe();
    (void)local_move_probe();
    return 0;
}
