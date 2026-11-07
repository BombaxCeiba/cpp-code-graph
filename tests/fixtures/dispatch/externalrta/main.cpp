// External RTA fixture: interfaces reached via a SwigDirector_* class or a
// plugin-loading call chain (dlopen/LoadLibrary -> GetProcAddress/dlsym ->
// a signed function-pointer cast whose product is used through the
// interface type inside the SAME loader function) get a "#external" row
// carrying R(I) = extends-descendant closure intersect the library's
// instantiated-type set. Every dlopen row traces to the concrete loader
// call site; a chain broken at the cast (no interface-typed use in the
// loader function) emits nothing. Dispatch consumes the row as
// display-only candidate ordering; reachability never becomes a site
// confidence.

extern "C" void* dlopen(const char*, int);
extern "C" void* GetProcAddress(void*, const char*);

struct IExt
{
    virtual ~IExt() = default;
    virtual int use() = 0;
};
struct ExtImpl : IExt
{
    int use() override { return 1; }
};
struct ExtOther : IExt
{
    int use() override { return 2; }
};

// Loaded boundary with no library-internal instantiation at all: the plugin
// implementation lives outside the index (hit==0 shape), reached here via
// the director wrapper.
struct IPlain
{
    virtual ~IPlain() = default;
    virtual int q() = 0;
};
struct PlainImpl : IPlain
{
    int q() override { return 3; }
};
struct SwigDirector_IPlain : IPlain
{
    int q() override { return 8; }
};

// Director shape: the SwigDirector_* wrapper class lives inside the index
// and extends the script-side base interface.
struct IDir
{
    virtual ~IDir() = default;
    virtual int step() = 0;
};
struct DirImpl : IDir
{
    int step() override { return 4; }
};
struct SwigDirector_IDir : IDir
{
    int step() override { return 5; }
};

// dlopen positive: loader call chain and interface-typed use share one
// function, so the boundary row traces to the GetProcAddress call site.
using MakeExt = IExt* (*)();
int run_ext()
{
    void* m = dlopen("ext.dll", 2);
    auto  mk = reinterpret_cast<MakeExt>(GetProcAddress(m, "make_ext"));
    IExt* p = mk();
    return p->use();
}

// register* heuristic negative: a register-prefixed function whose
// parameter type names an interface stays boundary-free - prefix guessing
// was removed along with the .i text scan (only deterministic call-site
// evidence claims a boundary).
struct IReg
{
    virtual ~IReg() = default;
    virtual int reg() = 0;
};
struct RegImpl : IReg
{
    int reg() override { return 6; }
};
void register_sinks(IReg* registry) { (void)registry; }

// Library-internal instantiations feeding I_all: ExtImpl/DirImpl get one;
// ExtOther, PlainImpl and SwigDirector_* stay deliberately un-instantiated.
IExt* make_impl() { return new ExtImpl; }
IDir* make_dir() { return new DirImpl; }

// All probe sites take their receiver from a parameter (no dynamic
// evidence): the static-type interface decides whether RTA activates.
int probe_export(IExt* p) { return p->use(); }
int probe_plain(IPlain* p) { return p->q(); }
int probe_director(IDir* p) { return p->step(); }

int main() { return 0; }
