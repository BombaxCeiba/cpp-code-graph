// Fixture for the static-method shell-factory seam (the Attach shape):
// a pointer-shell class template exposes a static factory taking exactly
// one T* parameter and returning the shell by value. Six consumption
// sites assert that the pointee evidence of the T* argument
// flows into the shell slot and reaches the virtual dispatch; the
// negative matrix asserts the gates stay closed: two T* parameters, no
// T* parameter, value parameter, non-shell return, non-static method,
// non-template shell, free factory and a no-arrow shell. All criteria
// are pure AST shapes - no gate consults a name. Sinks carry bodies so
// their slots exist for assertions.

#include <limits>

struct IFoo
{
    virtual void Virt() = 0;
    virtual ~IFoo() = default;
};

struct ImplFoo final : IFoo
{
    void Virt() override {}
};

template <class T>
class Box
{
    T* ptr_ = nullptr;

public:
    T*         operator->() const { return ptr_; }
    static Box Attach(T* p)
    {
        Box b;
        b.ptr_ = p;
        return b;
    }
    // Multi-argument factory with exactly one T* slot: positive.
    static Box Wrap(T* p, int flags)
    {
        Box b;
        b.ptr_ = p;
        (void)flags;
        return b;
    }
    static int Count() { return 0; } // non-shell return
    static Box FromValue(T v)
    {
        (void)v;
        return Box();
    } // value param
    Box() = default;
    Box(T* p) : ptr_(p) {}
    Box Clone() const { return *this; } // non-static
};

template <class T>
class TwoPtr // two T* parameters: no provable unique pointee slot
{
    T* a_ = nullptr;

public:
    T*            operator->() const { return a_; }
    static TwoPtr Attach(T* x, T* y)
    {
        (void)x;
        (void)y;
        return TwoPtr();
    }
};

template <class T>
class FromOnly // no T* parameter
{
    T* p_ = nullptr;

public:
    T*              operator->() const { return p_; }
    static FromOnly Create() { return FromOnly(); }
};

template <class T>
class NoArrow // no operator->: not a pointer shell
{
    T* p_ = nullptr;

public:
    static NoArrow Attach(T* p)
    {
        (void)p;
        return NoArrow();
    }
    T* Get() const { return p_; }
};

class PlainShell // non-template shell: outside the template gate
{
    IFoo* p_ = nullptr;

public:
    IFoo*             operator->() const { return p_; }
    static PlainShell Attach(IFoo* p)
    {
        (void)p;
        return PlainShell();
    }
};

template <class T>
Box<T> MakeBox(T* p)
{
    return Box<T>(p);
} // free factory: rejected

Box<IFoo> ret_shell(IFoo* p)
{
    return Box<IFoo>::Attach(p); // return slot
}

struct Holder
{
    Box<IFoo> f_;
    explicit Holder(IFoo* p)
        : f_{Box<IFoo>::Attach(p)} // ctor-init field store
    {
    }
    void refill(IFoo* p) { f_ = Box<IFoo>::Attach(p); } // field operator= store
    void use() { f_->Virt(); }
};

void attach_shell_host(IFoo* raw)
{
    // local initialization from the factory.
    auto s = Box<IFoo>::Attach(raw);
    s->Virt();
    // chained factory call as the virtual receiver.
    Box<IFoo>::Attach(raw)->Virt();
    // operator= store into a default-constructed shell.
    Box<IFoo> t;
    t = Box<IFoo>::Attach(raw);
    t->Virt();
    // positive: multi-argument factory, single T* at position 0.
    Box<IFoo> u = Box<IFoo>::Wrap(raw, 1);
    u->Virt();
    // two T* parameters -> reject, no pointee evidence for tp.
    TwoPtr<IFoo> tp = TwoPtr<IFoo>::Attach(raw, raw);
    // no T* parameter -> reject.
    FromOnly<IFoo> fo = FromOnly<IFoo>::Create();
    // no operator-> -> not a shell, reject.
    NoArrow<IFoo> na = NoArrow<IFoo>::Attach(raw);
    // non-template shell -> outside the gate, existing paths.
    PlainShell ps = PlainShell::Attach(raw);
    // free factory -> reject (semantic parent is no record).
    Box<IFoo> mb = MakeBox(raw);
    // value parameter -> reject, the operand must not feed fv (a
    // concrete type instantiates the value slot: the interface itself
    // is abstract and cannot pass by value).
    ImplFoo      direct;
    Box<ImplFoo> fv = Box<ImplFoo>::FromValue(direct);
    // non-shell return -> no evidence channel.
    int counted = Box<IFoo>::Count();
    // non-static method returning a shell -> reject.
    Box<IFoo> cl;
    Box<IFoo> cloned = cl.Clone();
    // Interference source: static method returning an own template
    // parameter scalar - stays rejected.
    int lim = std::numeric_limits<int>::max();
    (void)tp;
    (void)fo;
    (void)na;
    (void)ps;
    (void)mb;
    (void)fv;
    (void)counted;
    (void)cloned;
    (void)lim;
}

int main()
{
    ImplFoo impl;
    IFoo*   raw = &impl;
    attach_shell_host(raw);
    Holder holder(raw);
    holder.use();
    holder.refill(raw);
    holder.use();
    // The return-slot consuming side: the returned shell carries the
    // argument's pointee evidence through the return slot.
    Box<IFoo> rs = ret_shell(raw);
    rs->Virt();
    return 0;
}
