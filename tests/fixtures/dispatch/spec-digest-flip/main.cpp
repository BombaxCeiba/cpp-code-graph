// 特化摘要翻转夹具：v1 形态。hierarchy_digest 必须覆盖 Specializes——
// 编辑只增删显式全特化（Box<char>），extends/overrides 边集一字不动，
// 净特化边变化若不翻转摘要，增量批会带着过期继承图静默通过。
// 红绿断言在 tests/analysis.test.ixx 的 spec_digest_flip（digest 前后比对）；
// 边级端到端等价用例在 tests/incr.test.ixx 的 spec_escalation_edges。
struct Device
{
    virtual ~Device();
    virtual int poke(int v) = 0;
};
struct Screen : Device
{
    int poke(int v) override;
};
struct Speaker : Device
{
    int poke(int v) override;
};

template <class T>
struct Box
{
    int describe() const { return 1; }
};
template <>
struct Box<int>
{
    int describe() const { return 2; }
};

int call_all(Device& d);

int main()
{
    Screen s;
    Speaker sp;
    int r = call_all(s) + call_all(sp);
    Box<int> bi;
    Box<double> bd;
    return r + bi.describe() + bd.describe();
}

int call_all(Device& d) { return d.poke(1); }
