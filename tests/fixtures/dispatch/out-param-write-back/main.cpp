// Fixture for the out-param write-back collection seams. Form 1 (the
// tool write-back): a factory delegates its `*pp = value` write to a
// helper taking the value and the out slot - the helper is a template
// instantiated at the call, its body is not the factory's own
// dereference write, so the factory's out-parameter slot never received
// a write row and the consumer's write-back never fired. The call site
// now collects the row under two structural gates: the callee parameter
// must be a two-level pointer to a record (the direction gate) and the
// value's pointee record must derive from (or equal) that record (the
// pairing gate); the value source is the template argument of the shell
// factory call. Form 2 (the address-of write-back): the write goes
// through an out-param wrapper shell dereferenced at the write site
// (`*out.Put() = impl`); the row lands on the slot bound at the shell's
// construction. Negative: a value whose record does not derive the out
// pointee never locks the slot. All criteria are pure AST shapes.

struct ISvc
{
    virtual int Work() = 0;
    virtual ~ISvc() = default;
};

struct AcmeSvc final : ISvc
{
    int Work() override { return 11; }
};

struct Unrelated
{
    int x = 0;
};

template <class T>
class AcmeBox
{
    T* ptr_ = nullptr;

public:
    AcmeBox() = default;
    explicit AcmeBox(T* p) : ptr_{p} {}
    T* operator->() const { return ptr_; }
    T* Get() const { return ptr_; }
};

template <class T>
AcmeBox<T> MakeAcmeBox()
{
    return AcmeBox<T>(new T());
}

// The write-back tool: value first, out slot second; the direction is
// carried by the second parameter's shape (record double pointer).
template <class R, class T>
void AcmeSetResult(R&& result, T* p_result)
{
    (*p_result) = result.Get();
}

// A park-only helper with the same shape: the pairing gate must accept
// the deriving value (the write claim is structural, not body-based).
template <class R, class T>
void AcmeParkResult(R&& result, T* p_result)
{
    (void)result;
    (void)p_result;
}

void AcmeCreateSvc(int flags, ISvc** pp_out)
{
    AcmeSetResult(MakeAcmeBox<AcmeSvc>(), pp_out);
    (void)flags;
}

int ConsumeTool()
{
    ISvc* p = nullptr;
    AcmeCreateSvc(1, &p);
    return p->Work(); // converges to AcmeSvc::Work
}

// Negative: the value record does not derive the out pointee - the
// pairing gate rejects, no write row, the consumer keeps its fallback.
void AcmeCreateUnrelated(int flags, ISvc** pp_out)
{
    *pp_out = nullptr;
    AcmeParkResult(MakeAcmeBox<Unrelated>(), pp_out);
    (void)flags;
}

// Negative for the depth half of the direction gate: a triple-level
// pointer parameter is an out-out shape (a slot that receives a slot),
// never a direct write-back slot - even with a deriving value at the
// other position the depth gate must reject the pairing, so no
// out_param_tool row may fire for the triple-level slot.
template <class R>
void AcmeDeepPark(R&& result, ISvc*** ppp_slot)
{
    (void)result;
    (void)ppp_slot;
}

void AcmeCreateDeep(int flags, ISvc*** ppp_out)
{
    AcmeDeepPark(MakeAcmeBox<AcmeSvc>(), ppp_out);
    (void)flags;
}

int ConsumeDeep()
{
    ISvc** pp = nullptr;
    AcmeCreateDeep(1, &pp);
    if (pp == nullptr)
    {
        return 0;
    }
    return (*pp)->Work(); // deref receiver: element stays unproven
}

int ConsumeUnrelated()
{
    ISvc* p = nullptr;
    AcmeCreateUnrelated(1, &p);
    if (p == nullptr)
    {
        return 0;
    }
    return p->Work(); // stays fallback: no tool row for this slot
}

// Form 2: the out-param wrapper shell (double-pointer field, binding
// constructor, single-parameter writeback method, arrow accessor).
template <class T>
class AcmeOutBox
{
    T** pp_;

public:
    explicit AcmeOutBox(T** pp) : pp_{pp} {}
    T** Put() { return pp_; }
    void Set(T* p) { *pp_ = p; }
    T* operator->() const { return *pp_; }
};

int AcmeCreateSvcOut(int flags, ISvc** pp_out)
{
    AcmeOutBox<ISvc> out(pp_out);
    auto*            impl = new AcmeSvc();
    *out.Put() = impl; // the address-of write-back seam
    return flags;
}

int ConsumeOutBox()
{
    ISvc* p = nullptr;
    AcmeCreateSvcOut(2, &p);
    return p->Work(); // converges to AcmeSvc::Work
}

int main()
{
    int acc = ConsumeTool();
    acc += ConsumeOutBox();
    acc += ConsumeUnrelated();
    acc += ConsumeDeep();
    return acc > 0 ? 0 : 1;
}
