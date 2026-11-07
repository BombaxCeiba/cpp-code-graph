// COM 根层巨族的接收者可达过滤夹具：AddRef/Release/QueryInterface 形态
// 的根协议方法在接口生态库上的 override 族是全体接口实现类——动态类型
// 集被污染（多类型并集）时整族披露，混入接收者静态类型后代闭包之外的
// 运行时不可能目标。覆盖：污染形态收窄（不可达实现被剔）、接口静态根
// 不收窄（闭包=全体，判据空转不添乱）、全部不可达回退全候选（宁保持
// 披露）与 CRTP 模板特化基形态（特化行折算进可达闭包，真实现不误砍）。

// ---- 根协议与两个无关分支：全体实现都 override 根层方法 ----

struct IRoot
{
    virtual ~IRoot() = default;
    virtual unsigned long AddRef() = 0;
    virtual unsigned long Release() = 0;
};

struct IPic : IRoot
{
    virtual int Snap() = 0;
};

struct PicA : IPic
{
    unsigned long AddRef() override { return 1; }
    unsigned long Release() override { return 1; }
    int Snap() override { return 1; }
};

struct PicB : IPic
{
    unsigned long AddRef() override { return 2; }
    unsigned long Release() override { return 2; }
    int Snap() override { return 2; }
};

struct ITone : IRoot
{
    virtual int Beep() = 0;
};

struct ToneA : ITone
{
    unsigned long AddRef() override { return 3; }
    unsigned long Release() override { return 3; }
    int Beep() override { return 3; }
};

struct ToneB : ITone
{
    unsigned long AddRef() override { return 4; }
    unsigned long Release() override { return 4; }
    int Beep() override { return 4; }
};

// ---- 污染形态：接收者槽的动态集混入闭包外实现（C 风格转型保留操作数
// 证据，模拟 may 扇投/族写回灌入的假类型）——Release 站点多候选里
// ToneA::Release 对 IPic* 接收者是运行时不可能目标，可达过滤必须剔除；
// PicA/PicB 同在闭包内，双候选 0.4 保持（may 披露不伪装唯一）。 ----

int polluted_release(IPic* p)
{
    return static_cast<int>(p->Release());
}

int drive_pollution()
{
    PicA  a;
    ToneA t;
    int   sink = polluted_release(&a);
    sink += static_cast<int>(
        polluted_release(reinterpret_cast<IPic*>(&t)));
    return sink;
}

// ---- 接口静态根形态：接收者静态类型是根协议（闭包 = 全体实现），可达
// 判据空转——全部候选都在闭包内，不得改变任何候选（回归保护）。 ----

int root_release(IRoot* p)
{
    return static_cast<int>(p->Release());
}

int drive_root(PicA& a, ToneA& t)
{
    int sink = root_release(&a);
    sink += root_release(&t);
    return sink;
}

// ---- 唯一收敛不添乱形态：污染批在巨族上收敛唯一（仅 PicA 一个实现
// 参与时 nearest 已收敛），过滤不启用、唯一候选与档位原样。 ----

int single_release(IPic* p)
{
    return static_cast<int>(p->Release());
}

int drive_single()
{
    PicA a;
    return single_release(&a);
}

// ---- 全部不可达回退形态：接收者槽动态集全是闭包外实现（IPic* 静态
// 根收到两个 Tone 实现）——可达过滤剔空时回退全候选（判据自身可能有
// extends 缺边盲区，宁保持披露），两条候选边原样保留。 ----

int orphan_release(IPic* p)
{
    return static_cast<int>(p->Release());
}

int drive_orphan()
{
    ToneA ta;
    ToneB tb;
    int   sink = static_cast<int>(
        orphan_release(reinterpret_cast<IPic*>(&ta)));
    sink += static_cast<int>(orphan_release(reinterpret_cast<IPic*>(&tb)));
    return sink;
}

// ---- CRTP 模板特化基形态：实现类经特化行继承接口（extends 边挂在主
// 模板行上、特化行无 extends 出边）——接收者可达闭包必须把特化行经
// specializes 反向折算为结构派生，否则真实现被当闭包外剔除。部分盲
// 区形态：动态集混入界外 ToneA 与界内直连实现 DirectGear 时，全滤空
// 回退救不了被误砍的特化路径实现 GearImpl。 ----

struct IGear : IRoot
{
    virtual int Spin() = 0;
};

template <typename T>
struct TGearBase : IGear
{
    unsigned long AddRef() override { return 5; }
};

struct GearImpl : TGearBase<GearImpl>
{
    unsigned long Release() override { return 7; }
    int Spin() override { return 6; }
};

struct DirectGear : IGear
{
    unsigned long AddRef() override { return 8; }
    unsigned long Release() override { return 8; }
    int Spin() override { return 9; }
};

int crtp_release(IGear* g)
{
    return static_cast<int>(g->Release());
}

int drive_crtp()
{
    GearImpl   g;
    DirectGear d;
    ToneA      t;
    int        sink = crtp_release(&g);
    sink += static_cast<int>(crtp_release(reinterpret_cast<IGear*>(&d)));
    sink += static_cast<int>(crtp_release(reinterpret_cast<IGear*>(&t)));
    return sink;
}

int main()
{
    int   sink = drive_pollution();
    PicA  a;
    ToneA t;
    sink += drive_root(a, t);
    sink += drive_single();
    sink += drive_orphan();
    sink += drive_crtp();
    return sink == 0 ? 0 : 0;
}
