// Fixture base for the constructor-initializer seam shape, plus the
// prerequisite lambda shadowing gate: a local declared inside a lambda
// body with the same name as an outer name must not feed any evidence
// into the outer slot (declared rows, direct-new rows, argument name
// records), while capture-style references to the outer parameter keep
// recording into the outer domain. The constructor-initializer positive
// matrix below covers the plain (non-shell) field arm: colon-initializer
// MemberRef pairing with the assignment-style RHS evidence triplet.
// Each shape lives in its own record type because field slots union at
// record level; shapes must not share records or assertions pollute.

struct IDevice
{
    virtual int poke() = 0;
    virtual ~IDevice() = default;
};

struct RealDev final : IDevice
{
    int poke() override { return 1; }
};

struct LocalDev final : IDevice
{
    int poke() override { return 2; }
};

void take_dev(IDevice* d);

struct Holder
{
    IDevice* dev_;
    explicit Holder(IDevice* dev) : dev_(dev) {}
    int poke_later() { return dev_->poke(); }
};

void shadow_host(IDevice* ctx)
{
    Holder held{ctx};
    // Capture positive: the reference resolves back to the outer
    // parameter and stays recorded in the outer domain.
    auto fwd = [ctx]() { take_dev(ctx); };
    (void)fwd;
    // Shadowing negative: the inner declaration hides the outer name;
    // the outer ctx slot must not receive LocalDev evidence and the
    // shadowed call must not record the name as an argument variable.
    auto shadow = [ctx]()
    {
        LocalDev* ctx = new LocalDev;
        take_dev(ctx);
        delete ctx;
    };
    (void)shadow;
    (void)held.poke_later();
}

// Positive 1: plain pointer field bound to the constructor parameter,
// virtual call through the field must connect via the ctor-init alias.
struct PlainHolder
{
    IDevice* f_;
    explicit PlainHolder(IDevice* p) : f_(p) {}
    int use() { return f_->poke(); }
};
int ctorinit_plain_pointer()
{
    PlainHolder h{new RealDev};
    return h.use();
}

// Positive 2: reference member — the language forces the colon
// initializer to be the only write point for reference members.
struct RefHolder
{
    IDevice& r_;
    explicit RefHolder(IDevice& r) : r_(r) {}
    int use() { return r_.poke(); }
};
int ctorinit_plain_reference()
{
    RealDev   d;
    RefHolder h{d};
    return h.use();
}

// Positive 3a: const pointer member with brace form.
struct ConstBraceHolder
{
    IDevice* const c_;
    explicit ConstBraceHolder(IDevice* p) : c_{p} {}
    int use() { return c_->poke(); }
};
int ctorinit_plain_const_brace()
{
    ConstBraceHolder h{new RealDev};
    return h.use();
}

// Positive 3b: shadowed name — parameter spelled exactly like the
// field; the RHS reference must resolve to the ParmDecl domain, never
// to the field slot itself (reference identity, not spelling match).
struct ShadowHolder
{
    IDevice* image_;
    explicit ShadowHolder(IDevice* image_) : image_(image_) {}
    int use() { return image_->poke(); }
};
int ctorinit_plain_shadow_name()
{
    ShadowHolder h{new RealDev};
    return h.use();
}

// Positive 4: ctor-init plus a function-body write — row-level
// multi-source union keeps both candidates low, never a unique edge.
// The body write flows through a field-to-field alias so the record
// domain resolves; a bare parameter name on the body path stays a dead
// record-domain edge under existing semantics (out of batch scope).
struct DualWriteHolder
{
    IDevice* f_;
    IDevice* g_;
    DualWriteHolder(IDevice* a, IDevice* b) : f_(a), g_(b) { f_ = g_; }
    int use() { return f_->poke(); }
};
int ctorinit_plain_dual_write()
{
    RealDev         a;
    LocalDev        b;
    DualWriteHolder h{&a, &b};
    return h.use();
}

// Negative 1: base and delegating initialization are TypeRef shapes —
// naturally excluded from the MemberRef pairing.
struct CtorBase
{
    explicit CtorBase(int);
};
struct CtorDerived final : CtorBase
{
    IDevice* f_ = nullptr;
    explicit CtorDerived(IDevice* p, int x) : CtorBase(x), f_(p) {}
    CtorDerived() : CtorDerived(nullptr, 42) {}
    int use() { return f_ ? f_->poke() : 0; }
};
int ctorinit_negative_base_delegate()
{
    CtorDerived d{new RealDev, 1};
    return d.use();
}

// Negative 2: array and literal RHS — all three evidence channels stay
// empty, nothing is emitted (prefer missing over guessing).
struct OpaqueHolder
{
    IDevice* arr_[2];
    int      n_;
    explicit OpaqueHolder(IDevice* p, IDevice* q) : arr_{p, q}, n_(5) {}
};
int ctorinit_negative_opaque_rhs()
{
    RealDev      a;
    LocalDev     b;
    OpaqueHolder h{&a, &b};
    return h.n_;
}

// Negative 3: in-class default member initializer never goes through
// the constructor pairing path (scope locked for this batch).
struct NsdmiHolder
{
    IDevice* nsd_ = nullptr;
    int      use() { return nsd_ ? nsd_->poke() : 0; }
};
int ctorinit_negative_nsdmi()
{
    NsdmiHolder h;
    return h.use();
}

// Negative 4: multi-argument value field — the whole constructor call
// is the RHS; no per-argument split.
struct PairVal
{
    PairVal(int x, int y);
};
struct MultiArgHolder
{
    PairVal v_;
    explicit MultiArgHolder(int x, int y) : v_(x, y) {}
};
int ctorinit_negative_multi_arg()
{
    MultiArgHolder h{1, 2};
    (void)h;
    return 0;
}

int main() { return 0; }
