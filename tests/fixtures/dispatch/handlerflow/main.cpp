// Fixture base for the handler parameter-flow seam shape, plus the
// prerequisite lambda shadowing gate: a local declared inside a lambda
// body with the same name as an outer parameter must not feed evidence
// into the outer slot, while capture-style references keep recording
// into the outer domain. The dereference-penetration matrix below
// covers the `*p` argument form: the deref shell is stripped so the
// operand name reaches the callee parameter slot through propagation.
// Sinks carry bodies so their parameter slots exist for assertions.

struct IService
{
    virtual int call() = 0;
    virtual ~IService() = default;
};

struct FakeSvc final : IService
{
    int call() override { return 1; }
};

struct OtherSvc final : IService
{
    int call() override { return 2; }
};

void use_svc(IService* s);

void handler_host(IService* ctx)
{
    // Capture positive: reference resolves to the outer parameter and
    // the argument name record stays in the outer domain.
    auto fwd = [ctx]() { use_svc(ctx); };
    (void)fwd;
    // Shadowing negative: inner declaration hides the outer name; the
    // outer ctx slot must not receive OtherSvc evidence and the
    // shadowed call must not record the name as an argument variable.
    auto shadow = [ctx]()
    {
        OtherSvc* ctx = new OtherSvc;
        use_svc(ctx);
        delete ctx;
    };
    (void)shadow;
}

// Handler body: interface reference parameter with a virtual call —
// the consuming side of the parameter flow.
int HandleSvc(IService& ctx, int payload) { return ctx.call() + payload; }

// Value-slice negative needs a polymorphic base with a concrete body.
struct SinkBase
{
    virtual int hit() { return 0; }
    virtual ~SinkBase() = default;
};
struct SinkDerived final : SinkBase
{
    int hit() override { return 3; }
};

void       SinkRef2(IService& s) { (void)s; }
void       SinkVal(SinkBase b) { (void)b; }
void       SinkInt(int v) { (void)v; }
void       SinkIntRef(int& v) { (void)v; }
static int square(int v) { return v * v; }

void handler_flow_host(
    IService*    ctx,
    SinkDerived* dp,
    int**        pp,
    int*         ip,
    int (*fp)(int))
{
    // Positive 1: lambda-captured parameter dereferenced as a direct
    // call argument — the capture reference resolves back to the outer
    // parameter, so the operand name joins the outer function domain.
    auto fwd = [ctx](int payload) { return HandleSvc(*ctx, payload); };
    (void)fwd(1);
    // Positive 2: the same chain through a second sink — a one-level
    // pointer to a record can only bind a reference (or value) callee
    // parameter under the four penetration criteria, never a raw
    // pointer parameter, so the gate's non-value arm is exercised with
    // a second reference sink.
    auto fwdp = [ctx]()
    {
        SinkRef2(*ctx);
        return 0;
    };
    (void)fwdp();
    auto neg = [dp, pp, ip, fp]()
    {
        // value slice: callee parameter is a value — the operand
        // slot must not leak derived types into a sliced copy.
        SinkVal(*dp);
        // double dereference: the inner operand points at a
        // pointer, not a record — no slot, no name.
        SinkIntRef(**pp);
        // non-record pointee: int has no record declaration.
        SinkInt(*ip);
        // function-pointer dereference on the callee side stays in
        // the indirect-target domain; no object slot evidence.
        (void)(*fp)(1);
        return 0;
    };
    (void)neg();
}

// Two fake callers union their local object types into the ctx
// parameter slot; the interface type itself never becomes a candidate.
void handler_flow_seed_fake()
{
    FakeSvc fake;
    handler_flow_host(&fake, nullptr, nullptr, nullptr, &square);
}

void handler_flow_seed_other()
{
    OtherSvc fake;
    handler_flow_host(&fake, nullptr, nullptr, nullptr, &square);
}

int main() { return 0; }
