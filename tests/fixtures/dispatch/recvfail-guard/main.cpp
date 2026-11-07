// Fixture for the implicit-this guard against receivers that are present
// but unresolved. Three shapes where the receiver expression is written in
// the source (an out-param dereference `(*pp_out)->AddRef()`, a subscript
// `slots[i]->AddRef()`, and a chained non-virtual getter `ref.get()`)
// must never let the bare-this fallback adopt the caller's own class as
// the static root - the runtime object is a different family
// implementation, so such a direct connect would be a same-family
// wrong-object edge. The dereference shape now resolves to the out-param
// slot (the write row types the slot with the stored implementation); the
// chained getter's non-virtual middle link is structurally unreachable in
// this libclang build, so the site carries the receiver-present marker and
// stays disclosed; the bare-this call (no receiver expression at all)
// keeps its wave of direct resolution.

struct IAcmeWidget
{
    virtual int AddRef() = 0;
    virtual ~IAcmeWidget() = default;
};

// The implementation family that actually lands in the out slot.
struct AcmeJsonWidget final : IAcmeWidget
{
    int AddRef() override { return 1; }
};

// A passthrough reference shell whose zero-parameter getter returns the
// own template parameter by reference.
template <class T>
struct AcmeRef
{
    T* ptr_ = nullptr;

    AcmeRef() = default;
    explicit AcmeRef(T* p) : ptr_{p} {}
    T& get() const noexcept { return *ptr_; }
};

// The calling class itself also implements the same root interface: an
// unguarded fallback would direct-connect the site straight to this
// override - the exact wrong-object shape the guard must prevent.
struct AcmeHostWidget : IAcmeWidget
{
    int AddRef() override { return 2; }

    // Shape one: write into the out slot, then call through the
    // dereferenced slot - the receiver resolves to the out-param slot and
    // the dynamic evidence converges on the stored implementation.
    int ProduceJson(IAcmeWidget** pp_out)
    {
        *pp_out = new AcmeJsonWidget();
        (*pp_out)->AddRef();
        return 0;
    }

    // Shape two (chained getter): the middle link is a non-virtual
    // template getter; its object child is not exposed by this libclang
    // build, so the receiver stays nameless and the site must remain
    // disclosed instead of falling back onto AcmeHostWidget.
    int ChainCall(AcmeRef<IAcmeWidget>& ref)
    {
        return ref.get().AddRef();
    }
};

// Shape three (subscript): the receiver is a slot of the pointer array;
// without an element row the site stays disclosed - it must not adopt
// AcmeHostWidget either.
int SubscriptCall(AcmeHostWidget& host, IAcmeWidget** slots, int i)
{
    (void)host;
    slots[i]->AddRef();
    return 0;
}

// Control: a bare-this call (no receiver expression at all) on a leaf
// class with a family-unique override keeps the direct resolution.
struct IAcmeSolo
{
    virtual int Poke() = 0;
    virtual ~IAcmeSolo() = default;
};

struct AcmeSoloHost final : IAcmeSolo
{
    int Poke() override { return 4; }
    int Run() { return Poke(); }
};

int main()
{
    AcmeHostWidget host{};
    IAcmeWidget*   out = nullptr;
    host.ProduceJson(&out);
    AcmeRef<IAcmeWidget> ref{out};
    int acc = host.ChainCall(ref);
    IAcmeWidget* slots[2] = {out, nullptr};
    acc += SubscriptCall(host, slots, 0);
    AcmeSoloHost solo{};
    acc += solo.Run();
    return acc > 0 ? 0 : 1;
}
