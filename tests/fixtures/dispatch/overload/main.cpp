// 同名重载夹具（非模板 + 模板）：模板上下文调用走模板版。
struct Ret
{
    int code = 0;
};
inline bool IsFailed(const Ret& r) { return r.code != 0; } // 非模板版
template <class T>
inline bool IsFailed(const T& t)
{ // 模板版
    (void)t;
    return false;
}
struct Wrapper
{
    template <class Q>
    Q qi_result() const
    {
        return Q{};
    }
};
template <class T>
int host_t(T x)
{
    return IsFailed(x) ? 1 : 0;
} // 依赖实参 → 模板版
int host_c(Ret r) { return IsFailed(r) ? 1 : 0; } // 具体实参 → 非模板版
int main()
{
    Ret r;
    return host_c(r) + host_t(r);
}
