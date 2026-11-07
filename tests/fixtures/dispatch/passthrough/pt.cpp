// 透传包装判据夹具（槽位基建）：atomic/optional/ptr/deref 四形态 +
// 双形参 + 三反例。消费函数防死代码消除并给透传闸留用。
template <class T>
struct WrapAtomic
{ // 值形态 getter（atomic<T>::load 同形）
    T v{};
    T load() { return v; }
};

template <class T>
struct WrapOpt
{ // 值形态第二式（optional<T>::value 同形）
    T v{};
    T value() { return v; }
};

template <class T>
struct WrapPtr
{ // 指针形态（shared_ptr<T>::get 同形）
    T* p{};
    T* get() { return p; }
};

template <class T>
struct WrapDeref
{ // operator-> 形态（指针式壳：T* 存储 + operator-> + 透传 getter；与 get 并存双登记）
    T* p{};
    T* operator->() { return p; }
    T* get() { return p; }
};

template <class A, class B>
struct WrapTwo
{ // 双形参：pick 返回第 2 个（arg_index=1）
    A a{};
    B b{};
    B pick() { return b; }
};

struct NotTemplate
{ // 反例：非模板
    int* get() { return nullptr; }
};

template <class T>
struct NoGetter
{ // 反例：返回不含模板形参
    T    v{};
    void use() {}
};

template <class T>
struct WithArgs
{ // 反例：getter 带参
    T v{};
    T get(int) { return v; }
};

struct Consumer
{
    int consume()
    {
        WrapAtomic<int>        a;
        WrapOpt<int>           o;
        WrapPtr<Consumer>      p;
        WrapDeref<Consumer>    d;
        WrapTwo<int, Consumer> t;
        NotTemplate            n;
        NoGetter<int>          ng;
        WithArgs<int>          wa;
        return a.load() + o.value() + (p.get() != nullptr)
               + (d.operator->() != nullptr) + t.pick().consume()
               + (n.get() != nullptr) + ng.v + wa.get(1);
    }
};

int main() { return Consumer{}.consume(); }
