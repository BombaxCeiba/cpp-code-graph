/**
 * @module cg.util.ownedptr
 *
 * @brief 带删除器的拥有型指针 OwnedPtr：为 C 接口经出参交回的资源而设，
 *        成员照组件对象智能指针 ComPtr 的设计来。
 *
 * @internal
 * 本单元是句柄单元的延续：类名留在 cg::util::handle 里，与句柄拥有者
 * Owned 并列，由句柄单元再导出；单独成单元是为了不让句柄单元超出行数
 * 预算。句柄拥有者持有的是句柄值，资源由接口的返回值交回，取用走
 * adopt。C 接口另有一批资源经出参交回（预编译语句、接口指针、运行时上
 * 下文），带删除器的标准唯一指针交不出内部指针的地址，调用处只能先用
 * 裸指针接住出参、再包一层，资源在两步之间无人持有。OwnedPtr 自己交出
 * 内部指针的地址，出参直接落进拥有者。只依赖标准库，不分配堆内存，可
 * 以放在钩子与子进程派生的路径上。删除器在句柄单元里。
 */
export module cg.util.ownedptr;

import std;

export namespace cg::util::handle
{

    /**
     * @brief 一个指针连同它的删除器的拥有者：析构时经删除器释放指针指
     *        向的资源。
     *
     * @internal
     * 成员照组件对象智能指针 ComPtr 的设计来：默认构造对应 ComPtr 的默认
     * 构造，get 对应 Get，取成员运算符对应 ComPtr 的取成员运算符，adopt
     * 对应 Attach，release 对应 Detach，reset 对应 Reset，get_address_of
     * 对应 GetAddressOf，release_and_get_address_of 与取地址运算符对应
     * ReleaseAndGetAddressOf 与取地址运算符，布尔转换对应 ComPtr 的布尔
     * 转换。ComPtr 靠引用计数
     * 可以拷贝；本类独占资源，只能移动。只带删除器的构造函数与
     * get_deleter 是本类特有的成员：删除器可以带状态，有的状态（交还时
     * 要带的个数）要到接口调用之后才知道，经 get_deleter 补进删除器。
     * 命名与句柄拥有者 Owned 一致（adopt、reset、release、get、valid）。
     * T 是指针指向的类型，字符缓冲传元素类型 char。
     *
     * 空指针不调删除器。删除器的调用只在 reset 一处，析构、移动赋值与
     * 先放掉再交出地址的两个入口都经它。只能移动，被移动走的对象为空。
     * 析构、移动、接管与交出地址都不抛，所以删除器的调用与移动也不许
     * 抛，实例化时检查。默认构造与 adopt 省略删除器时用到删除器的默认
     * 构造。不分配堆内存；删除器是空类时，对象与单个裸指针一样大。
     *
     * 取地址运算符交出的是内部指针的地址，不是对象自己的地址。所以这类
     * 对象不能放进标准容器，也不能靠取地址得到对象自己的地址；要对象自
     * 己的地址用 std::addressof。交出去的地址只交给紧跟着的那一次 C 接
     * 口调用，在它返回之前不要移动或释放这个对象。
     */
    template <typename T, typename Deleter>
    class OwnedPtr
    {
        /** @internal 删除器的调用与移动都不抛，本类各处的不抛许诺才成立。 */
        static_assert(
            std::is_nothrow_move_constructible_v<Deleter>
                && std::is_nothrow_move_assignable_v<Deleter>,
            "删除器的移动不许抛出");
        static_assert(
            std::is_nothrow_invocable_v<Deleter&, T*>,
            "删除器的调用不许抛出");

    public:
        /** @brief 不持有资源（ComPtr 的默认构造）；删除器取默认构造的那个。 */
        OwnedPtr() = default;

        /**
         * @brief 不持有资源，只记下一个删除器。
         *
         * @internal
         * 删除器带状态时（关闭函数、映射长度）先用它构造，再把地址交给
         * C 接口。
         */
        explicit OwnedPtr(Deleter deleter) noexcept
            : deleter_{std::move(deleter)}
        {
        }

        OwnedPtr(const OwnedPtr&) = delete;
        auto operator=(const OwnedPtr&) -> OwnedPtr& = delete;

        /** @brief 被移动走的对象为空。 */
        OwnedPtr(OwnedPtr&& other) noexcept
            : pointer_{std::exchange(other.pointer_, nullptr)},
              deleter_{std::move(other.deleter_)}
        {
        }

        /** @brief 先放掉自己原有的资源再接管对方的；自己赋给自己保持原样。 */
        auto operator=(OwnedPtr&& other) noexcept -> OwnedPtr&
        {
            // 取地址运算符已被本类改写，对象的地址只能经 std::addressof 取。
            if (this != std::addressof(other))
            {
                reset();
                pointer_ = std::exchange(other.pointer_, nullptr);
                deleter_ = std::move(other.deleter_);
            }
            return *this;
        }

        ~OwnedPtr() { reset(); }

        /**
         * @brief 接管一个已经取到的资源（ComPtr::Attach）；空指针表示接口
         *        没有交出资源，对象保持为空。
         */
        [[nodiscard]]
        static auto adopt(T* pointer, Deleter deleter = Deleter{}) noexcept
            -> OwnedPtr
        {
            return OwnedPtr{pointer, std::move(deleter)};
        }

        /** @brief 是否持有资源（ComPtr 没有对应的成员，与 Owned 的 valid
         *          同义）。 */
        [[nodiscard]]
        auto valid() const noexcept -> bool
        {
            return pointer_ != nullptr;
        }

        /**
         * @brief 裸指针，只供传给系统调用（ComPtr::Get）；不持有资源时是
         *        空指针。
         */
        [[nodiscard]]
        auto get() const noexcept -> T*
        {
            return pointer_;
        }

        /**
         * @brief 经持有的指针访问成员（ComPtr 的取成员运算符）；不持有资
         *        源时不得使用。
         */
        [[nodiscard]]
        auto operator->() const noexcept -> T*
        {
            return pointer_;
        }

        /** @brief 立即释放资源，对象变为空（ComPtr::Reset）；本来就不持有
         *          则不做任何事。 */
        auto reset() noexcept -> void
        {
            T* const pointer = std::exchange(pointer_, nullptr);
            if (pointer != nullptr)
            {
                deleter_(pointer);
            }
        }

        /**
         * @brief 交出所有权（ComPtr::Detach）：返回裸指针，此后本对象不再
         *        持有它，析构也不会释放它。
         */
        [[nodiscard]]
        auto release() noexcept -> T*
        {
            return std::exchange(pointer_, nullptr);
        }

        /**
         * @brief 删除器；带状态的删除器在取用之后才补全状态时经它修改。
         *
         * @internal
         * 例子：引擎交还记号数组时要带上记号个数，个数要到取记号的那次
         * 调用返回才知道。修改删除器不改变持有状态。
         */
        [[nodiscard]]
        auto get_deleter() noexcept -> Deleter&
        {
            return deleter_;
        }

        [[nodiscard]]
        auto get_deleter() const noexcept -> const Deleter&
        {
            return deleter_;
        }

        /**
         * @brief 内部指针的地址，不改变持有状态（ComPtr::GetAddressOf）。
         *
         * @internal
         * 给只读指针值的调用用。让 C 接口往里写资源要用
         * release_and_get_address_of：对象已持有资源时经这里写进来，原
         * 有的资源就漏掉了。
         */
        [[nodiscard]]
        auto get_address_of() noexcept -> T**
        {
            return &pointer_;
        }

        /**
         * @brief 先放掉已持有的资源，再交出内部指针的地址
         *        （ComPtr::ReleaseAndGetAddressOf），供 C 接口把资源写进来。
         *
         * @internal
         * 返回之后对象为空：接口没有写进来时它保持为空；写进来时它是唯一
         * 的拥有者，接口失败时留下的半成品资源也由它回收。
         */
        [[nodiscard]]
        auto release_and_get_address_of() noexcept -> T**
        {
            reset();
            return &pointer_;
        }

        /**
         * @brief 同 release_and_get_address_of（ComPtr 的取地址运算符）：
         *        让 &obj 可以直接当出参传给 C 接口。
         *
         * @internal
         * 交出的不是对象自己的地址，限制见类注释。
         */
        [[nodiscard]]
        auto operator&() noexcept -> T**
        {
            return release_and_get_address_of();
        }

        /** @brief 是否持有资源（ComPtr 的布尔转换）。 */
        explicit operator bool() const noexcept { return valid(); }

    private:
        /** @brief 接管一个指针并记下它的删除器。 */
        OwnedPtr(T* pointer, Deleter deleter) noexcept
            : pointer_{pointer}, deleter_{std::move(deleter)}
        {
        }

        /** @brief 指向被持有的资源；空指针表示不持有。 */
        T* pointer_ = nullptr;

        /** @brief 删除器。 */
        [[no_unique_address]]
        Deleter deleter_{};
    };

} // namespace cg::util::handle
