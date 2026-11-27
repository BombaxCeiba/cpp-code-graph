module;
// SQLite 原生接口的声明与结果码宏：宏不跨模块边界，头文件只能放在使用
// 它的单元的全局模块片段里。图存储单元与包装器单元各自包含同一个头
// 文件，声明重复不产生定义冲突。
#include <sqlite3.h>

/**
 * @module cg.db.native
 *
 * @brief SQLite 原生接口的拥有型封装：预编译语句、库分配的文本，以及
 *        执行一段语句文本的助手。
 *
 * @internal
 * 采集库的引导步骤需要手写语句：类型化查询库不提供预编译日志模式查询，
 * 也没有建表语句迁移的写法，所以经这里的语句对象与执行助手完成。
 * 图存储的三张旁路表不在这里：它们的语句由图存储单元自己的拥有型指针持有，
 * 图存储单元只从这里取库分配文本的拥有者。语句与文本的指针在这里交给带
 * 删除器的拥有型指针持有，销毁调用只出现在删除器的函数体里；库经出参交
 * 回的指针直接写进拥有者，不经过裸指针。用到的单元只调用成员函数，不直
 * 接向库取资源，也不写释放调用。除标准库外只导入句柄单元，
 * 图存储单元导入它不成环。
 */
export module cg.db.native;

import std;
import cg.util.handle;

export namespace cg::db
{

    /**
     * @brief 库分配的一段文本的拥有者：析构时交还给库。
     *
     * @internal
     * 执行失败的错误串、展开后的语句文本都由库分配，必须经库的释放函
     * 数交还。只能移动；被移动走的对象不持有文本，取到的视图为空。文
     * 本指针由带删除器的拥有型指针持有，空指针不调删除器。
     */
    class NativeText
    {
        /** @brief 库分配的文本的删除器；库的释放函数只在这一处出现。 */
        struct Deleter
        {
            auto operator()(char* text) const noexcept -> void
            {
                sqlite3_free(text);
            }
        };

        /** @brief 文本指针的拥有者。 */
        using Owner = cg::util::handle::OwnedPtr<char, Deleter>;

    public:
        /** @brief 不持有文本。 */
        NativeText() = default;

        /** @brief 接管库分配的一段文本；空指针表示库没有给出文本。 */
        explicit NativeText(char* text) noexcept : text_{Owner::adopt(text)} {}

        NativeText(const NativeText&) = delete;
        auto operator=(const NativeText&) -> NativeText& = delete;
        NativeText(NativeText&& other) noexcept = default;
        auto operator=(NativeText&& other) noexcept -> NativeText& = default;
        ~NativeText() = default;

        /** @brief 是否持有文本。 */
        [[nodiscard]]
        auto valid() const noexcept -> bool
        {
            return text_.valid();
        }

        /** @brief 文本内容；不持有文本时为空。 */
        [[nodiscard]]
        auto view() const noexcept -> std::string_view
        {
            const char* text = text_.get();
            if (text == nullptr)
            {
                return {};
            }
            return {text, std::strlen(text)};
        }

        /**
         * @brief 交出内部指针的地址，供库把文本写进来；先放掉已持有的
         *        文本。
         *
         * @internal
         * 只供本单元的执行助手交给库的出参；其他单元不直接向库取资源。
         */
        [[nodiscard]]
        auto release_and_get_address_of() noexcept -> char**
        {
            return text_.release_and_get_address_of();
        }

    private:
        Owner text_;
    };

    /**
     * @brief 一次步进的结局：库的结果码原值，外加三个判定。
     *
     * @internal
     * 步进有三种结局：还有一行可读、语句走到末尾、出错。调用方一般只
     * 关心前两种；出错时的结果码留给诊断。
     */
    struct StepOutcome
    {
        std::int32_t code; ///< 库的结果码原值

        /** @brief 还有一行可读。 */
        [[nodiscard]]
        auto has_row() const noexcept -> bool
        {
            return code == SQLITE_ROW;
        }

        /** @brief 语句已走到末尾，没有更多行。 */
        [[nodiscard]]
        auto finished() const noexcept -> bool
        {
            return code == SQLITE_DONE;
        }

        /** @brief 出错：既没有行，也没走到末尾。 */
        [[nodiscard]]
        auto failed() const noexcept -> bool
        {
            return !has_row() && !finished();
        }
    };

    /**
     * @brief 一条原生预编译语句的拥有者：析构时销毁语句。
     *
     * @internal
     * 只能移动。被移动走的对象不持有语句，对它调用步进、绑定、复位一
     * 律按误用处理（结果码是库的误用码，取列得到空值），都不抛。取列
     * 得到的文本视图只在下一次步进、绑定或复位之前有效。语句指针由带删
     * 除器的拥有型指针持有：库的销毁调用只在删除器里，析构、移动赋值与
     * 交出地址都经它，所以移动与析构都不抛。
     */
    class NativeStatement
    {
        /** @brief 预编译语句的删除器；库的销毁调用只在这一处出现。 */
        struct StmtDeleter
        {
            auto operator()(sqlite3_stmt* stmt) const noexcept -> void
            {
                sqlite3_finalize(stmt);
            }
        };

        /** @brief 语句指针的拥有者。 */
        using Owner = cg::util::handle::OwnedPtr<sqlite3_stmt, StmtDeleter>;

    public:
        /** @brief 不持有语句。 */
        NativeStatement() = default;

        NativeStatement(const NativeStatement&) = delete;
        auto operator=(const NativeStatement&) -> NativeStatement& = delete;
        NativeStatement(NativeStatement&& other) noexcept = default;
        auto operator=(NativeStatement&& other) noexcept
            -> NativeStatement& = default;
        ~NativeStatement() = default;

        /**
         * @brief 预编译一条语句。
         *
         * @internal
         * 失败时不抛：返回库给出的错误文本。预编译失败时库可能留下半
         * 成品语句，它在返回之前已经销毁。文本里没有语句（只有空白或
         * 注释）也按失败处理，因为返回的对象必须持有语句。
         *
         * @param db  连接；语句销毁之前它必须一直有效。
         * @param sql 语句文本，到首个零字符为止。
         */
        [[nodiscard]]
        static auto prepare(sqlite3* db, const char* sql)
            -> std::expected<NativeStatement, std::string>;

        /** @brief 是否持有语句。 */
        [[nodiscard]]
        auto valid() const noexcept -> bool;

        /** @brief 步进一次。 */
        [[nodiscard]]
        auto step() noexcept -> StepOutcome;

        /**
         * @brief 读当前行某一列的文本。
         *
         * @internal
         * 列号从 0 起。空值列读出空视图。视图指向库的内存，要久存得自己
         * 拷贝。
         */
        [[nodiscard]]
        auto column_text(int column) const noexcept -> std::string_view;

        /** @brief 读当前行某一列的 64 位整数；空值列读出 0。 */
        [[nodiscard]]
        auto column_int64(int column) const noexcept -> std::int64_t;

        /** @brief 给第 index 个参数绑定一个 64 位整数，返回库的结果码。
         *          参数序号从 1 起。 */
        auto bind_int64(int index, std::int64_t value) noexcept -> int;

        /** @brief 复位语句以便重新执行，返回最近一次执行的结果码。 */
        auto reset() noexcept -> int;

        /** @brief 清除全部参数绑定。 */
        auto clear_bindings() noexcept -> void;

    private:
        /**
         * @brief 交出内部指针的地址，供库把预编译好的语句写进来；先销毁
         *        已持有的语句。
         *
         * @internal
         * 只供 prepare 交给库的出参。
         */
        [[nodiscard]]
        auto release_and_get_address_of() noexcept -> sqlite3_stmt**
        {
            return stmt_.release_and_get_address_of();
        }

        Owner stmt_;
    };

    auto NativeStatement::prepare(sqlite3* db, const char* sql)
        -> std::expected<NativeStatement, std::string>
    {
        // 预编译失败时库可能留下半成品语句：出参直接落进拥有者，由它的
        // 析构销毁。
        NativeStatement owned;
        const int       rc = sqlite3_prepare_v2(
            db,
            sql,
            -1,
            owned.release_and_get_address_of(),
            nullptr);
        if (rc != SQLITE_OK)
        {
            return std::unexpected{std::string{sqlite3_errmsg(db)}};
        }
        if (!owned.valid())
        {
            return std::unexpected{
                std::string{"no SQL statement in the given text"}};
        }
        return owned;
    }

    auto NativeStatement::valid() const noexcept -> bool
    {
        return stmt_.valid();
    }

    auto NativeStatement::step() noexcept -> StepOutcome
    {
        if (!stmt_.valid())
        {
            return StepOutcome{.code = SQLITE_MISUSE};
        }
        return StepOutcome{.code = sqlite3_step(stmt_.get())};
    }

    auto NativeStatement::column_text(int column) const noexcept
        -> std::string_view
    {
        if (!stmt_.valid())
        {
            return {};
        }
        const auto* text = sqlite3_column_text(stmt_.get(), column);
        if (text == nullptr)
        {
            return {};
        }
        return {
            reinterpret_cast<const char*>(text),
            static_cast<std::size_t>(
                sqlite3_column_bytes(stmt_.get(), column))};
    }

    auto NativeStatement::column_int64(int column) const noexcept
        -> std::int64_t
    {
        if (!stmt_.valid())
        {
            return 0;
        }
        return sqlite3_column_int64(stmt_.get(), column);
    }

    auto NativeStatement::bind_int64(int index, std::int64_t value) noexcept
        -> int
    {
        if (!stmt_.valid())
        {
            return SQLITE_MISUSE;
        }
        return sqlite3_bind_int64(stmt_.get(), index, value);
    }

    auto NativeStatement::reset() noexcept -> int
    {
        if (!stmt_.valid())
        {
            return SQLITE_MISUSE;
        }
        return sqlite3_reset(stmt_.get());
    }

    auto NativeStatement::clear_bindings() noexcept -> void
    {
        if (stmt_.valid())
        {
            static_cast<void>(sqlite3_clear_bindings(stmt_.get()));
        }
    }

    /**
     * @brief 执行一段不含参数的语句文本，不读取结果行。
     *
     * @internal
     * 建表语句、切换日志模式一类的整段文本由它执行：不设回调，结果的
     * 行一律丢弃，不显式开事务，每条语句自动提交。失败时不抛，返回库给
     * 出的错误文本；错误文本由库分配，经出参直接落进拥有型对象，拷进
     * 结果后随作用域交还。
     */
    [[nodiscard]]
    inline auto exec_native(sqlite3* db, const char* sql)
        -> std::expected<void, std::string>
    {
        NativeText message;
        const int  rc = sqlite3_exec(
            db,
            sql,
            nullptr,
            nullptr,
            message.release_and_get_address_of());
        if (rc == SQLITE_OK)
        {
            return {};
        }
        return std::unexpected{
            message.valid() ? std::string{message.view()}
                            : std::string{sqlite3_errmsg(db)}};
    }

} // namespace cg::db
