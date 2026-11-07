// Fixture for the static-face priority over the out-param family-union
// band. The corpus shape: a receiver declared with a final concrete type
// is fed through a generic keyed fetch (an interface call whose out slot
// receives the union of everything the filling family stores - a may
// superset band; the union is family-level, so the guard arm and the
// control arm use separate fetch families). When the filling family
// never stores the receiver's own declared type, the union batch is
// entirely outside the declared face, the symmetric no-clip guard lets
// it through, and the slot's dynamic evidence conflicts head-on with the
// sealed static face. The static face (final syntax / no-descendant
// concrete class) is hard source evidence and must outrank the
// propagated union approximation: the site resolves via the static
// direct link instead of converging on an off-face implementation. The
// control arm keeps a genuine in-face dynamic evidence channel working
// (a partially in-face batch gets clipped to the in-face part, the
// consumer stays on the converged band).

struct IAcmeRoot
{
    virtual int Release() = 0;
    virtual ~IAcmeRoot() = default;
};

// The receiver's declared face: final concrete class.
struct AcmeSessionObj final : IAcmeRoot
{
    int Release() override { return 1; }
};

// Off-face implementations the union band may carry.
struct AcmeCoreSvc final : IAcmeRoot
{
    int Release() override { return 2; }
};

struct AcmeAltSvc final : IAcmeRoot
{
    int Release() override { return 3; }
};

// Guard-arm family: every implementor stores an off-face type - the
// whole batch stays off-face, the symmetric guard lets it through, and
// the polluted slot must lose to the sealed static face. The fetch out
// slot is typed at the wide root interface (the keyed-fetch corpus
// shape: the out slot's own face is the wide root, the receiver keeps
// the narrow declared face).
struct IAcmeOffFetch
{
    virtual void Fetch(IAcmeRoot** pp) = 0;
    virtual ~IAcmeOffFetch() = default;
};

struct AcmeCoreFetch final : IAcmeOffFetch
{
    void Fetch(IAcmeRoot** pp) override { *pp = new AcmeCoreSvc; }
};

// Control-arm family: one implementor stores the declared type, another
// stays off-face - the batch is partially in-face, gets clipped to the
// in-face part, and the consumer keeps its dynamic convergence.
struct IAcmeMixFetch
{
    virtual void Fetch(IAcmeRoot** pp) = 0;
    virtual ~IAcmeMixFetch() = default;
};

struct AcmeTrueFetch final : IAcmeMixFetch
{
    void Fetch(IAcmeRoot** pp) override { *pp = new AcmeSessionObj; }
};

struct AcmeAltFetch final : IAcmeMixFetch
{
    void Fetch(IAcmeRoot** pp) override { *pp = new AcmeAltSvc; }
};

// Guard arm: the off-face union meets a sealed declared face - the
// terminal state must be the static direct link to the declared type
// (whether the off-face batch got clipped away at the write-back or the
// static-priority guard pressed it back, the site never converges on the
// off-face implementation). The keyed-fetch corpus shape: the fetch out
// slot is typed at the wide root, the receiver keeps the narrow declared
// face.
int disconnect_off_face(IAcmeOffFetch* fetcher)
{
    AcmeSessionObj* session = nullptr;
    fetcher->Fetch(reinterpret_cast<IAcmeRoot**>(&session));
    return session->Release();
}

// Control arm: partially in-face batch - clipped, converged band stays.
int disconnect_mix_face(IAcmeMixFetch* fetcher)
{
    AcmeSessionObj* session = nullptr;
    fetcher->Fetch(reinterpret_cast<IAcmeRoot**>(&session));
    return session->Release();
}

int main()
{
    AcmeCoreFetch core{};
    AcmeTrueFetch tfetch{};
    AcmeAltFetch  afetch{};
    int           sink = 0;
    sink += disconnect_off_face(&core);
    sink += disconnect_mix_face(&tfetch);
    sink += disconnect_mix_face(&afetch);
    return sink == 0 ? 0 : 0;
}
