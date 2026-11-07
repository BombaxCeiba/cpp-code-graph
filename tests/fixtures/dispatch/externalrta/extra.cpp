// Negative controls (same fixture, separate TU): INeg is mentioned by no
// boundary at all; INoSig sits at a loader chain that breaks at the cast -
// the loader function casts to an unsigned void(*)() and the interface is
// only consumed by a different function, so no row is emitted for either
// (no "#external" row, no external_* suffix on fallback sites).

extern "C" void* LoadLibraryA(const char*);
extern "C" void* GetProcAddress(void*, const char*);

struct INeg
{
    virtual ~INeg() = default;
    virtual int neg() = 0;
};
struct NegImpl : INeg
{
    int neg() override { return 9; }
};

struct INoSig
{
    virtual ~INoSig() = default;
    virtual int sig() = 0;
};
struct NoSigImpl : INoSig
{
    int sig() override { return 10; }
};

// Unsigned-cast negative: the API call sites exist, but no interface-typed
// call lives in this function - the signature chain is broken at the cast.
static void load_nosig()
{
    void* m = LoadLibraryA("nosig.dll");
    auto  f = reinterpret_cast<void (*)()>(GetProcAddress(m, "open"));
    f();
    (void)m;
}

int probe_neg(INeg* p) { return p->neg(); }
int probe_nosig(INoSig* p) { return p->sig(); }

int main2() { return 0; }
