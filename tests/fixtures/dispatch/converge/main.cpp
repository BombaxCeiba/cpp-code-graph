// Nearest-override convergence fixture: dynamic types whose
// nearest overrides all converge onto one implementation upgrade 0.4 to
// 0.7 with the _nearest_converged reason suffix; a diverging control
// keeps 0.4 and never claims convergence.
struct NB
{
    virtual int w() { return 8; }
    virtual ~NB() = default;
};
struct ND1 : NB
{
};
struct ND2 : NB
{
};

struct SB
{
    virtual int v() = 0;
    virtual ~SB() = default;
};
struct S1 : SB
{
    int v() override { return 9; }
};
struct S2 : SB
{
    int v() override { return 10; }
};

// Single dynamic type: converges trivially, keeps >= 0.7 (direct new 1.0)
// and carries the suffix on its reason.
int probe_single()
{
    NB* p = new ND1;
    return p->w();
}

// Two dynamic types, neither overrides w: both converge onto NB::w.
int probe_converge(bool c)
{
    NB* p = c ? static_cast<NB*>(new ND1) : static_cast<NB*>(new ND2);
    return p->w();
}

// Control: two types with distinct overrides diverge - stays 0.4.
int probe_diverge(bool c)
{
    SB* p = c ? static_cast<SB*>(new S1) : static_cast<SB*>(new S2);
    return p->v();
}

int main()
{
    // No &obj arguments: address-of evidence would shadow the conditional
    // slot under test with a stronger single-type path.
    return probe_single() + probe_converge(true) + probe_diverge(false);
}
