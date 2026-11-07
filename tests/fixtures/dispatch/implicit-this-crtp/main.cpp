// 隐式 this 穿透模板语境夹具：CRTP 模板基（class Impl : Base<Impl>）
// 主模板行方法体内的裸虚调用——没有接收者表达式，调用方所属记录是模板
// 主模板行（方法宿主全部挂在主模板，实例行不重复挂成员）。三变体：
// 唯一命中直连（implicit_this_template_unique）、特化侧覆盖多候选不猜
// （implicit_this_template_narrowed）、折算缺边守门降档
// （implicit_this_template_fold_gap，主模板只声明不定型使实例行缺
// specializes 出边，构成闭包缺口）。

// ---- 变体一族：生命周期方法族（AddRef 家族锚在根接口） ----

struct IAcmePart
{
    virtual int AddRef() = 0;
    virtual ~IAcmePart() = default;
};

template <class TAcme>
struct AcmePartBase : IAcmePart
{
    int QueryInterface()
    {
        AddRef(); // 隐式 this：宿主是模板主模板行，特化侧不覆盖
        return 0;
    }
    int AddRef() override { return 1; } // 家族共享实现挂在主模板行
};

struct AcmeJsonPart final : AcmePartBase<AcmeJsonPart>
{
    int load() { return 1; } // 不覆盖 AddRef：闭包内唯一命中成立
};

struct AcmeStorePart final : AcmePartBase<AcmeStorePart>
{
    int load() { return 2; }
};

// 同根接口的独立实现（闭包外家族成员）：防止"全家族恰一非纯"的假阳性
// 直连——直连必须来自折算闭包的裁剪，而不是家族本来就只有一个实现。
struct AcmeForeignPart : IAcmePart
{
    int AddRef() override { return 9; }
};

// ---- 变体二：特化侧覆盖——折算闭包内两个非纯成员，多候选不猜 ----
// 显式特化行自带实现（成员挂在特化行上、overrides 直指根接口），经
// specializes 折算进入主模板闭包，构成闭包内第二非纯成员。

template <class TAcme>
struct AcmeProxyBase : IAcmePart
{
    int QueryInterface()
    {
        AddRef(); // 隐式 this：特化行自己实现了 AddRef
        return 0;
    }
    int AddRef() override { return 1; }
};

template <>
struct AcmeProxyBase<int> : IAcmePart
{
    int AddRef() override { return 5; } // 特化行实现，进折算闭包
};

// ---- 变体三：折算缺边守门 ----
// 主模板只声明不定型：显式特化行定义在库内、specializes 出边目标键无
// 行可落（主模板无定义不产符号行），实例行成为折算闭包的缺口。缺口行
// 的 extends 子树成员对闭包结构性失明——唯一命中不可再证，必须降档。

struct IAcmeGate
{
    virtual int alive() = 0;
    virtual ~IAcmeGate() = default;
};

template <class T>
struct AcmeGhost; // 只声明不定型：缺口源头

template <>
struct AcmeGhost<int>
{
    int payload() { return 1; }
};

struct AcmeGhostGate : AcmeGhost<int>, IAcmeGate
{
    // extends 上行链途经缺口行 AcmeGhost<int>（键含 <> 且无
    // specializes 出边）。
    int alive() override { return 7; }
};

template <class TAcme>
struct AcmeGateBase : IAcmeGate
{
    int open()
    {
        alive(); // 隐式 this：闭包内唯一命中，但缺口在案须降档披露
        return 0;
    }
    int alive() override { return 1; }
};

struct AcmeGatePart final : AcmeGateBase<AcmeGatePart>
{
    // 不覆盖 alive
};

int main()
{
    AcmeJsonPart    jp;
    AcmeStorePart   sp;
    AcmeForeignPart fp;
    AcmeGatePart    gp;
    AcmeGhost<int>  ghost;
    (void)ghost;
    return jp.QueryInterface() + sp.QueryInterface() + fp.AddRef()
         + gp.open() + jp.load() + sp.load();
}
