// 接收者静态类型是透传壳类（壳剥 pointee 实参）与有名接收者但站点
// 静态类型空（声明面回退补根）两形态的虚站点夹具。全部判据零名字
// （结构判定），负例覆盖多实现不猜与壳无可剥实参跳过。
#include <memory>

struct IAbst
{
    virtual int f() = 0;
    virtual ~IAbst() = default;
};
struct Leaf final : IAbst
{
    int f() override { return 1; }
};
struct IAbst2
{
    virtual int g() = 0;
    virtual ~IAbst2() = default;
};
struct Impl2a : IAbst2
{
    int g() override { return 1; }
};
struct Impl2b : IAbst2
{
    int g() override { return 2; }
};

// 界内透传壳：operator-> 是零参返回 own 形参的透传 getter。
template <typename T>
struct OwnShell
{
    T* p_ = nullptr;
    explicit OwnShell(T* p) : p_(p) {}
    T* operator->() { return p_; }
    T* operator->() const { return p_; }
};

// 负例壳：非类型形参前置——own 形参下标域拒绝，无法定位 pointee 实参
// 位，剥壳必须跳过（站点行为与无剥壳时一致）。
template <int N, typename T>
struct MixedShell
{
    T*                   p_ = nullptr;
    T*                   get() { return p_; }
    static constexpr int n = N;
};

// ---- 壳正例：壳实参是叶子实现类，家族唯一覆盖 → 唯一决议 ----
int shell_leaf()
{
    OwnShell<Leaf> s{new Leaf};
    return s->f();
}

// ---- 壳形态：界外壳（标准库）实测不剥——主模板定义在系统头内，其
//      pattern 游标对本工具链不暴露成员，透传判据无从取数（漏收方向，
//      宁漏勿猜）；本站点的决议来自 make_unique 工厂返回槽的动态证据
//      （既有机制，不依赖剥壳）----
int std_shell_leaf()
{
    std::unique_ptr<Leaf> p = std::make_unique<Leaf>();
    return p->f();
}

// ---- 壳形态：字段壳（隐式 this 字段接收者）→ 唯一决议 ----
struct Holder
{
    OwnShell<Leaf> held_{nullptr};
    int            use() { return held_->f(); }
};

// ---- 壳形态：界外壳形参（标准库）实测不剥（同上，pattern 成员不可
//      见）；无构造/工厂动态证据时站点不决议——界外壳形参的可救面
//      留给剥壳之外的既有证据路径 ----
int std_shell_param(std::unique_ptr<Leaf> q) { return q->f(); }

// ---- 壳负例：壳实参接口有多实现 → 降档不猜（收窄候选、无推断边）----
int shell_multi()
{
    OwnShell<IAbst2> s{new Impl2a};
    return s->g();
}

// ---- 壳负例（形参形态）：壳自身键作动态证据与家族无交集 → 不猜 ----
int shell_multi_param(OwnShell<IAbst2> s) { return s->g(); }

// ---- 壳负例：壳无可剥实参（own 域拒绝）→ 不追加，行为同旧 ----
int shell_mixed()
{
    MixedShell<3, IAbst2> s{nullptr};
    return s.get()->g();
}

// ---- 声明面回退宿主：局部变量声明行在库；测试内把站点 receiver_types
//      清空，模拟界外/宏产物形态下采集面丢静态类型（声明行仍在库）----
int declared_leaf_host()
{
    Leaf* q = nullptr;
    return q->f();
}

// ---- 声明面回退负例宿主：声明类型是多实现接口 → 补根后仍降档不猜 ----
int declared_multi_host()
{
    IAbst2* q = nullptr;
    return q->g();
}

// ---- 声明面回退负例宿主：形参声明行不带静态面（type_set 为空）→
//      无声明面可补，行为同旧 ----
int declared_param_host(IAbst2* q) { return q->g(); }

int main()
{
    (void)shell_leaf();
    (void)std_shell_leaf();
    (void)std_shell_param(std::make_unique<Leaf>());
    (void)Holder{OwnShell<Leaf>{new Leaf}}.use();
    (void)shell_multi();
    (void)shell_multi_param(OwnShell<IAbst2>{nullptr});
    (void)shell_mixed();
    (void)declared_leaf_host();
    (void)declared_multi_host();
    (void)declared_param_host(nullptr);
    return 0;
}
