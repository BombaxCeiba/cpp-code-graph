// 透传壳闸夹具：断链 1（getter 返回槽）/断链 2（ctor 初始化列表、
// operator= 直写、壳 move 搬运）/断链 3（operator-> 字段站点）/断链 4
// （非模板壳 getter 体追源）六形态 + 双负例（多实现不猜、空壳无证据不
// 发推断边）。判据只认 AST 形态（类模板 + 零参成员函数返回 own 模板形
// 参 + T* 字段 + operator->），零项目命名。
#include <utility>

struct IFace
{
    virtual int virt() = 0;
    virtual ~IFace() = default;
};
struct ImplA final : IFace
{
    int virt() override { return 1; }
};
struct ImplB final : IFace
{
    int virt() override { return 2; }
};

// 指针式壳（T* 存储 + operator-> + 透传 getter Get）。
template <class T>
struct Shell
{
    T* ptr_{};
    Shell() = default;
    Shell(T* p) : ptr_{p} {}
    Shell& operator=(T* p)
    {
        ptr_ = p;
        return *this;
    }
    T*  Get() const { return ptr_; }
    T*  operator->() const { return ptr_; }
    T** Put() { return &ptr_; } // 取址形态（T**）：登记但值面不透传
};

// 各形态独立记录：字段槽是记录级并集，同记录混写会互相污染断言。
struct HolderCtor
{
    Shell<IFace> f_{};
    HolderCtor(IFace* p) : f_{p} {}
    int use() { return f_->virt(); } // 断链 3：operator-> 字段站点
};

struct HolderMove
{
    Shell<IFace> f_{};
    int          use() { return f_->virt(); }
};

struct HolderMulti
{
    Shell<IFace> f_{};
    HolderMulti(IFace* p) : f_{p} {}
    int use() { return f_->virt(); }
};

struct PlainWrap
{ // 断链 4：非模板壳类的 getter 体透传链
    Shell<IFace> f_{};
    PlainWrap(IFace* p) : f_{p} {}
    IFace* get() const { return f_.Get(); }
};

int ctor_init_penetration()
{
    HolderCtor h{new ImplA};
    return h.use();
}

int assign_penetration()
{
    Shell<IFace> v;
    IFace*       raw = new ImplA;
    v = raw; // operator=(T*) 直写局部壳槽
    return v->virt();
}

int field_move_transport()
{
    HolderMove   h;
    Shell<IFace> s{new ImplB};
    h.f_ = std::move(s); // 字段槽 operator=(壳) 的 move 搬运
    return h.use();
}

int getter_chain_receiver()
{
    Shell<IFace> s{new ImplA};
    return s.Get()->virt(); // 断链 1：链式接收者解链到壳实例槽
}

int getter_value_slot()
{
    Shell<IFace> s{new ImplA};
    IFace*       r = s.Get(); // 断链 1：值槽别名到壳实例槽
    return r->virt();
}

int plain_getter_chain()
{
    PlainWrap w{new ImplB};
    return w.get()->virt(); // 断链 4：零参单 return 的函数体追源
}

int negative_multi()
{
    HolderMulti a{new ImplA};
    HolderMulti b{new ImplB};
    return a.use() + b.use(); // 字段槽并集 {ImplA, ImplB}：不猜
}

int negative_empty_site(Shell<IFace>& s)
{
    return s->virt(); // 无证据：不发推断边
}

int negative_put_value()
{
    Shell<IFace> s{new ImplA};
    IFace**      pp = s.Put(); // 取址形态：值槽不得按透传别名壳槽
    (void)pp;
    return s->virt();
}
