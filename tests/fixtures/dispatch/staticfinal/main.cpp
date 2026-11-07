// Static resolution fixture: final/concrete receiver static types
// with a unique non-pure family implementation must resolve 1.0 with
// reason=static_unique_override. Attack functions deliberately take NO
// parameters and use bare null pointers: passing &obj would feed a
// collected address-of evidence into the parameter slot and flip the site onto the
// dynamic path, masking the static branch under test.

// Form 1: pure interface + single implementation on a leaf (target = the
// overriding method itself).
struct IA2b
{
    virtual int h() = 0;
    virtual ~IA2b() = default;
};
struct LeafA2b : IA2b
{
    int h() override { return 5; }
};

int probe_pure_single()
{
    LeafA2b* p = nullptr;
    return p->h();
}

// Form 2: leaf type whose family's only non-pure implementation is the
// inherited method itself (target = the method, body on the base).
struct BaseA2
{
    virtual int g() { return 4; }
    virtual ~BaseA2() = default;
};
struct LeafA2 : BaseA2
{
};

int probe_self_impl()
{
    LeafA2* p = nullptr;
    return p->g();
}

// Form 3: graph-final LEAF type with a 3-implementation family -
// the receiver being a leaf pins the dynamic type, so nearest-override
// convergence is statically provable (1.0, reason=static_final_unique).
struct IA1
{
    virtual int f() = 0;
    virtual ~IA1() = default;
};
struct D1A1 : IA1
{
    int f() override { return 1; }
};
struct D2A1 : IA1
{
    int f() override { return 2; }
};
struct Final3 : IA1
{
    int f() override { return 3; }
};

int probe_final_multi()
{
    Final3* p = nullptr;
    return p->f();
}

// Form 4: NON-leaf receiver with a multi-implementation
// family - no static final evidence exists, both the unique-implementation
// guard and the leaf+nearest static resolution must refuse.
struct MidA1 : IA1
{
    int f() override { return 7; }
};
struct SubMidA1 : MidA1
{
    int f() override { return 8; }
};

int probe_mid_multi()
{
    MidA1* p = nullptr;
    return p->f();
}

/**
 * @brief 形态 5（方法级 final 正例）：开放类 + 方法自带 final 的静态
 *        直连。
 *
 * @internal OpenProbe 开放（无类级 final）且图内有后代 DirectorProbe——
 *           类级证据（语法位与图内无后代）全部缺失，唯一证据是
 *           add_ref/release 的方法级 final 语法位。probe_extra 纯虚面
 *           封死第一优先的 sealed 口径（直接成员含纯虚的记录不
 *           sealed），让站点走到第 1.5 优先的方法级接力。两站点：具
 *           名接收者（reason=static_final_method）与隐式 this 自调
 *           （OpenProbe::query 体内的 add_ref()，reason=
 *           implicit_this_final_method）。攻击函数不接收 &obj 实参、
 *           用裸空指针，防取址证据把站点翻到动态路径。
 */
struct IOpenProbe
{
    virtual int add_ref() = 0;
    virtual int release() = 0;
    virtual int query() = 0;
    virtual int extra() = 0;
    virtual ~IOpenProbe() = default;
};
struct OpenProbe : IOpenProbe
{
    int add_ref() final { return 1; }
    int release() final { return 2; }
    int extra() override = 0;
    int query() override
    {
        add_ref();
        return 3;
    }
};
struct DirectorProbe : OpenProbe
{
    int query() override { return 4; }
    int extra() override { return 5; }
};

int probe_final_method_named()
{
    OpenProbe* d = nullptr;
    return d->release();
}

/**
 * @brief 形态 6（守卫）：方法只 override 不 final 必须拒绝。
 *
 * @internal 与形态 5 同构（开放类 + 图内后代 + 直接纯虚面），唯一差
 *           异是 release 不带 final——方法级证据缺失，第 1.5 优先的
 *           两级判定都不得触发，站点落 DTA 收窄展示。
 */
struct GuardHost : IOpenProbe
{
    int release() override { return 6; }
    int query() override { return 7; }
    int extra() override = 0;
};
struct GuardLeaf : GuardHost
{
    int add_ref() override { return 8; }
    int extra() override { return 9; }
};

int probe_final_method_guard()
{
    GuardHost* p = nullptr;
    return p->release();
}

/**
 * @brief 形态 7（声明/定义分离）：final 位随定义游标保留。
 *
 * @internal add_ref 的 final 声明在类内、函数体在类外——符号库取自
 *           定义处，final 位必须随定义游标保留，直连判定不因分离而
 *           丢失证据。
 */
struct SplitHost : IOpenProbe
{
    int add_ref() final;
    int release() override { return 10; }
    int query() override { return 11; }
    int extra() override = 0;
};
struct SplitLeaf : SplitHost
{
    int query() override { return 12; }
    int extra() override { return 13; }
};

int SplitHost::add_ref() { return 14; }

int probe_final_method_split()
{
    SplitHost* p = nullptr;
    return p->add_ref();
}

/**
 * @brief 形态 8（多根不猜守卫）：两个静态根各自最近覆盖不同 final
 *        方法时并集两成员不猜。
 *
 * @internal m 家族双实现（RootAProbe::m 与 RootBProbe::m 各自
 *           final）。测试把站点接收者改写为双根并集——最近覆盖并集
 *           得 2 个成员，静态不可证唯一，必须拒绝直连（宁漏勿猜），
 *           落 DTA 收窄的双候选展示。SubAProbe/SubBProbe 保证两根图
 *           内有后代（类级证据同样缺失，隔离出纯方法级证据面）。
 */
struct IDualProbe
{
    virtual int m() = 0;
    virtual int n() = 0;
    virtual ~IDualProbe() = default;
};
struct RootAProbe : IDualProbe
{
    int m() final { return 1; }
    int n() override { return 2; }
};
struct SubAProbe : RootAProbe
{
    int n() override { return 3; }
};
struct RootBProbe : IDualProbe
{
    int m() final { return 4; }
    int n() override { return 5; }
};
struct SubBProbe : RootBProbe
{
    int n() override { return 6; }
};

int probe_final_method_union()
{
    IDualProbe* p = nullptr;
    return p->m();
}

int main() { return 0; }
