// Dependent-receiver member calls (field corpus): a member access on a
// template-parameter receiver must never produce a named call edge, even
// when a same-name member exists elsewhere in the TU (the hallucination
// condition behind a fake Acme::IsFailed -> sol2 dump_error::error_code
// edge). Implicit-this calls inside a class template stay untouched.
struct Phantom
{
    int error_code() const;
};

int Phantom::error_code() const { return 0; }

// Same-name member in the TU (sol2 stand-in): engines that hallucinate the
// dependent reference must still yield no named edge (guard on receiver
// type, not on referenced).
template <class T>
bool probe_call(const T& t)
{
    return t.error_code() < 0;
}

// Non-call member read: same dependent shape, never a call site at all.
template <class T>
bool probe_field(const T& t)
{
    return t.error_code > 0;
}

// Class template: implicit-this call targets the class's own pattern
// method (a real symbol) - the guard must keep it; the dependent receiver
// t.go() must be skipped. The explicit `this->helper()` form must survive
// too: this has type W<T>* (a formed template, not a bare parameter).
template <class T>
struct W
{
    void helper();
    void run(T& t)
    {
        helper();
        this->helper();
        t.go();
    }
};

template <class T>
void W<T>::helper()
{
}

struct Concrete
{
    bool go();
    // Field (not a call): the instantiation target for probe_field's
    // dependent member READ - two-phase lookup needs the member to exist
    // on the instantiated type for the TU to parse at all.
    int error_code = 0;
    virtual ~Concrete() = default;
};

bool Concrete::go() { return true; }

int use_all(W<Concrete>& w, Concrete& c)
{
    w.run(c);
    return c.go() ? 1 : 0;
}

int main()
{
    Concrete    c;
    W<Concrete> w;
    Phantom     p;
    // probe_call instantiates with Phantom (its error_code() member
    // function exists); inside the template body t.error_code() is still a
    // dependent-receiver expression regardless of the instantiated type.
    const int r = (probe_call(p) || probe_field(c)) ? use_all(w, c) : 0;
    (void)p.error_code();
    return r;
}
