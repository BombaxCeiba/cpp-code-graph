// dlopen zero-activation fixture: same loader chain shape as
// externalrta, but the API prototypes come from an out-of-roots header
// (../externalout-api/ext_api.h) instead of in-TU extern "C" declarations.
// The pairing must still fire: IExtOut gets a "#external" dlopen_boundary
// row whose anchor names the loader call site in THIS file; the API call
// site itself lands in call_sites with callee_name="dlopen", never
// resolved, never a calls edge. The same-name in-library definition
// (ext_call_name probe below would collide via by-name resolve) is also
// covered: a library function literally named "dlopen" is NOT linked to
// the out-of-index site (short-circuit by the API list).

#include "ext_api.h"

struct IExtOut
{
    virtual ~IExtOut() = default;
    virtual int use() = 0;
};
struct OutImpl : IExtOut
{
    int use() override { return 1; }
};
struct OutOther : IExtOut
{
    int use() override { return 2; }
};

// Library-internal instantiation feeding I_all (hit==1 shape).
IExtOut* make_out() { return new OutImpl; }

// Loader chain: API sites + interface-typed use share one function.
using MakeOut = IExtOut* (*)();
int run_out()
{
    void*    m = dlopen("out.dll", 2);
    auto     mk = reinterpret_cast<MakeOut>(GetProcAddress(m, "make_out"));
    IExtOut* p = mk();
    return p->use();
}

// Static-type probe (no dynamic evidence): RTA narrows candidates to the
// reachable implementation only.
int probe_out(IExtOut* p) { return p->use(); }

int main() { return 0; }
