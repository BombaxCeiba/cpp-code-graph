// Fixture for the return-slot shell passthrough seam (the `return
// shell;` relay shape): a function returning a pointer shell by copying
// a local shell variable is a copy/move construction at the return
// statement - the construction penetration arm has no T* parameter to
// recurse into, so the return slot used to receive no row at all and
// the consumer's return-flow edge stayed empty (family_ambiguous). The
// copy-source alias edge now points the return slot at the source
// variable slot, so the producing factory's evidence (DirectNew through
// the static Attach factory) survives the relay hop. Brace construction
// of a shell from a pointer penetrates the single element. Negative
// matrix: a two-implementation merge stays a disclosed may (no unique
// edge), a non-template shell keeps the legacy aggregate path, and a
// plain record return is unchanged. All criteria are pure AST shapes -
// no gate consults a name.

struct ISession
{
    virtual int Id() = 0;
    virtual ~ISession() = default;
};

struct AcmeSession final : ISession
{
    int Id() override { return 7; }
};

struct AcmeAltSession final : ISession
{
    int Id() override { return 9; }
};

template <class T>
class AcmeHandle
{
    T* ptr_ = nullptr;

public:
    AcmeHandle() = default;
    explicit AcmeHandle(T* p) : ptr_{p} {}
    T* operator->() const { return ptr_; }
    T* Get() const { return ptr_; }
    static AcmeHandle Attach(T* p) { return AcmeHandle{p}; }
};

AcmeHandle<ISession> ProduceSession()
{
    return AcmeHandle<ISession>::Attach(new AcmeSession());
}

// Relay: return of a shell local by name (the copy/move construction
// shape - the seam under test).
AcmeHandle<ISession> RelaySession()
{
    AcmeHandle<ISession> ctx = ProduceSession();
    return ctx;
}

// Copy-initialization of a shell from a shell variable: the same alias
// semantics on the copy's slot.
int UseLocalCopy()
{
    AcmeHandle<ISession> a = ProduceSession();
    AcmeHandle<ISession> b = a;
    return b->Id(); // converges to AcmeSession::Id
}

int ConsumeRelay()
{
    AcmeHandle<ISession> s = RelaySession();
    return s->Id(); // converges to AcmeSession::Id
}

// Two-implementation merge: the relay unions two producers - the site
// must stay a disclosed may with both candidates and no unique edge.
AcmeHandle<ISession> RelayAmbiguous(bool pick_alt)
{
    AcmeHandle<ISession> first  = ProduceSession();
    AcmeHandle<ISession> second = AcmeHandle<ISession>::Attach(
        new AcmeAltSession());
    return pick_alt ? second : first;
}

int ConsumeAmbiguous(bool pick_alt)
{
    AcmeHandle<ISession> s = RelayAmbiguous(pick_alt);
    return s->Id(); // may: AcmeSession + AcmeAltSession
}

// Brace construction of a shell from a pointer: penetration through the
// single element (the shell's own static type never enters the slot).
int ConsumeBrace()
{
    AcmeHandle<ISession> s{new AcmeSession()};
    return s->Id(); // converges to AcmeSession::Id
}

struct PlainShell // non-template shell: outside the template gate
{
    ISession* p_ = nullptr;
    ISession* operator->() const { return p_; }
};

PlainShell RelayPlain()
{
    PlainShell out;
    out.p_ = new AcmeSession();
    return out; // legacy path unchanged, no crash
}

int ConsumePlain()
{
    PlainShell s = RelayPlain();
    return s->Id(); // unchanged legacy band
}

struct PlainRecord
{
    int v = 3;
};

PlainRecord MakeRecord()
{
    return PlainRecord{};
}

int main()
{
    int acc = ConsumeRelay();
    acc += UseLocalCopy();
    ConsumeAmbiguous(acc % 2 == 0);
    acc += ConsumeBrace();
    acc += ConsumePlain();
    const PlainRecord rec = MakeRecord();
    acc += rec.v;
    return acc > 0 ? 0 : 1;
}
