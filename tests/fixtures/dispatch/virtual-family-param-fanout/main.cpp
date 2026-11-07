// 虚族扇投入参（实参→形参方向的虚族并集）夹具：虚调用的被调方解析到
// 接口/基类声明时，声明无函数体无同位置形参槽，实参的类型证据死在不确
// 定调用边界——直连失败后沿 overrides 边扇投进族内每个有体 override 的
// 同位置形参槽（may 语义超集），override 体内经该形参的虚调用站点由此
// 获得接收者动态证据。覆盖 StringImpl 代理穿透形态（两实现族收双候选）、
// 具体类接收者直连不扇投（已唯一解析走精确通道）、族内无体成员不投、
// 跨族隔离与多级继承链（链根两级闭包达叶子）。

// ---- 多实现族：接口声明无槽，两个有体实现的 #p0 槽各收扇投批 ----

struct IPayload
{
    virtual ~IPayload() = default;
    virtual int Name() = 0;
};

struct PayloadA : IPayload
{
    int Name() override { return 1; }
};

struct PayloadB : IPayload
{
    int Name() override { return 2; }
};

struct IStore
{
    virtual ~IStore() = default;
    virtual void Keep(IPayload* p) = 0;
};

struct StoreA : IStore
{
    void Keep(IPayload* p) override { (void)p->Name(); }
};

struct StoreB : IStore
{
    void Keep(IPayload* p) override { (void)p->Name(); }
};

// StringImpl 代理穿透形态：payload 经直连流进代理形参槽，代理体内经接口
// 指针调用（被调解析到 IStore::Keep 声明无体）——实参槽的类型证据扇投
// 进族内每个有体 Keep 的 #p0 槽，救活点不在代理内而在 override 体内。
void proxy_keep(IStore* store, IPayload* payload)
{
    store->Keep(payload);
}

// ---- 直连负例：具体类接收者直连，已唯一解析不扇投 ----

struct Nega
{
    virtual ~Nega() = default;
    virtual int Tag() = 0;
};

struct NegaA : Nega
{
    int Tag() override { return 7; }
};

struct INegaStore
{
    virtual ~INegaStore() = default;
    virtual void Keep(Nega* n) = 0;
};

struct NegaStoreA : INegaStore
{
    void Keep(Nega* n) override { (void)n->Tag(); }
};

struct NegaStoreB : INegaStore
{
    void Keep(Nega* n) override { (void)n->Tag(); }
};

// 接收者静态类型是具体类：被调解析到 NegaStoreA::Keep（有体有槽）走直连
// 精确通道——NegaA 只进 NegaStoreA 的 #p0，兄弟实现 NegaStoreB 不得经
// 族扇投拿到 NegaA 证据（绝不可能派发到该 override 的调用点不参与反传）。
void direct_concrete(NegaStoreA* store, Nega* n)
{
    store->Keep(n);
}

// ---- 族内无体成员负例：声明无定义的 override 无槽不投 ----

struct IDeclStore
{
    virtual ~IDeclStore() = default;
    virtual void Keep(IPayload* p) = 0;
};

struct DeclStoreA : IDeclStore
{
    void Keep(IPayload* p) override; // 声明无定义：无函数体无槽
};

struct DeclStoreB : IDeclStore
{
    void Keep(IPayload* p) override { (void)p->Name(); }
};

void proxy_decl(IDeclStore* s, IPayload* payload)
{
    s->Keep(payload);
}

// ---- 跨族隔离：不相关接口的实现类不得混入 Keep 族的候选 ----

struct IOther
{
    virtual ~IOther() = default;
    virtual int Hue() = 0;
};

struct RedColor : IOther
{
    int Hue() override { return 3; }
};

// ---- 多级继承链：链根与中间层均为纯虚声明，有体槽只在叶子 ----
// overrides 边逐级相连（叶子 -> 中间声明 -> 链根声明），从链根查询时一
// 层邻居只有中间层声明（无函数体无槽）——族闭包必须展开完整双向传递闭
// 包才能跨过中间层拿到叶子实现槽；退化成"自身 + 直接邻居"一层时链根站
// 点一无所获（漏救）。

struct IChainStore
{
    virtual ~IChainStore() = default;
    virtual void Keep(IPayload* p) = 0;
};

struct IChainMidStore : IChainStore
{
    void Keep(IPayload* p) override = 0;
};

struct ChainStoreA : IChainMidStore
{
    void Keep(IPayload* p) override { (void)p->Name(); }
};

struct ChainStoreB : IChainMidStore
{
    void Keep(IPayload* p) override { (void)p->Name(); }
};

void proxy_chain(IChainStore* s, IPayload* payload)
{
    s->Keep(payload);
}

int main()
{
    StoreA   sa;
    StoreB   sb;
    PayloadA pa;
    PayloadB pb;
    proxy_keep(&sa, &pa);
    proxy_keep(&sb, &pb);
    NegaStoreA nsa;
    NegaA      na;
    direct_concrete(&nsa, &na);
    DeclStoreB dsb;
    proxy_decl(&dsb, &pa);
    RedColor red;
    (void)red.Hue();
    ChainStoreA csa;
    ChainStoreB csb;
    proxy_chain(&csa, &pa);
    proxy_chain(&csb, &pb);
    return 0;
}
