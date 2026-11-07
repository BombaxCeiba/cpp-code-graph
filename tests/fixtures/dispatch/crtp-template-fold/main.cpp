// CRTP 模板折叠夹具：CRTP 模板基形态（class Impl : Base<Impl>）的上行
// 折算——extends 边挂在主模板行、特化行无 extends 出边，只走 extends
// 的祖先链到特化行即死，nearest_overrides 对方法宿主在主模板行的家族
// 结构性零交集（family_ambiguous 的 CRTP 模板主/实例链断裂带根因）。特化行经 specializes
// 正向折算到主模板行后距离表可达宿主行。四形态：动态路径唯一默认实现
// （famamb -> nearest_converged）、静态路径 final 接收者（absent ->
// static_final_unique）、两级模板链（extends + 折算混合跳跃）、跨家族
// 隔离护栏（同名方法互不串）。静态路径攻击函数用裸空指针：传 &obj 会
// 把取址证据喂进接收者槽、把站点翻到动态路径，盖住被测的静态分支。

// ---- 形态 A：家族唯一非纯实现挂在主模板行，派生不覆盖 ----

template <typename T>
struct AcmeNodeBase
{
    virtual int tick() { return 1; }
    virtual ~AcmeNodeBase() = default;
};

struct AcmeGadget : AcmeNodeBase<AcmeGadget>
{
    // 不覆盖 tick：非纯成员只有主模板行的默认实现
};

int probe_gadget_dynamic()
{
    AcmeGadget* g = new AcmeGadget;
    return g->tick();
}

// ---- 形态 B：开放叶接收者 + 家族双非纯实现（普通接口混入） ----
// AcmeOtherSink 经普通继承入族使家族非纯数 = 2，第一优先（全家族唯一
// 实现）失败；AcmeOpenUser 自身无成员，最近覆盖只能落在主模板行——
// 折算前距离表缺席（absent）不直连，折算后唯一收敛 + 接收者图内无后
// 代（类级 final 口径）-> static_final_unique。

struct IAcymeSink
{
    virtual int cost() = 0;
    virtual ~IAcymeSink() = default;
};

template <typename T>
struct AcmeCostBase : IAcymeSink
{
    int cost() final { return 1; }
};

struct AcmeOpenUser : AcmeCostBase<AcmeOpenUser>
{
    // 不覆盖 cost（final 封死）
};

struct AcmeOtherSink : IAcymeSink
{
    int cost() override { return 9; }
};

int probe_sealed_static()
{
    AcmeOpenUser* p = nullptr;
    return p->cost();
}

// ---- 形态 E：两级模板链——特化行折算与 dependent extends 边混合 ----
// AcmeDeep -> 特化行 AcmeMid<AcmeDeep> ->（折算）AcmeMid 主模板行 ->
// （extends）AcmeOuter 主模板行：距离表要连跳三步才到 step 宿主。

template <typename T>
struct AcmeOuter
{
    virtual int step() { return 1; }
    virtual ~AcmeOuter() = default;
};

template <typename T>
struct AcmeMid : AcmeOuter<T>
{
};

struct AcmeDeep : AcmeMid<AcmeDeep>
{
    // 不覆盖 step
};

int probe_deep_chain()
{
    AcmeDeep* d = new AcmeDeep;
    return d->step();
}

// ---- 形态 C：跨家族隔离护栏 ----
// 两个独立 CRTP 家族各有一个同名方法 tag：折算只经本家族的 specializes
// 边上行，AcmeBeta 的祖先链绝不含 AcmeTagBase 家族——AcmeAlpha::tag
// 不得出现在 AcmeBeta 站点的候选面（零误连纪律）。

template <typename T>
struct AcmeTagBase
{
    virtual int tag() { return 1; }
    virtual ~AcmeTagBase() = default;
};

struct AcmeAlpha : AcmeTagBase<AcmeAlpha>
{
    int tag() override { return 2; }
};

template <typename T>
struct AcmeMarkBase
{
    virtual int tag() { return 3; }
    virtual ~AcmeMarkBase() = default;
};

struct AcmeBeta : AcmeMarkBase<AcmeBeta>
{
    // 不覆盖 tag：最近覆盖 = AcmeMarkBase 主模板行
};

int probe_cross_family()
{
    AcmeBeta* b = new AcmeBeta;
    return b->tag();
}

int main()
{
    int sink = 0;
    sink += probe_gadget_dynamic();
    sink += probe_sealed_static();
    sink += probe_deep_chain();
    sink += probe_cross_family();
    (void)new AcmeAlpha;
    return sink == 0 ? 0 : 0;
}
