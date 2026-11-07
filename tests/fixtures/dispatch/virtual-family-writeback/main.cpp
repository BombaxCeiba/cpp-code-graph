// 虚族并集写回夹具：虚装填调用的被调方解析到接口/基类声明时，
// 声明侧形参槽天然无写回行，写回证据在各 override 实现的同位置槽上——
// 直连失败后沿 overrides 族取并集回传。覆盖手写多实现族、单实现族、
// 生成代码直转（stub 处理器体内虚装填就近消费）、两道负例（族内无行
// 不连、跨族不串）、静态面裁剪、壳槽静态面裁剪（模板壳槽的语义面 =
// pointee 实参键）、多级继承链（链根消费须两级闭包）与 CRTP 模板特化
// 基形态（特化行折算进后代闭包，真实现不饿死）。

// ---- 壳类模板：Get/operator-> 命中透传判据（pointee 实参位收敛），
// Put 命中取址方法判据（T** 出参地址形态）——出参壳模板指针同构。 ----

template <typename T>
class TPtr
{
    T* p_ = nullptr;

public:
    TPtr() = default;
    explicit TPtr(T* p) noexcept : p_(p) {}
    T* Get() const noexcept { return p_; }
    T* operator->() const noexcept { return p_; }
    T** Put() noexcept { return &p_; }
};

// ---- 多实现族：接口声明无行，两个实现的出参槽各带实现类 ----

struct IVFruit
{
    virtual ~IVFruit() = default;
    virtual int Name() = 0;
};

struct Apple : IVFruit
{
    int Name() override { return 1; }
};

struct Banana : IVFruit
{
    int Name() override { return 2; }
};

struct IVFiller
{
    virtual ~IVFiller() = default;
    virtual void Fill(IVFruit** pp) = 0;
};

struct AppleFiller : IVFiller
{
    void Fill(IVFruit** pp) override { *pp = new Apple; }
};

struct BananaFiller : IVFiller
{
    void Fill(IVFruit** pp) override { *pp = new Banana; }
};

// 手写形态：装填调用是虚调用（filler 静态类型是接口），消费点拿族并集。
int consume_two_impls(IVFiller* filler)
{
    IVFruit* r = nullptr;
    filler->Fill(&r);
    return r->Name();
}

// 生成代码同机制形态：stub 处理器体内 impl->Fill(pp_out) 把出参直转给
// 真实实现并就近消费——装填与消费同函数，族并集直接救活局部接收者。
int stub_consume_and_use(IVFiller* impl)
{
    IVFruit* out = nullptr;
    impl->Fill(&out);
    return out->Name();
}

// ---- 单实现族：并集唯一实现类，决议端 nearest 收敛到单目标 ----

struct ISingleFiller
{
    virtual ~ISingleFiller() = default;
    virtual void Fill(IVFruit** pp) = 0;
};

struct SingleFiller : ISingleFiller
{
    void Fill(IVFruit** pp) override { *pp = new Apple; }
};

int consume_single_impl(ISingleFiller* filler)
{
    IVFruit* r = nullptr;
    filler->Fill(&r);
    return r->Name();
}

// ---- 负例一：族内全部成员都无写回行（读了不写）——证据不足不连 ----

struct INoWriteFiller
{
    virtual ~INoWriteFiller() = default;
    virtual void Fill(IVFruit** pp) = 0;
};

struct NoWriteFiller : INoWriteFiller
{
    void Fill(IVFruit** pp) override { (void)pp; }
};

int consume_no_write(INoWriteFiller* filler)
{
    IVFruit* r = nullptr;
    filler->Fill(&r);
    return r->Name();
}

// ---- 负例二：跨族隔离——具体类直连装填，另一族的类型不得混入 ----

struct IColor
{
    virtual ~IColor() = default;
    virtual int Hue() = 0;
};

struct RedColor : IColor
{
    int Hue() override { return 3; }
};

struct RedFiller
{
    void WriteOut(IColor** pp) { *pp = new RedColor; }
};

int consume_unrelated()
{
    IColor* c = nullptr;
    RedFiller filler;
    filler.WriteOut(&c);
    return c->Hue();
}

// ---- 静态面裁剪：具体类出参的族并集被静态声明面收窄 ----
// QI 快速路径形态：出参静态类型是具体类（QI 成功分支的运行时类型已定），
// 装填虚调用的族并集 [Apple, Banana] 里静态界外的 Banana 不得稀释唯一性。

struct ITypedFiller
{
    virtual ~ITypedFiller() = default;
    virtual void Fill(IVFruit** pp) = 0;
};

struct TypedAppleFiller : ITypedFiller
{
    void Fill(IVFruit** pp) override { *pp = new Apple; }
};

struct TypedBananaFiller : ITypedFiller
{
    void Fill(IVFruit** pp) override { *pp = new Banana; }
};

int consume_static_clip(ITypedFiller* filler)
{
    Apple* a = nullptr;
    filler->Fill(reinterpret_cast<IVFruit**>(&a));
    return a->Name();
}

// ---- 壳槽静态面裁剪：装填实参经壳的 Put()（取址槽形态定位到壳变量
// 槽）——壳槽静态面 = 主模板键 + pointee 实参键（Apple），主模板键不
// 是语义类型面（跳过），族并集 [Apple, Banana] 被 Apple 的静态闭包裁到
// [Apple]——模板壳槽不再是 may 并集的无界扩散口。 ----

int consume_shell_clip(ITypedFiller* filler)
{
    TPtr<Apple> box;
    filler->Fill(reinterpret_cast<IVFruit**>(box.Put()));
    return box->Name();
}

// ---- 多级继承链：链根与中间层均为纯虚声明，写回行只在叶子 ----
// overrides 边逐级相连（叶子 -> 中间声明 -> 链根声明），从链根查询时
// 一层邻居只有中间层声明（无函数体无写回行）——族闭包必须展开完整
// 双向传递闭包才能跨过中间层拿到叶子并集；退化成"自身 + 直接邻居"
// 一层时链根站点一无所获（漏救）。

struct IChainFiller
{
    virtual ~IChainFiller() = default;
    virtual void Fill(IVFruit** pp) = 0;
};

struct IChainMidFiller : IChainFiller
{
    void Fill(IVFruit** pp) override = 0;
};

struct ChainAppleFiller : IChainMidFiller
{
    void Fill(IVFruit** pp) override { *pp = new Apple; }
};

struct ChainBananaFiller : IChainMidFiller
{
    void Fill(IVFruit** pp) override { *pp = new Banana; }
};

// 从链根消费：两级闭包后才见叶子写回行（多级链主形态）。
int consume_chain_root(IChainFiller* filler)
{
    IVFruit* r = nullptr;
    filler->Fill(&r);
    return r->Name();
}

// 从中间层消费：一层即可达叶子，完整闭包不得回退此形态（回归保护）。
int consume_chain_mid(IChainMidFiller* filler)
{
    IVFruit* r = nullptr;
    filler->Fill(&r);
    return r->Name();
}

// ---- CRTP 模板特化基形态：实现类经特化行继承接口（IDL 生成模式）。
// extends 边挂在主模板行上、特化行（TFruitBase<KiwiImpl>）无 extends
// 出边——后代闭包只走 extends 边时对真实现结构性失明。混合批形态：
// 族并集 [ProxyFruit（直连继承）, KiwiImpl（特化路径）] 里只有
// ProxyFruit 在盲闭包的 allowed 面内——"全批面外整体不裁"的对称守卫
// 只救全批面外的槽，救不了这种部分盲区；闭包必须把特化行经
// specializes 反向边折算为结构派生，KiwiImpl 才进壳槽
// （TPtr<IShellFruit> 的语义面 = pointee 键）的静态面。

struct IShellFruit
{
    virtual ~IShellFruit() = default;
    virtual int Kind() = 0;
};

template <typename T>
struct TFruitBase : IShellFruit
{
};

struct KiwiImpl : TFruitBase<KiwiImpl>
{
    int Kind() override { return 21; }
};

struct ProxyFruit : IShellFruit
{
    int Kind() override { return 22; }
};

struct ILoadFiller
{
    virtual ~ILoadFiller() = default;
    virtual void Load(IShellFruit** pp) = 0;
};

struct ProxyLoadFiller : ILoadFiller
{
    void Load(IShellFruit** pp) override { *pp = new ProxyFruit; }
};

struct KiwiLoadFiller : ILoadFiller
{
    void Load(IShellFruit** pp) override { *pp = new KiwiImpl; }
};

int consume_crtp_shell(ILoadFiller* filler)
{
    TPtr<IShellFruit> box;
    filler->Load(box.Put());
    return box->Kind();
}

int main()
{
    AppleFiller  af;
    BananaFiller bf;
    int          sink = 0;
    sink += consume_two_impls(&af);
    sink += consume_two_impls(&bf);
    sink += stub_consume_and_use(&af);
    SingleFiller   sf;
    NoWriteFiller  nw;
    sink += consume_single_impl(&sf);
    sink += consume_no_write(&nw);
    sink += consume_unrelated();
    TypedAppleFiller taf;
    sink += consume_static_clip(&taf);
    sink += consume_shell_clip(&taf);
    ChainAppleFiller  caf;
    ChainBananaFiller cbf;
    sink += consume_chain_root(&caf);
    sink += consume_chain_root(&cbf);
    sink += consume_chain_mid(&caf);
    sink += consume_chain_mid(&cbf);
    ProxyLoadFiller plf;
    KiwiLoadFiller  klf;
    sink += consume_crtp_shell(&plf);
    sink += consume_crtp_shell(&klf);
    return sink == 0 ? 0 : 0;
}
