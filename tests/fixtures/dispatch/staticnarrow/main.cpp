// DTA static narrowing fixture: an interface static type (has
// descendants AND a direct pure member) must NOT resolve; candidates are
// family intersect subtree at Low(0.4), display-only - never dispatch
// edges. The sibling interface JB probes that subtree filtering actually
// excludes implementations living outside the receiver's subtree.

struct IB
{
    virtual int f() = 0;
    virtual ~IB() = default;
};
struct MidB : IB
{
    int f() override { return 6; }
};
struct XB : MidB
{
    int f() override { return 7; }
};

// Sibling interface: XJ::f must never leak into an IB-typed site's
// candidate set (negative control for the subtree filter).
struct JB
{
    virtual int f() = 0;
    virtual ~JB() = default;
};
struct XJ : JB
{
    int f() override { return 9; }
};

int probe_iface()
{
    IB* p = nullptr;
    return p->f();
}
// Mid-level static type: the method itself (MidB::f) is excluded from the
// candidates; only XB::f remains.
int probe_mid()
{
    MidB* p = nullptr;
    return p->f();
}
int probe_sibling()
{
    JB* p = nullptr;
    return p->f();
}

int main() { return 0; }
