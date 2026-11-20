// 限定调用判据回归夹具：call_sites.qualified 的行为锚点。
//
// 普通调用走 AST 判据（引用名范围差 ∪ 被调子式的 NamespaceRef 子游标），
// 构造调用保持 token 判据（限定符游标挂在外层 CXXFunctionalCastExpr，
// 内层构造表达式不可达）。每个形态一个独立 caller 函数，断言按
// (caller, callee, is_virtual) 唯一定位。
namespace qns
{
    struct T
    {
        explicit T(int v)
            : v_(v)
        {
        }

        int value() const { return v_; }

      private:
        int v_;
    };

    inline int make(int v) { return v + 1; }
} // namespace qns

struct PlainS
{
    explicit PlainS(int v)
        : val(v)
    {
    }

    int val;
};

struct VBase
{
    virtual ~VBase() = default;
    virtual int vstep(int v) { return v; }
};

struct VDer : VBase
{
    int vstep(int v) override { return v + 1; }
};

int vfree(int v) { return v; }

template <class A>
int vqual_t(A a)
{
    // 判据只看调用表达式形态；两个实参类型（qns::T / PlainS）的成员面
    // 不同，模板体不触碰成员，保证两实例化都能编译。
    (void)a;
    return 1;
}

#define QMACRO_CALL vd->VBase::vstep(2)

// 宏定义与调用之间垫 >4KB 注释：宏调用表达式的 extent 两端 spelling 分落
// 宏定义体与调用行，跨度超 token 卫生闸上限——token 判据在此拒绝并输出
// 非限定（漏报形态），AST 判据不受 spelling 错位影响，仍读出限定真值。
/* --------------------------------------------------------------------------
   限定调用夹具的跨度填充段：这一大段注释存在的唯一目的是把宏定义体与
   宏调用行在文件内的字节距离撑过 token 卫生闸的单语句跨度上限（4096
   字节）。填充内容本身没有语义，不要在这里引入任何会被误认作判据特征
   的文本。以下重复行仅计字节。
   ---------------------------------------------------------------------- */
/* filler-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-f */
/* filler-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-f */
/* filler-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-f */
/* filler-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-f */
/* filler-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-f */
/* filler-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-f */
/* filler-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-f */
/* filler-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-f */
/* filler-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-f */
/* filler-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-f */
/* filler-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-f */
/* filler-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-f */
/* filler-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-f */
/* filler-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-f */
/* filler-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-f */
/* filler-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-f */
/* filler-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-f */
/* filler-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-f */
/* filler-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-f */
/* filler-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-f */
/* filler-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-f */
/* filler-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-f */
/* filler-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-f */
/* filler-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-f */
/* filler-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-f */
/* filler-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-f */
/* filler-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-f */
/* filler-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-f */
/* filler-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-f */
/* filler-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-f */
/* filler-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-f */
/* filler-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-f */
/* filler-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-f */
/* filler-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-f */
/* filler-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-f */
/* filler-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-f */
/* filler-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-f */
/* filler-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-f */
/* filler-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-f */
/* filler-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-f */
/* filler-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-f */
/* filler-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-f */
/* filler-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-f */
/* filler-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-f */
/* filler-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-f */
/* filler-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-f */
/* filler-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-f */
/* filler-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-f */
/* filler-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-f */
/* filler-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-f */
/* filler-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-f */
/* filler-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-f */
/* filler-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-f */
/* filler-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-f */
/* filler-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-f */
/* filler-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-f */
/* filler-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-f */
/* filler-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-fill-f */
int a_qual_member(VDer* vd)
{
    return vd->VBase::vstep(1); // 限定虚调用：qualified=1
}

int b_unqual_member(VDer* vd)
{
    return vd->vstep(1); // 无限定虚调用：qualified=0
}

int c_qual_ns()
{
    return qns::make(1); // 命名空间限定：qualified=1
}

int d_qual_global()
{
    return ::vfree(1); // 全局限定：qualified=1
}

int e_plain()
{
    return vfree(1); // 无限定自由调用：qualified=0
}

int f_arg_qual(VDer* vd)
{
    // 实参里的命名空间限定不得传染给外层调用；内层调用自身是限定的。
    return vd->vstep(qns::make(2)); // vstep: qualified=0；make: qualified=1
}

int g_macro_qual(VDer* vd)
{
    return QMACRO_CALL; // 宏错位形态：token 闸拒绝、AST 读出 qualified=1
}

int h_tmpl_ns_arg()
{
    qns::T t(1);
    return vqual_t<qns::T>(t); // 带命名空间的显式模板实参：qualified=1
}

int i_tmpl_plain_arg()
{
    PlainS s(1);
    return vqual_t<PlainS>(s); // 显式普通类型模板实参不得误判：qualified=0
}

int j_ctor_qual()
{
    (void)qns::T(1); // 限定构造（token 判据保留面）：qualified=1
}

int k_ctor_plain()
{
    (void)PlainS(1); // 无限定构造：qualified=0
}

int main()
{
    VDer der;
    VDer* vd = &der;
    return a_qual_member(vd) + b_unqual_member(vd) + c_qual_ns()
           + d_qual_global() + e_plain() + f_arg_qual(vd) + g_macro_qual(vd)
           + h_tmpl_ns_arg() + i_tmpl_plain_arg() + j_ctor_qual()
           + k_ctor_plain();
}
