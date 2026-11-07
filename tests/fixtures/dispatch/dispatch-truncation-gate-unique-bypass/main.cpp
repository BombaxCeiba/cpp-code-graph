// 截断观测硬闸夹具：槽类型集整批搬运截断（排序后保留前 32）时，
// 唯一收敛不得越过截断闸；家族全库唯一非纯实现是唯一旁路（唯一守卫
// 不依赖槽观测完整性：任何可实例化动态类型的最近覆盖必为那个唯一成
// 员）。真实现命名排字典序最前（Acme00 前缀 < AcmeFill），确保截断
// 保留。截断触发形态：局部指针槽累积 33 类型后整批传参——逐调用点
// 单类型实参不触发批截断，截断只发生在整批搬运时。

struct AcmeGateBase
{
    virtual int Pass() = 0;
    virtual ~AcmeGateBase() = default;
};
// 槽内唯一收敛目标（类级 final）：截断闸无旁路时它也不得提档。
struct Acme00Solo final : AcmeGateBase
{
    int Pass() override { return 1; }
};
// 家族第二实现（不进槽）：Pass 家族非唯一，截断闸失去全库唯一旁路。
struct AcmeSideGate : AcmeGateBase
{
    int Pass() override { return 2; }
};

struct AcmeMarkBase
{
    virtual int Mark() = 0;
    virtual ~AcmeMarkBase() = default;
};
// Mark 家族全库唯一非纯实现：截断观测位不再阻断提档。
struct Acme00Mark final : AcmeMarkBase
{
    int Mark() override { return 3; }
};

// ---- 截断填充异族（32 个）：与两个基接口均无继承关系 ----

struct AcmeFill00
{
    int Pad() { return 0; }
};
struct AcmeFill01
{
    int Pad() { return 0; }
};
struct AcmeFill02
{
    int Pad() { return 0; }
};
struct AcmeFill03
{
    int Pad() { return 0; }
};
struct AcmeFill04
{
    int Pad() { return 0; }
};
struct AcmeFill05
{
    int Pad() { return 0; }
};
struct AcmeFill06
{
    int Pad() { return 0; }
};
struct AcmeFill07
{
    int Pad() { return 0; }
};
struct AcmeFill08
{
    int Pad() { return 0; }
};
struct AcmeFill09
{
    int Pad() { return 0; }
};
struct AcmeFill10
{
    int Pad() { return 0; }
};
struct AcmeFill11
{
    int Pad() { return 0; }
};
struct AcmeFill12
{
    int Pad() { return 0; }
};
struct AcmeFill13
{
    int Pad() { return 0; }
};
struct AcmeFill14
{
    int Pad() { return 0; }
};
struct AcmeFill15
{
    int Pad() { return 0; }
};
struct AcmeFill16
{
    int Pad() { return 0; }
};
struct AcmeFill17
{
    int Pad() { return 0; }
};
struct AcmeFill18
{
    int Pad() { return 0; }
};
struct AcmeFill19
{
    int Pad() { return 0; }
};
struct AcmeFill20
{
    int Pad() { return 0; }
};
struct AcmeFill21
{
    int Pad() { return 0; }
};
struct AcmeFill22
{
    int Pad() { return 0; }
};
struct AcmeFill23
{
    int Pad() { return 0; }
};
struct AcmeFill24
{
    int Pad() { return 0; }
};
struct AcmeFill25
{
    int Pad() { return 0; }
};
struct AcmeFill26
{
    int Pad() { return 0; }
};
struct AcmeFill27
{
    int Pad() { return 0; }
};
struct AcmeFill28
{
    int Pad() { return 0; }
};
struct AcmeFill29
{
    int Pad() { return 0; }
};
struct AcmeFill30
{
    int Pad() { return 0; }
};
struct AcmeFill31
{
    int Pad() { return 0; }
};

int gate(AcmeGateBase* g)
{
    return g->Pass();
}
int mark(AcmeMarkBase* m)
{
    return m->Mark();
}

// 硬闸臂：33 类型整批搬运截断，全部缺席可证异族、唯一收敛成立，
// 但截断观测位为真且家族非唯一 -> 不提档。
int drive_gate()
{
    Acme00Solo     s;
    AcmeFill00     f00;
    AcmeFill01     f01;
    AcmeFill02     f02;
    AcmeFill03     f03;
    AcmeFill04     f04;
    AcmeFill05     f05;
    AcmeFill06     f06;
    AcmeFill07     f07;
    AcmeFill08     f08;
    AcmeFill09     f09;
    AcmeFill10     f10;
    AcmeFill11     f11;
    AcmeFill12     f12;
    AcmeFill13     f13;
    AcmeFill14     f14;
    AcmeFill15     f15;
    AcmeFill16     f16;
    AcmeFill17     f17;
    AcmeFill18     f18;
    AcmeFill19     f19;
    AcmeFill20     f20;
    AcmeFill21     f21;
    AcmeFill22     f22;
    AcmeFill23     f23;
    AcmeFill24     f24;
    AcmeFill25     f25;
    AcmeFill26     f26;
    AcmeFill27     f27;
    AcmeFill28     f28;
    AcmeFill29     f29;
    AcmeFill30     f30;
    AcmeFill31     f31;
    AcmeGateBase*  p = &s;
    p = reinterpret_cast<AcmeGateBase*>(&f00);
    p = reinterpret_cast<AcmeGateBase*>(&f01);
    p = reinterpret_cast<AcmeGateBase*>(&f02);
    p = reinterpret_cast<AcmeGateBase*>(&f03);
    p = reinterpret_cast<AcmeGateBase*>(&f04);
    p = reinterpret_cast<AcmeGateBase*>(&f05);
    p = reinterpret_cast<AcmeGateBase*>(&f06);
    p = reinterpret_cast<AcmeGateBase*>(&f07);
    p = reinterpret_cast<AcmeGateBase*>(&f08);
    p = reinterpret_cast<AcmeGateBase*>(&f09);
    p = reinterpret_cast<AcmeGateBase*>(&f10);
    p = reinterpret_cast<AcmeGateBase*>(&f11);
    p = reinterpret_cast<AcmeGateBase*>(&f12);
    p = reinterpret_cast<AcmeGateBase*>(&f13);
    p = reinterpret_cast<AcmeGateBase*>(&f14);
    p = reinterpret_cast<AcmeGateBase*>(&f15);
    p = reinterpret_cast<AcmeGateBase*>(&f16);
    p = reinterpret_cast<AcmeGateBase*>(&f17);
    p = reinterpret_cast<AcmeGateBase*>(&f18);
    p = reinterpret_cast<AcmeGateBase*>(&f19);
    p = reinterpret_cast<AcmeGateBase*>(&f20);
    p = reinterpret_cast<AcmeGateBase*>(&f21);
    p = reinterpret_cast<AcmeGateBase*>(&f22);
    p = reinterpret_cast<AcmeGateBase*>(&f23);
    p = reinterpret_cast<AcmeGateBase*>(&f24);
    p = reinterpret_cast<AcmeGateBase*>(&f25);
    p = reinterpret_cast<AcmeGateBase*>(&f26);
    p = reinterpret_cast<AcmeGateBase*>(&f27);
    p = reinterpret_cast<AcmeGateBase*>(&f28);
    p = reinterpret_cast<AcmeGateBase*>(&f29);
    p = reinterpret_cast<AcmeGateBase*>(&f30);
    p = reinterpret_cast<AcmeGateBase*>(&f31);
    (void)AcmeSideGate{};
    return gate(p);
}

// 唯一旁路：同样 33 类型整批截断，但 Mark 家族全库唯一非纯 -> 提档。
int drive_mark()
{
    Acme00Mark     m;
    AcmeFill00     f00;
    AcmeFill01     f01;
    AcmeFill02     f02;
    AcmeFill03     f03;
    AcmeFill04     f04;
    AcmeFill05     f05;
    AcmeFill06     f06;
    AcmeFill07     f07;
    AcmeFill08     f08;
    AcmeFill09     f09;
    AcmeFill10     f10;
    AcmeFill11     f11;
    AcmeFill12     f12;
    AcmeFill13     f13;
    AcmeFill14     f14;
    AcmeFill15     f15;
    AcmeFill16     f16;
    AcmeFill17     f17;
    AcmeFill18     f18;
    AcmeFill19     f19;
    AcmeFill20     f20;
    AcmeFill21     f21;
    AcmeFill22     f22;
    AcmeFill23     f23;
    AcmeFill24     f24;
    AcmeFill25     f25;
    AcmeFill26     f26;
    AcmeFill27     f27;
    AcmeFill28     f28;
    AcmeFill29     f29;
    AcmeFill30     f30;
    AcmeFill31     f31;
    AcmeMarkBase*  p = &m;
    p = reinterpret_cast<AcmeMarkBase*>(&f00);
    p = reinterpret_cast<AcmeMarkBase*>(&f01);
    p = reinterpret_cast<AcmeMarkBase*>(&f02);
    p = reinterpret_cast<AcmeMarkBase*>(&f03);
    p = reinterpret_cast<AcmeMarkBase*>(&f04);
    p = reinterpret_cast<AcmeMarkBase*>(&f05);
    p = reinterpret_cast<AcmeMarkBase*>(&f06);
    p = reinterpret_cast<AcmeMarkBase*>(&f07);
    p = reinterpret_cast<AcmeMarkBase*>(&f08);
    p = reinterpret_cast<AcmeMarkBase*>(&f09);
    p = reinterpret_cast<AcmeMarkBase*>(&f10);
    p = reinterpret_cast<AcmeMarkBase*>(&f11);
    p = reinterpret_cast<AcmeMarkBase*>(&f12);
    p = reinterpret_cast<AcmeMarkBase*>(&f13);
    p = reinterpret_cast<AcmeMarkBase*>(&f14);
    p = reinterpret_cast<AcmeMarkBase*>(&f15);
    p = reinterpret_cast<AcmeMarkBase*>(&f16);
    p = reinterpret_cast<AcmeMarkBase*>(&f17);
    p = reinterpret_cast<AcmeMarkBase*>(&f18);
    p = reinterpret_cast<AcmeMarkBase*>(&f19);
    p = reinterpret_cast<AcmeMarkBase*>(&f20);
    p = reinterpret_cast<AcmeMarkBase*>(&f21);
    p = reinterpret_cast<AcmeMarkBase*>(&f22);
    p = reinterpret_cast<AcmeMarkBase*>(&f23);
    p = reinterpret_cast<AcmeMarkBase*>(&f24);
    p = reinterpret_cast<AcmeMarkBase*>(&f25);
    p = reinterpret_cast<AcmeMarkBase*>(&f26);
    p = reinterpret_cast<AcmeMarkBase*>(&f27);
    p = reinterpret_cast<AcmeMarkBase*>(&f28);
    p = reinterpret_cast<AcmeMarkBase*>(&f29);
    p = reinterpret_cast<AcmeMarkBase*>(&f30);
    p = reinterpret_cast<AcmeMarkBase*>(&f31);
    return mark(p);
}

int main()
{
    int sink = drive_gate() + drive_mark();
    return sink == 0 ? 0 : 0;
}
