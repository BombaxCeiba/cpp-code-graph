// 异族豁免封口四态夹具：缺席类型的封口语义细分。
// 覆盖三态：(1) 异族缺席（与接收者声明面可证无关）豁免 + final 封口
// 提档；(2) 接口名缺席（合法声明面的后代接口）保持阻断；(4) 异族豁免
// 成立但 final 封口缺失（家族多实现且目标无 final 位）保持弱带。
// 污染手段：C 风格转换保留操作数证据（界外实现灌入形参槽）；接口名
// 经 static_cast 向下转型进入槽（cast 目标是合法动态候选证据）。

// ---- 态一骨架：唯一槽内实现带类级 final，家族另有一个界外实现 ----

struct AcmeNode
{
    virtual int Render() = 0;
    virtual ~AcmeNode() = default;
};
struct AcmeGlyph final : AcmeNode
{
    int Render() override { return 1; }
};
// 家族第二实现（不进接收者槽）：Render 家族非唯一，提档只能走 final 封口。
struct AcmePlainGlyph : AcmeNode
{
    int Render() override { return 5; }
};

// ---- 态四骨架：槽内唯一收敛目标无 final 位，家族多实现 ----

struct AcmePane
{
    virtual int Size() = 0;
    virtual ~AcmePane() = default;
};
struct AcmeWidePane : AcmePane
{
    int Size() override { return 2; }
};
struct AcmeTallPane : AcmePane
{
    int Size() override { return 3; }
};

// ---- 态二骨架：中间抽象接口经向下转型进槽（接口名缺席形态）----

struct AcmeShape
{
    virtual int Area() = 0;
    virtual ~AcmeShape() = default;
};
// 抽象接口：Area 家族在其祖先链上只有纯虚宿主——合法声明面的合法缺席。
struct AcmeRound : AcmeShape
{
    virtual int Radius() = 0;
};
struct AcmeDisc final : AcmeRound
{
    int Area() override { return 4; }
    int Radius() override { return 1; }
};
// 家族第二实现（不进接收者槽）：Area 家族非唯一，接口名缺席连全库
// 唯一旁路都拿不到——唯一可能的豁免路径被排除，阻断归因精确。
struct AcmeSquare final : AcmeShape
{
    int Area() override { return 9; }
};

// ---- 异族污染源：与各接收者声明面无任何继承关系 ----

struct AcmeUnrelatedA
{
    int Pad() { return 0; }
};
struct AcmeUnrelatedB
{
    int Pad() { return 0; }
};

int render(AcmeNode* n)
{
    return n->Render();
}
int size(AcmePane* p)
{
    return p->Size();
}
int area(AcmeShape* s)
{
    return s->Area();
}

// 态一：异族缺席豁免 + 类级 final 封口 -> 提档 medium。
int drive_xeno()
{
    AcmeGlyph       g;
    AcmeUnrelatedA  u;
    int             sink = render(&g);
    sink += render(reinterpret_cast<AcmeNode*>(&u));
    (void)AcmePlainGlyph{};
    return sink;
}

// 态四：异族缺席豁免成立，但槽内目标不 final 且家族多实现 -> 保持 0.4。
int drive_no_final()
{
    AcmeWidePane    w;
    AcmeUnrelatedB  u;
    int             sink = size(&w);
    sink += size(reinterpret_cast<AcmePane*>(&u));
    return sink;
}

// 态二：接口名缺席（AcmeRound 是 AcmeShape 的后代接口）-> 保持 0.4。
// 接口名经向下转型进槽：从 AcmeShape* 转向中间接口是 downcast（转型
// 目标记为动态候选证据）；直接对具体对象取址向上转型不产生接口名。
int drive_iface_name()
{
    AcmeDisc   d;
    AcmeShape* base = &d;
    AcmeShape* leak = static_cast<AcmeRound*>(base);
    return area(&d) + area(leak);
}

int main()
{
    int sink = drive_xeno() + drive_no_final() + drive_iface_name();
    return sink == 0 ? 0 : 0;
}
