// Fixture for the iid lock seam: an identity token (a zero-value-arg
// template call with a single explicit record template argument, passed
// at a record-valued parameter position) paired with an out-direction
// parameter (two peelable pointer layers, any pointee - the void
// double-pointer of a QueryInterface shape qualifies) locks the out
// slot to the interface named by the token. The interface-level type
// would flow into the slot as evidence and as the provable static face.
// The row emission is currently fused: the full real-library index
// reproducibly segfaulted in the post-propagation write phase with the
// rows on and ran clean with them off, so by the prefer-missing-over-
// guessing discipline the two rows stay unemitted (criteria kept, see
// the fuse note in the collector); the consumer sites therefore keep
// their legacy bands. The member-template form (recv.As<Iface>(token)
// returning Iface*) is fused the same way. Negative: a concrete
// (non-interface) template argument and a non-token argument keep the
// gates closed. All criteria are pure AST shapes - no gate consults a
// name.

#include <memory>

struct AcmeGuid
{
    unsigned long long lo;
    unsigned long long hi;
};

template <class T>
AcmeGuid AcmeIidOf()
{
    return AcmeGuid{1ull, 2ull};
}

struct IBackend
{
    virtual int Mat() = 0;
    virtual ~IBackend() = default;
};

struct CpuBackend final : IBackend
{
    int Mat() override { return 21; }
};

struct IWide // two implementations: stays a may
{
    virtual int Wide() = 0;
    virtual ~IWide() = default;
};

struct WideA final : IWide
{
    int Wide() override { return 1; }
};

struct WideB final : IWide
{
    int Wide() override { return 2; }
};

struct IProduct
{
    virtual int QueryInterface(const AcmeGuid& iid, void** out) = 0;
    virtual ~IProduct() = default;
};

struct ProductImpl final : IProduct
{
    int QueryInterface(const AcmeGuid& iid, void** out) override
    {
        if (iid.lo == 1)
        {
            backend_.reset(new CpuBackend());
            *out = backend_.get();
        }
        return 0;
    }

private:
    std::unique_ptr<IBackend> backend_;
};

// Single-implementation interface: the lock converges the consumer to
// the unique implementation.
int ConsumeQI(IProduct* product)
{
    void* p_backend = nullptr;
    product->QueryInterface(AcmeIidOf<IBackend>(), &p_backend);
    auto* backend = static_cast<IBackend*>(p_backend);
    return backend->Mat(); // converges to CpuBackend::Mat
}

// Two-implementation interface: the lock discloses both candidates and
// never fabricates a unique edge.
int ConsumeWideQI(IProduct* product)
{
    void* p_wide = nullptr;
    product->QueryInterface(AcmeIidOf<IWide>(), &p_wide);
    auto* wide = static_cast<IWide*>(p_wide);
    return wide->Wide(); // may: WideA::Wide + WideB::Wide
}

// Negative: no identity token at the record parameter - no lock, the
// site keeps its legacy band.
int ConsumeNoToken(IProduct* product)
{
    void* p_backend = nullptr;
    product->QueryInterface(AcmeGuid{3, 4}, &p_backend);
    auto* backend = static_cast<IBackend*>(p_backend);
    return backend->Mat(); // no qi row for this slot
}

// The member-template form: As returns the interface pointer named by
// its own template argument, with the token argument naming the same
// interface - the result slot is locked to that interface.
template <class T>
class AcmePtr
{
    T* ptr_ = nullptr;

public:
    AcmePtr() = default;
    explicit AcmePtr(T* p) : ptr_{p} {}
    T* operator->() const { return ptr_; }
    T* Get() const { return ptr_; }

    template <class U>
    U* As(const AcmeGuid& iid)
    {
        (void)iid;
        return static_cast<U*>(static_cast<void*>(Get()));
    }
};

int ConsumeAs(AcmePtr<IProduct>& holder)
{
    IBackend* backend = holder.As<IBackend>(AcmeIidOf<IBackend>());
    return backend->Mat(); // converges to CpuBackend::Mat
}

// Negative: a concrete class template argument carries no interface
// identity - the gate stays closed (asserted on the hint table, the
// site itself may resolve through the static path).
int ConsumeAsConcrete(AcmePtr<IProduct>& holder)
{
    CpuBackend* backend = holder.As<CpuBackend>(AcmeIidOf<CpuBackend>());
    return backend->Mat();
}

int main()
{
    ProductImpl       product;
    AcmePtr<IProduct> holder{&product};
    int               acc = ConsumeQI(&product);
    ConsumeWideQI(&product);
    ConsumeNoToken(&product);
    acc += ConsumeAs(holder);
    acc += ConsumeAsConcrete(holder);
    return acc > 0 ? 0 : 1;
}
