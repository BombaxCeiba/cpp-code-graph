// 转型声称证据夹具：接收者指针来源未知时，
// reinterpret_cast / static_cast / C 样式转型的目标类型直接接受为接收者
// 槽的类型证据；槽有更强证据（直连 new / 入参扇投）时声称弃用。
struct IFace
{
    virtual int emit() = 0;
    virtual ~IFace() = default;
};
struct SwigDirector_IFace : IFace
{
    int emit() override;
};
int SwigDirector_IFace::emit() { return 1; }

// 双实现族：多候选诚实披露与优先级锚点用。
struct IPort
{
    virtual int open() = 0;
    virtual ~IPort() = default;
};
struct PortA : IPort
{
    int open() override;
};
struct PortB : IPort
{
    int open() override;
};
int PortA::open() { return 1; }
int PortB::open() { return 2; }

// 界外不透明来源：指针真身不可静态追溯（SWIG 解包同构）。
extern void* opaque_source();

// 锚点 1：reinterpret_cast 声称（赋值形态）。
int reinterpret_claim()
{
    IFace* p = nullptr;
    void* raw = opaque_source();
    p = reinterpret_cast<IFace*>(raw);
    return p->emit();
}

// 锚点 2：static_cast 声称（声明初始化形态）。
int static_claim()
{
    void* raw = opaque_source();
    IFace* q = static_cast<IFace*>(raw);
    return q->emit();
}

// 锚点 3：C 样式声称（此前整档无证据行）。
int cstyle_claim()
{
    void* raw = opaque_source();
    IFace* r = (IFace*)(raw);
    return r->emit();
}

// 锚点 4：槽空才填——直连 new 在场时声称不覆盖更强证据。
int direct_new_wins()
{
    IFace* p = new SwigDirector_IFace;
    p = reinterpret_cast<IFace*>(opaque_source());
    return p->emit();
}

// 锚点 5：槽空才填——入参扇投在场时声称不覆盖（bug 形态会并出第二个
// 候选把 0.7 稀释成 0.4 多候选）。
int fanin_guard(IPort* item)
{
    IPort* p = nullptr;
    p = reinterpret_cast<IPort*>(opaque_source());
    p = item;
    return p->open();
}
int fanin_driver() { return fanin_guard(new PortA); }

// 锚点 6：两个不同声称面并集——多候选诚实披露，不伪装唯一。
int two_claims()
{
    IPort* p = nullptr;
    p = reinterpret_cast<IPort*>(opaque_source());
    p = static_cast<PortB*>(opaque_source());
    return p->open();
}

int main()
{
    int r = reinterpret_claim() + static_claim() + cstyle_claim();
    r += direct_new_wins() + fanin_driver() + two_claims();
    return r;
}
