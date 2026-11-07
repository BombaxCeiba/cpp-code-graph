module;
#include "util.json.bridge.h"

/**
 * @module cg.util.json
 *
 * @brief 对 yyjson（唯一许可的 JSON 依赖，仅经此模块可达）的最小 JSON 门面：
 *        读侧（Document/ValueView）服务编译数据库加载器，写侧（Object/Array/
 *        stringify）服务 CLI 渲染器。导入方永远不应直接引用 yyjson 类型。
 *
 * @internal
 * 桥接动机（MSVC 移植二分探针 v1-v6 收敛的结论）：MSVC 14.51 的模块 IFC
 * writer 对模块单元内出现的任何 yyjson-cpp 类型都会触发 C1001（module
 * writer.cpp:2210 "sorry: not yet implemented"）——即使该类型只作导出类的
 * 私有成员、且无论有无 import std；同样的调用在经典 TU 中实例化则正常。
 * 因此全部 yyjson 状态经 util.json.bridge.cpp（经典翻译单元）以不透明
 * 句柄隔离，本模块的全局模块片段保持不含 STL 与 yyjson 的 include。
 */
export module cg.util.json;

import std;

export namespace cg::util::json
{

    /**
     * @brief 文档内单个 JSON 值的非拥有视图。
     *
     * 仅在产出它的 Document 存活期间有效。可拷贝、永不拥有；
     * 内部句柄是由 bridge 持有的借入式 yyjson 值指针。
     */
    class ValueView
    {
    public:
        /**
         * @brief 包装裸 bridge 值句柄的内部逃生口。
         *
         * @internal 供 for_each_member 使用：其模板体在导入方 TU 中实例化，
         * 需要此构造函数在导入侧可达。不属于稳定的门面表面。
         */
        explicit ValueView(const void* handle) noexcept : handle_{handle} {}

        /** @brief 当前值是否为字符串。 */
        [[nodiscard]]
        auto is_string() const noexcept -> bool
        {
            return ccg_json_is_string(handle_) != 0;
        }
        /** @brief 当前值是否为数组。 */
        [[nodiscard]]
        auto is_array() const noexcept -> bool
        {
            return ccg_json_is_array(handle_) != 0;
        }
        /** @brief 当前值是否为对象。 */
        [[nodiscard]]
        auto is_object() const noexcept -> bool
        {
            return ccg_json_is_object(handle_) != 0;
        }
        /** @brief 当前值是否为布尔。 */
        [[nodiscard]]
        auto is_bool() const noexcept -> bool
        {
            return ccg_json_is_bool(handle_) != 0;
        }
        /** @brief 当前值是否为数字。 */
        [[nodiscard]]
        auto is_number() const noexcept -> bool
        {
            return ccg_json_is_number(handle_) != 0;
        }

        /**
         * @brief 以字符串视图读取当前值。
         *
         * 视图指向文档缓冲区，如需长期持有须自行拷出。
         * @return 字符串内容；值不是字符串时返回 std::nullopt。
         */
        [[nodiscard]]
        auto as_string() const noexcept -> std::optional<std::string_view>
        {
            if (std::size_t len = 0;
                const char* s = ccg_json_str(handle_, &len))
            {
                return std::string_view{s, len};
            }
            return std::nullopt;
        }
        /**
         * @brief 以布尔读取当前值。
         * @return 值内容；不是布尔或读取失败时返回 std::nullopt。
         */
        [[nodiscard]]
        auto as_bool() const noexcept -> std::optional<bool>
        {
            int        ok = 0;
            const bool value = ccg_json_bool(handle_, &ok) != 0;
            return ok != 0 ? std::optional{value} : std::nullopt;
        }
        /**
         * @brief 以整数读取当前值。
         * @return 值内容；不是数字或读取失败时返回 std::nullopt。
         */
        [[nodiscard]]
        auto as_int() const noexcept -> std::optional<std::int64_t>
        {
            int                ok = 0;
            const std::int64_t value = ccg_json_int(handle_, &ok);
            return ok != 0 ? std::optional{value} : std::nullopt;
        }

        /** @brief 数组元素数或对象成员数；其他任何类型一律返回 0。 */
        [[nodiscard]]
        auto size() const -> std::size_t
        {
            return ccg_json_size(handle_);
        }

        /**
         * @brief 取数组第 index 个元素。
         * @return 元素视图；越界或视图不是数组时返回 std::nullopt。
         */
        [[nodiscard]]
        auto at(std::size_t index) const -> std::optional<ValueView>
        {
            if (const void* element = ccg_json_arr_at(handle_, index))
            {
                return ValueView{element};
            }
            return std::nullopt;
        }

        /**
         * @brief 取对象中键为 key 的成员。
         * @return 成员视图；成员不存在或视图不是对象时返回 std::nullopt。
         */
        [[nodiscard]]
        auto get(std::string_view key) const -> std::optional<ValueView>
        {
            if (const void* member =
                    ccg_json_obj_get(handle_, key.data(), key.size()))
            {
                return ValueView{member};
            }
            return std::nullopt;
        }

        /**
         * @brief 按文档顺序访问对象的每个成员；视图不是对象时为空操作。
         *
         * 键视图在产出此视图的 Document 存活期间有效。
         *
         * @internal
         * 模板体只使用内建类型、bridge 的 extern 函数与本类的导出构造函数。
         * 约束来自 MSVC 模块移植的探针规则：导出模块函数不能是全局模块片段中
         * 静态内联 C 帮助函数的内联包装（导入方 TU 实例化时会因 statics 不可达
         * 报 C2129）；bridge 一律以经典 TU 中的 extern 函数暴露，故此模板在
         * 导入方 TU 中的实例化不会回放任何全局模块声明。
         */
        template <typename Fn>
            requires std::invocable<const Fn&, std::string_view, ValueView>
        void for_each_member(Fn&& fn) const
        {
            const std::size_t count = ccg_json_size(handle_);
            for (std::size_t index = 0; index < count; ++index)
            {
                const char* key = nullptr;
                std::size_t key_len = 0;
                if (const void* member =
                        ccg_json_member_at(handle_, index, &key, &key_len))
                {
                    fn(std::string_view{key, key_len}, ValueView{member});
                }
            }
        }

    private:
        const void* handle_ = nullptr;
    };

    /**
     * @brief 对单份已解析 JSON 文档的拥有式句柄。
     *
     * 解析失败时抛出携带 yyjson 读取器错误消息的 std::runtime_error。
     */
    class Document
    {
    public:
        Document() = default;
        Document(Document&& other) noexcept
            : doc_{std::exchange(other.doc_, nullptr)}
        {
        }
        Document(const Document&) = delete;
        Document& operator=(const Document&) = delete;
        Document& operator=(Document&&) = delete;
        ~Document() { ccg_json_doc_free(doc_); }

        /**
         * @brief 解析 JSON 文本。
         *
         * @param text 须为完整、UTF-8 编码的 JSON 文本。
         * @return 拥有解析结果的 Document。
         * @throws std::runtime_error 输入格式非法时抛出，消息含 yyjson 读取器
         *                          的错误描述（缺失时为 unknown error）。
         */
        [[nodiscard]]
        static auto parse(std::string_view text) -> Document
        {
            const char* error = nullptr;
            void* doc = ccg_json_doc_parse(text.data(), text.size(), &error);
            if (doc == nullptr)
            {
                throw std::runtime_error{
                    std::string{"cg json parse: "}
                    + (error != nullptr ? error : "unknown error")};
            }
            return Document{doc};
        }

        /** @brief 返回文档根值的视图。 */
        [[nodiscard]]
        auto root() const noexcept -> ValueView
        {
            return ValueView{ccg_json_doc_root(doc_)};
        }

    private:
        explicit Document(void* doc) noexcept : doc_{doc} {}

        void* doc_ = nullptr; // bridge 文档句柄（本类拥有）
    };

    /**
     * @brief JSON 对象构建器（写侧）。
     *
     * Object 与 Array 构建器各自拥有其 yyjson 文档；将一个移动进父节点即由
     * 父节点接管其存储。键与字符串值总是被拷贝进文档，调用方可安全传入短命
     * 字符串上的视图。键假定唯一：本门面的构建器对每个键至多写一次。
     */
    class Array;

    class Object
    {
    public:
        Object() = default;
        Object(Object&& other) noexcept
            : writer_{std::exchange(other.writer_, nullptr)}
        {
        }
        Object(const Object&) = delete;
        Object& operator=(const Object&) = delete;
        Object& operator=(Object&&) = delete;
        ~Object() { ccg_json_writer_free(writer_); }

        /** @brief 写入标量成员：整数类型（含 bool）与浮点值。 */
        template <typename T>
            requires std::integral<T> || std::floating_point<T>
        void set(std::string_view key, T value)
        {
            ensure();
            if constexpr (std::same_as<T, bool>)
            {
                ccg_json_obj_set_bool(writer_, key.data(), key.size(), value);
            }
            else if constexpr (std::floating_point<T>)
            {
                ccg_json_obj_set_real(
                    writer_,
                    key.data(),
                    key.size(),
                    static_cast<double>(value));
            }
            else
            {
                ccg_json_obj_set_int(
                    writer_,
                    key.data(),
                    key.size(),
                    static_cast<std::int64_t>(value));
            }
        }

        /** @brief 写入字符串成员（值拷贝进文档）。 */
        void set(std::string_view key, std::string_view value)
        {
            ensure();
            ccg_json_obj_set_str(
                writer_,
                key.data(),
                key.size(),
                value.data(),
                value.size());
        }

        /** @brief 写入 null 成员。 */
        void set_null(std::string_view key)
        {
            ensure();
            ccg_json_obj_set_null(writer_, key.data(), key.size());
        }

        /** @brief 写入嵌套对象成员，nested 的存储移交给本对象。 */
        void set(std::string_view key, Object&& nested);
        /** @brief 写入嵌套数组成员，nested 的存储移交给本对象。 */
        void set(std::string_view key, Array&& nested);

    private:
        friend class Array;
        friend auto stringify(Object&& root) -> std::string;

        /**
         * @internal bridge 写出器惰性物化，使默认构造保持零分配
         * （对齐被其取代的 yyjson-cpp 构建器行为）。
         */
        void ensure() { writer_ = ccg_json_writer_ensure(writer_, 0); }

        void* writer_ = nullptr; // bridge 写出器句柄（本类拥有；对象根）
    };

    /**
     * @brief JSON 数组构建器（写侧）。
     *
     * 所有权与拷贝语义同 Object：自带 yyjson 文档，移动进父节点即由父节点
     * 接管存储；字符串值总是拷贝进文档。
     */
    class Array
    {
    public:
        Array() = default;
        Array(Array&& other) noexcept
            : writer_{std::exchange(other.writer_, nullptr)}
        {
        }
        Array(const Array&) = delete;
        Array& operator=(const Array&) = delete;
        Array& operator=(Array&&) = delete;
        ~Array() { ccg_json_writer_free(writer_); }

        /** @brief 追加标量元素：整数类型（含 bool）与浮点值。 */
        template <typename T>
            requires std::integral<T> || std::floating_point<T>
        void add(T value)
        {
            ensure();
            if constexpr (std::same_as<T, bool>)
            {
                ccg_json_arr_add_bool(writer_, value);
            }
            else if constexpr (std::floating_point<T>)
            {
                ccg_json_arr_add_real(writer_, static_cast<double>(value));
            }
            else
            {
                ccg_json_arr_add_int(writer_, static_cast<std::int64_t>(value));
            }
        }

        /** @brief 追加字符串元素（值拷贝进文档）。 */
        void add(std::string_view value)
        {
            ensure();
            ccg_json_arr_add_str(writer_, value.data(), value.size());
        }

        /** @brief 追加嵌套对象，nested 的存储移交给本数组。 */
        void add(Object&& nested);
        /** @brief 追加嵌套数组，nested 的存储移交给本数组（二维数组场景）。 */
        void add(Array&& nested);

    private:
        friend class Object;
        friend auto stringify(Array&& root) -> std::string;

        /**
         * @internal bridge 写出器惰性物化，使默认构造保持零分配
         * （对齐被其取代的 yyjson-cpp 构建器行为）。
         */
        void ensure() { writer_ = ccg_json_writer_ensure(writer_, 1); }

        void* writer_ = nullptr; // bridge 写出器句柄（本类拥有；数组根）
    };

    /**
     * @brief 将嵌套对象构建器并入父树（键 key 下挂接 nested）。
     *
     * @internal
     * 嵌套写出器经 bridge 挂进父树（其分配随父节点存活）；被移空的构建器
     * 保持空态，析构时不释放任何东西。先物化嵌套写出器再挂接，是为保住
     * yyjson-cpp 门面的空构建器语义：从未动过的嵌套 Object/Array 序列化为
     * "{}"/"[]"，而不是键缺失。
     */
    inline void Object::set(std::string_view key, Object&& nested)
    {
        ensure();
        nested.ensure();
        ccg_json_obj_set_writer(
            writer_,
            key.data(),
            key.size(),
            nested.writer_);
        nested.writer_ = nullptr;
    }

    /**
     * @brief 将嵌套数组构建器并入父树（键 key 下挂接 nested）。语义同 Object
     * 重载。
     */
    inline void Object::set(std::string_view key, Array&& nested)
    {
        ensure();
        nested.ensure();
        ccg_json_obj_set_writer(
            writer_,
            key.data(),
            key.size(),
            nested.writer_);
        nested.writer_ = nullptr;
    }

    /**
     * @brief 将嵌套对象构建器追加进数组。语义同 Object::set 的嵌套挂接。
     */
    inline void Array::add(Object&& nested)
    {
        ensure();
        nested.ensure();
        ccg_json_arr_add_writer(writer_, nested.writer_);
        nested.writer_ = nullptr;
    }

    /**
     * @brief 将嵌套数组追加进数组。语义同 Array::add(Object&&) 的嵌套挂接。
     */
    inline void Array::add(Array&& nested)
    {
        ensure();
        nested.ensure();
        ccg_json_arr_add_writer(writer_, nested.writer_);
        nested.writer_ = nullptr;
    }

    /**
     * @brief 将文档序列化为紧凑 JSON 文本。
     *
     * 结果完全拥有。从未动过的构建器序列化为其空根（"{}"）。
     *
     * @internal
     * bridge 的写出缓冲区在其被释放前已拷出。此门面最初正是为绕开 yyjson-cpp
     * 的一个坑而写：其 write() 返回的 json_string 派生自 std::string_view 且
     * 自身拥有并释放 yyjson 缓冲区——把它当作超出该临时对象寿命的
     * string_view 使用会静默悬垂（探针中实际踩中），故必须先拷出再释放。
     */
    [[nodiscard]]
    inline auto stringify(Object&& root) -> std::string
    {
        root.ensure();
        std::size_t len = 0;
        char* const buffer = ccg_json_writer_write(root.writer_, &len);
        if (buffer == nullptr)
        {
            return {};
        }
        std::string owned{std::string_view{buffer, len}};
        ccg_json_buf_free(buffer);
        return owned;
    }

    /**
     * @brief stringify(Object&&) 的数组根孪生版本。
     *
     * clang-scan-deps 合成的模块依赖编译数据库是顶层数组，此重载为其服务。
     */
    [[nodiscard]]
    inline auto stringify(Array&& root) -> std::string
    {
        root.ensure();
        std::size_t len = 0;
        char* const buffer = ccg_json_writer_write(root.writer_, &len);
        if (buffer == nullptr)
        {
            return {};
        }
        std::string owned{std::string_view{buffer, len}};
        ccg_json_buf_free(buffer);
        return owned;
    }

    /**
     * @brief 字符串列表编码为 JSON 数组文本（稀疏 param/template 列、类型
     *        集/实参组的仓内单点；排序由调用方负责）。
     *
     * 字符串转义由本门面保证——类类型模板实参的拼写可含引号字面量，手写
     * 零转义版本击穿时写侧产出非法 JSON、读侧静默解析为空。
     */
    [[nodiscard]]
    inline auto json_string_array(std::span<const std::string> items)
        -> std::string
    {
        Array arr;
        for (const std::string& item : items)
        {
            arr.add(item);
        }
        return stringify(std::move(arr));
    }

} // namespace cg::util::json
