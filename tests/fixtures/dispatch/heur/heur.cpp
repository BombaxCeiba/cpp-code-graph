// Structural-criterion fixtures for the dispatch heuristics unit tests
// (parse-input DATA, never compiled): chain-tie and out-param wrapper
// shapes, hits and adversarial misses alike, all free of real project
// names. The criterion functions under test live in
// cg::analysis::collect_detail (cg.test.analysis).
#include <memory>
#include <vector>

struct IFoo
{
    virtual int M() = 0;
    virtual ~IFoo() = default;
};

struct FooImpl final : IFoo
{
    int M() override { return 1; }
};

// -------- address-of-method criteria: member-chain shapes --------

// SlotPtr mirrors the AcmePtr shape: op-> returns the slot value, put()
// returns the slot address - both chains are [cur_] (expect tie).
template <class T>
struct SlotPtr
{
    T*  cur_;
    T*  operator->() const noexcept { return cur_; }
    T** put() noexcept { return &cur_; }
};

// Nested holder: p_ declaration is unique in Inner, but the chains differ in
// their base segment (a_ vs b_) - same declaration, different instance.
// Expect reject.
template <class T>
struct Inner
{
    T*  p_;
    T** grab() noexcept { return &p_; }
};

template <class T>
struct TwoSlot
{
    Inner<T> a_;
    Inner<T> b_;
    T*       operator->() const noexcept { return a_.p_; }
    T**      put() noexcept { return &b_.p_; }
};

// Identical dependent chain on both sides: the p_ segment resolves to
// InvalidFile inside the pattern body on BOTH sides - the pseudo-tie guard
// must degrade (uncertain), never tie.
template <class T>
struct SameChain
{
    Inner<T> a_;
    T*       operator->() const noexcept { return a_.p_; }
    T**      put() noexcept { return &a_.p_; }
};

// Arrow through a pointer field (s_->cur_): the implicit dereference of a
// non-member base expression - whole-class degrade (miss direction).
template <class T>
struct PtrIndirect
{
    T*           cur_;
    PtrIndirect* s_;
    T*           operator->() const noexcept { return s_->cur_; }
    T**          put() noexcept { return &cur_; }
};

// Array-subscript slots: both return expressions fail chain resolution.
// Expect uncertain.
template <class T>
struct ArrSlot
{
    T*  holder_[2];
    T*  operator->() const noexcept { return holder_[0]; }
    T** put() noexcept { return &holder_[1]; }
};

// Static-member slot: put returns &g_ (VarDecl) - resolvable but different
// declaration than cur_. Expect reject.
template <class T>
struct StaticSlot
{
    T*        cur_;
    static T* g_;
    T*        operator->() const noexcept { return cur_; }
    T**       put() noexcept { return &g_; }
};

// Declaration-only bodies: all four criteria hold on the signatures, the
// member-chain compare has no body to compare. Expect uncertain (degrade).
template <class T>
struct NoBody
{
    T*  cur_;
    T*  operator->() const noexcept;
    T** put() noexcept;
};

// Member template returning U** (probe_out): its parameter renumbers to
// type-parameter-0-0 and collides with the class parameter spelling - the
// non-template criterion rejects the FunctionTemplate outright.
template <class T>
struct Q
{
    T* p_;
    T* operator->() const noexcept { return p_; }
    template <class U>
    U** probe_out() noexcept
    {
        return nullptr;
    }
};

// const arrow vs T** put: 'const type-parameter-0-0' != 'type-parameter-0-0'
// - rejected (the innermost spelling differs).
template <class T>
struct ConstArrow
{
    T*       cur_;
    const T* operator->() const noexcept { return cur_; }
    T**      put() noexcept { return &cur_; }
};

// void** return: rejected, inner is 'void'.
template <class T>
struct VoidPut
{
    T*     cur_;
    T*     operator->() const noexcept { return cur_; }
    void** put_void() noexcept { return nullptr; }
};

// Multiple returns in op->: no single provable chain - uncertain.
template <class T>
struct MultiRet
{
    T*   a_;
    T*   b_;
    bool pick_ = false;
    MultiRet() = default;
    T* operator->() const noexcept
    {
        if (pick_)
        {
            return a_;
        }
        return b_;
    }
    T** put() noexcept { return &a_; }
};

// Forwarding put: return expression is a call, not a member chain -
// uncertain.
template <class T>
struct ForwardPut
{
    Inner<T> a_;
    Inner<T> b_;
    T*       operator->() const noexcept { return a_.p_; }
    T**      put() noexcept { return b_.grab(); }
};

// Cache: all four criteria pass; spare() points at the internal array
// (spare_), not the arrow slot (cur_) - only the member-chain check rejects
// it. slot()/alias_slot() tie.
template <class T>
struct Cache
{
    T*  cur_;
    T*  spare_[2];
    T*  operator->() const noexcept { return cur_; }
    T** spare() noexcept { return spare_; }
    T** slot() noexcept { return &cur_; }
    using PP = T**;
    PP alias_slot() noexcept { return &cur_; }
};

// Non-template pointer-style class: operator-> returns a concrete pointer,
// no type-parameter marker - rejected.
struct ConcPtr
{
    IFoo*  p_;
    IFoo*  operator->() const noexcept { return p_; }
    IFoo** put() noexcept { return &p_; }
};

// ------------------- out-param wrapper shapes -------------------

// Minimal slot writer (core + strict): expect hit.
template <class T>
struct HeurSlotWriter
{
    T** slot_;
    HeurSlotWriter(T** s) : slot_(s) {}
    void Assign(T* p) { *slot_ = p; }
};

// OutParam clones the AcmeOutPtr shape: every structural criterion
// passes (ctor slot binding, unique field, non-static single-T*
// write-back through a dereference) plus an operator-> alongside.
template <class T>
struct HeurOutParam
{
    T** pp_out_;
    HeurOutParam(T** s) : pp_out_(s) {}
    void Set(T* p) { *pp_out_ = p; }
    T*   Put() noexcept { return *pp_out_; }
    T*   operator->() const noexcept { return *pp_out_; }
};

// No T** storage field: binding cannot persist (the uniqueness
// guard rejects).
template <class T>
struct HeurNoField
{
    HeurNoField(T** s) { (void)s; }
    void Assign(T* p) { (void)p; }
};

// Static write-back: cannot touch the instance field (write-back
// methods must be non-static).
template <class T>
struct HeurStaticSet
{
    T** slot_;
    HeurStaticSet(T** s) : slot_(s) {}
    static void Assign(T* p) { (void)p; }
};

// Single-pointer ctor, smart-pointer-like (no T** ctor binding -
// miss).
template <class T>
struct HeurPtrHolder
{
    T* p_ = nullptr;
    HeurPtrHolder(T* p) : p_(p) {}
    void Reset(T* q) { p_ = q; }
};

// Double-ptr ctor + field but no single-T*-param method (no
// write-back method - miss).
template <class T>
struct HeurMatrix
{
    T** rows_;
    HeurMatrix(T** r) : rows_(r) {}
    T* row(long i) { return rows_[i]; }
};

// Binding param is a container reference, not T** (no T** ctor
// binding - miss).
template <class T>
struct HeurListPush
{
    std::vector<T*> v_;
    HeurListPush(std::vector<T*>& v) : v_(v) {}
    void push(T* x) { v_.push_back(x); }
};

// Ctor is a member template: its param is depth-1, not the class's own
// type parameter (own-index check rejects the spelling collision).
template <class T>
struct HeurMemberTmplCtor
{
    HeurMemberTmplCtor(T* p) { (void)p; }
    template <class U>
    HeurMemberTmplCtor(U** u)
    {
        (void)u;
    }
    void Assign(T* p) { (void)p; }
};

// Indexed-slot write-back: binding and method criteria pass but the
// assignment LHS is an array subscript, not a dereference (reject -
// prefer miss).
template <class T>
struct HeurIndexedSlot
{
    T**  slot_;
    long idx_;
    HeurIndexedSlot(T** s, long i) : slot_(s), idx_(i) {}
    void Assign(T* p) { slot_[idx_] = p; }
};

// Binding happens via a method, ctor does not bind (no T** ctor
// binding - miss -> fallback).
template <class T>
struct HeurBindMethod
{
    T** slot_ = nullptr;
    HeurBindMethod() = default;
    void Bind(T** s) { slot_ = s; }
    void Assign(T* p)
    {
        if (slot_)
        {
            *slot_ = p;
        }
    }
};

// Comparison, not assignment: '*slot_ == p' must not count as write-back
// proof (the dereference-assignment check rejects).
template <class T>
struct HeurCompareNotAssign
{
    T** slot_;
    HeurCompareNotAssign(T** s) : slot_(s) {}
    void Assign(T* p)
    {
        if (*slot_ == p)
        {
            (void)1;
        }
    }
};

// Multiple T** binding params (binds > 1): no-op, return empty.
template <class T>
struct HeurTwoBind
{
    T** s1_;
    T** s2_;
    HeurTwoBind(T** a, T** b) : s1_(a), s2_(b) {}
    void Assign(T* p) { *s1_ = p; }
};

// Two dp fields on the same index; the ctor binds slot_ (declared second)
// but Assign writes other_ (declared first) - every structural criterion
// mechanically passes while the bound slot is never written. The
// field-uniqueness guard must no-op the whole class (red-light shape,
// prefer miss).
template <class T>
struct HeurSwapFields
{
    T** other_;
    T** slot_;
    HeurSwapFields(T** s) : slot_(s) {}
    void Assign(T* p) { *other_ = p; }
};

template <class T>
T* StaticSlot<T>::g_ = nullptr;

int main()
{
    IFoo*         v = nullptr;
    SlotPtr<IFoo> sp;
    (void)sp;
    TwoSlot<IFoo> two;
    (void)two;
    SameChain<IFoo> same;
    (void)same;
    PtrIndirect<IFoo> pi;
    (void)pi;
    ArrSlot<IFoo> arr;
    (void)arr;
    StaticSlot<IFoo> st;
    (void)st;
    Q<IFoo> q;
    (void)q;
    ConstArrow<IFoo> ca;
    (void)ca;
    VoidPut<IFoo> vp;
    (void)vp;
    MultiRet<IFoo> mr;
    (void)mr;
    ForwardPut<IFoo> fp;
    (void)fp;
    Cache<IFoo> cache;
    (void)cache;
    ConcPtr conc;
    (void)conc;
    HeurSlotWriter<IFoo> w(&v);
    (void)w;
    HeurOutParam<IFoo> op(&v);
    (void)op;
    HeurNoField<IFoo> nf(&v);
    (void)nf;
    HeurStaticSet<IFoo> ss(&v);
    (void)ss;
    HeurPtrHolder<IFoo> ph(v);
    (void)ph;
    HeurMatrix<IFoo> mx(&v);
    (void)mx;
    std::vector<IFoo*> bucket;
    HeurListPush<IFoo> lp(bucket);
    (void)lp;
    HeurMemberTmplCtor<IFoo> mtc(v);
    (void)mtc;
    HeurIndexedSlot<IFoo> is(&v, 0);
    (void)is;
    HeurBindMethod<IFoo> bm;
    (void)bm;
    HeurCompareNotAssign<IFoo> cna(&v);
    (void)cna;
    HeurTwoBind<IFoo> tb(&v, &v);
    (void)tb;
    HeurSwapFields<IFoo> sf(&v);
    (void)sf;
    NoBody<IFoo> nb;
    (void)nb;
    auto shared = std::make_shared<FooImpl>();
    (void)shared;
    return 0;
}
