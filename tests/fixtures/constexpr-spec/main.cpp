// constexpr 声明检测判据的形态谱（断言面：各符号的 is_constexpr 旗标）。
// 检测基于编译器对 AST 的重打印文本：宏生成的说明符与名字都以展开后
// 的形态出现在打印里，源文件前缀文本的宏别名不参与判定。
//
// 方向性锚点：
//   macro_specifier_fn —— 说明符由宏生成：源前缀只见宏别名（旧判据漏报
//                         0），打印文本已展开为 constexpr 本词（判 1）。
//   body_leak_fn       —— 名字由宏生成 + 函数体内带 constexpr 局部量：
//                         旧回退把整个 extent 词法化吃进体内局部（误报
//                         1），打印文本截到函数体起点即拒绝（判 0）。
//   requires_compound_brace_fn / nttp_brace_default_fn —— 说明符之前的
//                         花括号组（requires 复合要求的体 / 类类型 NTTP
//                         花括号默认值）曾触发截断使说明符不可达（漏报
//                         0），语境感知配对跳过后说明符可达（判 1）。
//   AttrDefaulted / AttrStrEvalDefaulted —— defaulted 补面前缀里的属性
//                         实参字符串恰含说明符本词（曾误报 1），补面剥
//                         字符串后拒绝（判 0）。

#define CONSTEXPR_ALIAS constexpr
#define PLAIN_NAME_ALIAS macro_named_fn
#define LEAK_NAME_ALIAS body_leak_fn

// 说明符由宏生成（旧前缀判据漏报形态；打印文本里是展开后的说明符）。
CONSTEXPR_ALIAS int macro_specifier_fn() { return 1; }

// 名字由宏生成 + 说明符明写：打印文本头部含展开后的名字与本词说明符。
constexpr int PLAIN_NAME_ALIAS() { return 2; }

// 名字由宏生成 + 无说明符 + 体内 constexpr 局部量：局部量只属于函数体，
// 不得把整个函数误标为 constexpr（旧回退的误报形态）。
int LEAK_NAME_ALIAS()
{
    constexpr int body_local = 1;
    return body_local;
}

#undef CONSTEXPR_ALIAS
#undef PLAIN_NAME_ALIAS
#undef LEAK_NAME_ALIAS

// 基础说明符谱。
constexpr int constexpr_free_fn() { return 3; }
consteval int consteval_free_fn() { return 4; }
int plain_free_fn() { return 5; }
static /* 块注释拦在说明符段中间 */ constexpr int comment_split_fn()
{
    return 6;
}
[[nodiscard]] constexpr int attr_then_constexpr_fn() { return 7; }
template <typename T>
constexpr int tmpl_constexpr_fn(T v) { return static_cast<int>(v); }

// 属性实参是含 '#' 的字符串字面量：打印文本剥掉字符串后说明符照常命中。
[[deprecated("#x")]] constexpr int hash_attr_constexpr_fn() { return 8; }

// 变量谱：说明符命中、普通拒绝、字符串内容干扰拒绝。
constexpr int constexpr_var = 9;
int plain_var = 0;
const char* string_content_var = "constexpr";

// 类内谱：无说明符构造的体内 constexpr 局部量不得外溢；constexpr 方法
// 必须命中。
struct CtorBodyProbe
{
    CtorBodyProbe(int v)
    {
        constexpr int ctor_local = 1;
        (void)ctor_local;
        (void)v;
    }
    constexpr int const_method() const { return 10; }
};

// defaulted/deleted 谱：打印器对 "= default" 分支结构性丢弃 constexpr
// 说明符（各打印策略开关下均如此），defaulted 形态的说明符只能回源
// 前缀直读；"= delete" 与常规函数的打印保留说明符。
struct DefaultedProbe
{
    constexpr DefaultedProbe() noexcept = default;
};

struct DefaultedPlainCopy
{
    DefaultedPlainCopy() = default;
    DefaultedPlainCopy(const DefaultedPlainCopy&) noexcept = default;
};

constexpr int deleted_free_fn() = delete;

// 说明符之前的花括号谱：requires 复合要求的体与类类型非类型模板参数的
// 花括号默认值都在说明符之前——它们不是函数体，截断语义不得把说明符截
// 在身后（曾漏报的形态：requires/NTTP 花括号触发截断）。
struct RangeTag
{
    using value_type = int;
    int value() const noexcept { return 1; }
};

// requires 复合要求的体（花括号组在说明符之前）。
template <typename T>
    requires requires { typename T::value_type; }
constexpr int requires_compound_brace_fn(T v)
{
    return static_cast<int>(sizeof(v));
}

// 对照：requires 子句是括号表达式（无花括号），不经跳过路径。
template <typename T>
    requires (sizeof(T) >= 1)
constexpr int requires_paren_fn(T v)
{
    return static_cast<int>(sizeof(v));
}

// 复合要求体内再嵌花括号（无类型约束的 noexcept 复合要求）：配对跳过
// 的嵌套计数守卫。
template <typename T>
    requires requires(const T& t) { { t.value() } noexcept; }
constexpr int requires_nested_brace_fn(const T& t)
{
    return t.value();
}

// requires 体之后的函数体必须仍然截断：体内 constexpr 局部量不得把无
// 说明符的函数误标（跳过逻辑的体内污染守卫）。
template <typename T>
    requires requires { typename T::value_type; }
int requires_body_local_fn(T v)
{
    constexpr int local = 1;
    return local + static_cast<int>(sizeof(v));
}

struct NttpValueType
{
    int v;
};

// 类类型非类型模板参数的花括号默认值（模板形参表内的花括号组）。
template <NttpValueType N = NttpValueType{}>
constexpr int nttp_brace_default_fn()
{
    return N.v;
}

// 对照：非类型模板参数无花括号默认值，不经跳过路径。
template <int N = 3>
constexpr int nttp_plain_default_fn()
{
    return N;
}

// defaulted 补面的字符串谱：属性实参字符串恰含说明符本词——字符串内
// 容不是说明符（补面词匹配剥注释之外还要剥字符串字面量；曾误报的
// 形态：前缀起点含前导属性，字符串内容被引号切开成独立词命中）。
struct AttrDefaulted
{
    [[deprecated("constexpr")]] AttrDefaulted() = default;
};

struct AttrStrEvalDefaulted
{
    [[deprecated("consteval")]] AttrStrEvalDefaulted() = default;
};

// 对照：属性字符串 + 真说明符，补面剥字符串后的正向命中。
struct AttrStrRealCx
{
    [[deprecated("no")]] constexpr AttrStrRealCx() = default;
};

// 函数体超过单语句 token 扫描跨度上限：判定只看声明头，函数体多大都不
// 参与扫描。
constexpr int huge_body_fn()
{
    return 11;
    /* 大函数体填充开始（>4096 字节；判定只看声明头，函数体不参与扫描），
zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz
zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz
zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz
zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz
zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz
zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz
zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz
zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz
zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz
zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz
zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz
zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz
zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz
zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz
zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz
zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz
zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz
zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz
zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz
zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz
zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz
zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz
zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz
zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz
zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz
zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz
zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz
zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz
zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz
zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz
zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz
zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz
zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz
zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz
zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz
zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz
zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz
zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz
zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz
zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz
zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz
zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz
zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz
    大函数体填充结束 */
}

// 面外对照：函数体内局部量不是符号（存储经济）；纯声明不是定义，
// function_of 不收录。
void local_scope_only()
{
    constexpr int local_in_fn = 1;
    (void)local_in_fn;
}

consteval int declared_only_fn();

int main()
{
    RangeTag tag{};
    return constexpr_free_fn() + macro_specifier_fn()
           + requires_compound_brace_fn(tag) + requires_paren_fn(tag)
           + requires_nested_brace_fn(tag) + requires_body_local_fn(tag)
           + nttp_brace_default_fn<NttpValueType{7}>()
           + nttp_plain_default_fn<3>();
}
