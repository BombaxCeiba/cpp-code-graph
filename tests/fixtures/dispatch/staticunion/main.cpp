// Multi-static-type union fixture: the test rewrites
// probe_union's call_sites.receiver_types to the two leaves [L1, L2] after
// indexing - the stale-merge shape an incremental run leaves behind - so
// dispatch must narrow over the UNION of both static roots' subtrees
// instead of looking only at the front type.
struct IB2
{
    virtual int f() = 0;
    virtual ~IB2() = default;
};
struct L1 : IB2
{
    int f() override { return 1; }
};
struct L2 : IB2
{
    int f() override { return 2; }
};

int probe_union(IB2* p) { return p->f(); }

int main() { return 0; }
