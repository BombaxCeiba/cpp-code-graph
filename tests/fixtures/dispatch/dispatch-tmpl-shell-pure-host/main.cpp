// 静态决议补齐夹具：模板壳根清洗（剥 operator-> 后壳主模板行不再以
// 假缺席拖垮最近覆盖封口，final 密封根直连）与宿主抽象直连（被调方法
// 纯虚 + 全库唯一非纯 override，对象存在性论证直连）。

// ---- 模板壳根清洗：包装类主模板行 + pointee 密封根的静态面 ----

struct AcmeMarkHost
{
    virtual int AddMark() = 0;
    virtual ~AcmeMarkHost() = default;
};
struct AcmeMarkImpl final : AcmeMarkHost
{
    int AddMark() override { return 1; }
};
// 家族第二实现（不在接收者静态面）：家族非唯一，直连走 final 类级证据。
struct AcmeOtherMark final : AcmeMarkHost
{
    int AddMark() override { return 9; }
};

// 剥壳包装：operator-> 调用的接收者静态面 = 壳主模板行 + 模板实参，
// 壳自身与目标家族零交集（留在根集会以缺席拖垮封口）。
template <typename T>
class AcmeMarkBox
{
public:
    T* pointee_{nullptr};
    T* operator->() { return pointee_; }
};

// ---- 宿主抽象直连：方法纯虚 + 全库唯一非纯 override ----

struct AcmeTollHub
{
    virtual int Toll() = 0;
    virtual ~AcmeTollHub() = default;
};
struct AcmeTollLane final : AcmeTollHub
{
    int Toll() override { return 2; }
};

// 壳调用站（默认构造无动态证据）：清洗壳根后 final 密封根直连。
int drive_box()
{
    AcmeMarkBox<AcmeMarkImpl> impl;
    return impl->AddMark();
}

// 抽象宿主站（裸空指针防取址证据）：宿主不可实例化，全库唯一 override
// 即唯一目标。
int drive_toll()
{
    AcmeTollHub* hub = nullptr;
    return hub->Toll();
}

int main()
{
    int sink = drive_box() + drive_toll();
    (void)AcmeOtherMark{};
    return sink == 0 ? 0 : 0;
}
