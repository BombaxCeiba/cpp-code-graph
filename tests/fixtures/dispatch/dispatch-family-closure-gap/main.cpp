// Fixture for the family_unique_override closure-completeness guard. The
// override closure is anchored on method rows: when an interface is
// inherited through a template middle layer that never redeclares the
// method, the template implementors (dependent-base context) never get
// their overrides edges collected, so their method rows stay outside the
// closure and "the library-unique non-pure override" becomes an
// observation artifact. Two guard arms mirror the corpus hit that pinned
// this shape: the closure-gap arm (a named interface receiver whose
// runtime target set spans the hidden template implementors) and the
// indirect-receiver arm (a subscripted container element). A closed-form
// control (a genuinely unique implementation with no template middle
// layer) keeps its direct resolution. The receivers stay null so no
// dynamic type evidence flows into their slots - both arms exercise the
// static side where the guarded branch lives.

// ---- The vulnerable family: interface + template middle layer ----

struct IAcmeDescriber
{
    virtual int Describe() const = 0;
    virtual ~IAcmeDescriber() = default;
};

// The middle layer never redeclares Describe(): implementations below it
// are templates whose dependent base keeps the override edge uncollected.
template <class T>
struct AcmeMatcherFace : IAcmeDescriber
{
};

// The only non-pure member the closure can see (a direct implementor).
struct AcmeValueFace final : IAcmeDescriber
{
    int Describe() const override { return 1; }
};

// Hidden implementors: same method row name, override edge structurally
// missing through the dependent middle layer - the closure-gap evidence.
template <class T>
struct AcmeRedFace final : AcmeMatcherFace<T>
{
    int Describe() const override { return 2; }
};

template <class T>
struct AcmeBlueFace final : AcmeMatcherFace<T>
{
    int Describe() const override { return 3; }
};

// Closure-gap arm: a named interface receiver - the unique-override
// argument would direct-connect to AcmeValueFace, but the runtime target
// set also spans the hidden red/blue implementors, so the branch must
// stay blocked (disclosed with the gap reason).
int describe_one(const IAcmeDescriber* item)
{
    return item->Describe();
}

// Indirect-receiver arm: a subscripted container element - the receiver
// is an element slot with no per-element row; even with a closure that
// looks unique, an indirect receiver must not be adopted at high
// confidence on this branch.
int describe_all(const IAcmeDescriber* const* items, int n)
{
    int acc = 0;
    for (int i = 0; i < n; ++i)
    {
        acc += items[i]->Describe();
    }
    return acc;
}

// ---- Closed-form control: a genuinely unique implementation (no middle
// layer, closure complete) keeps the abstract-host direct link. ----

struct IAcmeStamp
{
    virtual int Stamp() const = 0;
    virtual ~IAcmeStamp() = default;
};

struct AcmeStampLeaf final : IAcmeStamp
{
    int Stamp() const override { return 5; }
};

int stamp_one(const IAcmeStamp* s)
{
    return s->Stamp();
}

// Instantiation anchors: force the template specialization rows (and the
// folded subclass links) into the index without flowing any instance
// type into the receiver slots above.
int main()
{
    static_cast<void>(sizeof(AcmeRedFace<int>));
    static_cast<void>(sizeof(AcmeBlueFace<long>));
    static_cast<void>(sizeof(AcmeValueFace));
    static_cast<void>(sizeof(AcmeStampLeaf));

    const IAcmeDescriber* items[3] = {nullptr, nullptr, nullptr};
    const IAcmeDescriber* one = nullptr;
    const IAcmeStamp*    stamp = nullptr;
    int                  sink = 0;
    sink += describe_one(one);
    sink += describe_all(items, 3);
    sink += stamp_one(stamp);
    return sink == 0 ? 0 : 0;
}
