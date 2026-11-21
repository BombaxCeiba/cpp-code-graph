module;
#include "tree_sitter/api.h" // 第三方头文件，仅限全局模块片段

/**
 * @module cg.util.treesit
 *
 * @brief tree-sitter 增量解析运行时（v0.27.1，唯一许可的语法树依赖，仅
 *        经此模块可达）之上的最小 RAII 门面：解析器/树/节点包装、遍历
 *        助手与错误/缺失标记查询。导入方永远不应直接引用 ts_ 原始符号。
 *
 * @internal
 * tree_sitter/api.h 是纯 extern "C" 声明、零 static-inline 助手，与
 * clang-c/Index.h 同形，走"直包"先例（对照 cg.util.json 的经典 TU 桥：
 * 那里要隔离的是 yyjson-cpp 的 C++ 模板类型——MSVC 模块 IFC writer 的
 * C1001 地雷；纯 C 声明不触发）。api.h 只出现在本单元的全局模块片段；
 * TSLanguage 经 export using 重导出（全局模块片段里的名字仅"可达"，
 * 导入方不能按名查找），其余 ts_ 符号对导入方完全不可见。运行时与
 * 官方 Bash 语法库以静态库链接（CPM 钉版配方，见 3rdParty/CMakeLists.txt
 * 的 _cpm_add_treesitter 两函数）。属主类型（Parser/Tree）的特殊成员
 * 函数与 parse 全部类外定义——非 inline 导出函数体只在接口单元编译
 * 一次，函数体内的 ts_ 调用不会经内联回放进入任何导入方的翻译单元；
 * Node 是轻量值包装，方法类内定义，但每个方法体同样只经 ts_ 纯函数
 * 读取自身携带的 POD（TSNode），无状态可泄漏。
 */

export module cg.util.treesit;

import std;

// tree-sitter-bash 静态库（v0.25.1）提供的语言构造函数：生成的
// parser.c 尾部自带 extern "C" 定义。声明留在模块内部（TU 局部），
// 导入方只经 bash_language() 触达它。
extern "C" const TSLanguage* tree_sitter_bash(void);

export {
    using ::TSLanguage;
}

export namespace cg::util::treesit
{

    /**
     * @brief 行列位置（row 零基；column 为字节列，同样零基）。
     *
     * 与 tree-sitter 的 TSPoint 同口径；独立命名以免泄漏 ts_ 名字。
     */
    struct Point
    {
        std::uint32_t row = 0;
        std::uint32_t column = 0;
    };

    class Tree;
    class Parser;

    /**
     * @brief 语法树节点的非拥有值包装。
     *
     * TSNode 本身就是可自由拷贝的 POD 值（内部携带树指针与偏移），因此
     * Node 保持纯值语义：拷贝廉价、无析构副作用。有效性与产出它的 Tree
     * 绑定——Tree 销毁后一切 Node 均悬垂（与指向文档缓冲区的视图同纪律，
     * 调用方负责保活）。默认构造为空节点，一切查询返回安全空值。
     */
    class Node
    {
    public:
        Node() = default;

        /** @brief 是否为空节点（默认构造或来源树已亡）。 */
        [[nodiscard]]
        auto is_null() const noexcept -> bool
        {
            return ts_node_is_null(node_) != 0;
        }

        /**
         * @brief 节点类型名（语法的一类，如 "command"、"heredoc_body"）。
         *
         * @return 指向语法库静态存储的视图；空节点返回空视图。
         */
        [[nodiscard]]
        auto type() const noexcept -> std::string_view
        {
            if (is_null())
            {
                return {};
            }
            const char* name = ts_node_type(node_);
            return name != nullptr ? std::string_view{name}
                                   : std::string_view{};
        }

        /** @brief 是否具名节点（语法树遍历的常规形态；标点为匿名）。 */
        [[nodiscard]]
        auto is_named() const noexcept -> bool
        {
            return !is_null() && ts_node_is_named(node_) != 0;
        }

        /** @brief 自身是否为错误节点（语法无法归类的原文段）。 */
        [[nodiscard]]
        auto is_error() const noexcept -> bool
        {
            return !is_null() && ts_node_is_error(node_) != 0;
        }

        /**
         * @brief 自身是否为缺失标记（错误恢复插入的"本该在这里"节点）。
         *
         * 缺失节点零字节宽（start==end），文本查询恒为空。
         */
        [[nodiscard]]
        auto is_missing() const noexcept -> bool
        {
            return !is_null() && ts_node_is_missing(node_) != 0;
        }

        /**
         * @brief 以本节点为根的子树是否含任何错误（ERROR 节点或缺失标记）。
         *
         * 整棵树健全性的快速闸门：根节点上的一次调用即判定。
         */
        [[nodiscard]]
        auto has_error() const noexcept -> bool
        {
            return !is_null() && ts_node_has_error(node_) != 0;
        }

        /** @brief 直接子节点总数（含匿名：标点与字面量）。 */
        [[nodiscard]]
        auto child_count() const noexcept -> std::uint32_t
        {
            return is_null() ? 0 : ts_node_child_count(node_);
        }

        /** @brief 直接具名子节点数。 */
        [[nodiscard]]
        auto named_child_count() const noexcept -> std::uint32_t
        {
            return is_null() ? 0 : ts_node_named_child_count(node_);
        }

        /**
         * @brief 按位序（零基）取直接子节点。
         * @return 越界或空节点时返回空节点。
         */
        [[nodiscard]]
        auto child(std::uint32_t index) const noexcept -> Node
        {
            if (is_null() || index >= child_count())
            {
                return Node{};
            }
            return Node{ts_node_child(node_, index)};
        }

        /** @brief 按位序（零基）取直接具名子节点；越界返回空节点。 */
        [[nodiscard]]
        auto named_child(std::uint32_t index) const noexcept -> Node
        {
            if (is_null() || index >= named_child_count())
            {
                return Node{};
            }
            return Node{ts_node_named_child(node_, index)};
        }

        /** @brief 父节点；根节点与空节点返回空节点。 */
        [[nodiscard]]
        auto parent() const noexcept -> Node
        {
            if (is_null())
            {
                return Node{};
            }
            return Node{ts_node_parent(node_)};
        }

        /** @brief 节点起始字节偏移（相对解析源串）。 */
        [[nodiscard]]
        auto start_byte() const noexcept -> std::uint32_t
        {
            return is_null() ? 0 : ts_node_start_byte(node_);
        }

        /** @brief 节点结束字节偏移（开区间；缺失节点与起始相等）。 */
        [[nodiscard]]
        auto end_byte() const noexcept -> std::uint32_t
        {
            return is_null() ? 0 : ts_node_end_byte(node_);
        }

        /** @brief 起始行列。 */
        [[nodiscard]]
        auto start_point() const noexcept -> Point
        {
            if (is_null())
            {
                return Point{};
            }
            const TSPoint p = ts_node_start_point(node_);
            return Point{p.row, p.column};
        }

        /**
         * @brief 节点在解析源串上的文本切片。
         *
         * 调用方传入解析时的同一份源串（门面不持有源文本）。源串短于节点
         * 偏移（源串与树不匹配）时按源串长度截断；空节点返回空视图。
         */
        [[nodiscard]]
        auto text(std::string_view source) const noexcept -> std::string_view
        {
            if (is_null())
            {
                return {};
            }
            const std::size_t begin = start_byte();
            const std::size_t end =
                std::min<std::size_t>(end_byte(), source.size());
            if (begin >= end)
            {
                return {};
            }
            return source.substr(begin, end - begin);
        }

        /**
         * @brief 同一树内两个节点是否为同一节点。
         *
         * 按节点身份比较（id）；两个空节点相等，来自不同树的节点即使
         * 偏移相同也不相等。仅比较自身 POD 成员，无 ts_ 调用。
         */
        [[nodiscard]]
        friend auto operator==(Node a, Node b) noexcept -> bool
        {
            return a.node_.id == b.node_.id;
        }

    private:
        friend class Tree;

        explicit Node(TSNode node) noexcept : node_{node} {}

        TSNode node_{};
    };

    /**
     * @brief 解析产物语法树的独占属主（TSTree* 的 RAII 包装）。
     *
     * 不可拷贝（无共享语义的消费场景）；移动后源对象交出树、退化为可安全
     * 析构的空树。产出的 Node 在本对象存活期间有效。
     */
    class Tree
    {
    public:
        Tree() = default;
        Tree(const Tree&) = delete;
        auto operator=(const Tree&) -> Tree& = delete;

        Tree(Tree&& other) noexcept;
        auto operator=(Tree&& other) noexcept -> Tree&;
        ~Tree();

        /** @brief 根节点；空树（含被移走的源对象）返回空节点。 */
        [[nodiscard]]
        auto root_node() const noexcept -> Node
        {
            if (tree_ == nullptr)
            {
                return Node{};
            }
            return Node{ts_tree_root_node(tree_)};
        }

    private:
        friend class Parser;

        explicit Tree(TSTree* tree) noexcept : tree_{tree} {}

        TSTree* tree_ = nullptr;
    };

    /**
     * @brief 解析器（TSParser* 的 RAII 包装）：一种语言一个实例，可反复
     *        解析多份源串。
     *
     * 构造失败（运行时不接受语言的 ABI）抛 std::runtime_error。解析本身
     * 不抛（错误恢复内建于 tree-sitter：畸形输入产出带错误标记的树，而非
     * 失败）；仅当运行时返回空树（分配失败等）抛 std::runtime_error。
     */
    class Parser
    {
    public:
        explicit Parser(const TSLanguage& language);
        Parser(const Parser&) = delete;
        auto operator=(const Parser&) -> Parser& = delete;
        Parser(Parser&& other) noexcept;
        auto operator=(Parser&& other) noexcept -> Parser&;
        ~Parser();

        /**
         * @brief 解析一份完整的 UTF-8 源串（如 shell 命令全文）。
         *
         * 输入按字节+长度喂入（内嵌 NUL 无碍，无需 NUL 结尾）。错误恢复
         * 内建：残缺输入同样产出树，健全性经 root_node().has_error() 与
         * collect_diagnostics 查询。
         */
        [[nodiscard]]
        auto parse(std::string_view source) const -> Tree;

    private:
        TSParser* parser_ = nullptr;
    };

    /**
     * @brief 按位序遍历 node 的全部直接子节点（含匿名）。
     *
     * @param fn 回调 void(Node)；node 为空节点时零次调用。
     */
    template <typename Fn>
    void for_each_child(Node node, Fn&& fn)
    {
        const std::uint32_t count = node.child_count();
        for (std::uint32_t i = 0; i < count; ++i)
        {
            fn(node.child(i));
        }
    }

    /**
     * @brief 先序深度优先遍历以 node 为根的子树（先访问自身，再依次下潜）。
     *
     * @param fn 回调 void(Node)；node 为空节点时零次调用。树的深度即命令
     *        嵌套层数（几十以内），递归实现无栈压力。
     */
    template <typename Fn>
    void for_each_descendant(Node node, Fn&& fn)
    {
        if (node.is_null())
        {
            return;
        }
        fn(node);
        const std::uint32_t count = node.child_count();
        for (std::uint32_t i = 0; i < count; ++i)
        {
            for_each_descendant(node.child(i), fn);
        }
    }

    /** @brief 诊断条目种类：错误节点（无法归类的原文段）或缺失标记。 */
    enum class DiagKind : std::uint8_t
    {
        ErrorNode,    ///< ERROR 节点：原文存在但语法无法归类
        MissingToken, ///< 缺失标记：语法恢复插入的零宽"应有"节点
    };

    /** @brief 一条语法诊断：节点（带其位置与类型）加种类。 */
    struct Diag
    {
        Node     node;
        DiagKind kind = DiagKind::ErrorNode;
    };

    /**
     * @brief 收集整棵树的错误节点与缺失标记（先序）。
     *
     * 根节点健全时直接返回空（树级 has_error 快速闸门，不产生遍历开销）。
     * 提取侧的"宁漏勿猜"硬线以此为闸：诊断非空即对残缺输入整体放弃。
     */
    [[nodiscard]]
    auto collect_diagnostics(const Tree& tree) -> std::vector<Diag>;

    /**
     * @brief 链接进来的官方 Bash 语法（tree-sitter-bash v0.25.1）。
     *
     * 静态库提供的唯一语言入口；返回引用的存活期是整个进程（语法表在
     * 静态存储里），与解析器、树的存活期无关。
     */
    [[nodiscard]]
    auto bash_language() -> const TSLanguage&;

    Tree::~Tree()
    {
        if (tree_ != nullptr)
        {
            ts_tree_delete(tree_);
        }
    }

    Tree::Tree(Tree&& other) noexcept : tree_{other.tree_}
    {
        other.tree_ = nullptr;
    }

    auto Tree::operator=(Tree&& other) noexcept -> Tree&
    {
        if (this != &other)
        {
            if (tree_ != nullptr)
            {
                ts_tree_delete(tree_);
            }
            tree_ = other.tree_;
            other.tree_ = nullptr;
        }
        return *this;
    }

    Parser::Parser(const TSLanguage& language)
    {
        parser_ = ts_parser_new();
        if (parser_ == nullptr)
        {
            throw std::runtime_error{
                "cg::util::treesit::Parser: runtime rejected allocation"};
        }
        if (ts_parser_set_language(parser_, &language) == 0)
        {
            ts_parser_delete(parser_);
            parser_ = nullptr;
            throw std::runtime_error{
                "cg::util::treesit::Parser: language ABI rejected by runtime "
                "(grammar/parser version mismatch)"};
        }
    }

    Parser::Parser(Parser&& other) noexcept : parser_{other.parser_}
    {
        other.parser_ = nullptr;
    }

    auto Parser::operator=(Parser&& other) noexcept -> Parser&
    {
        if (this != &other)
        {
            if (parser_ != nullptr)
            {
                ts_parser_delete(parser_);
            }
            parser_ = other.parser_;
            other.parser_ = nullptr;
        }
        return *this;
    }

    Parser::~Parser()
    {
        if (parser_ != nullptr)
        {
            ts_parser_delete(parser_);
        }
    }

    auto Parser::parse(std::string_view source) const -> Tree
    {
        // 空串喂一个字面 ""（data() 对空串可能返回 nullptr，而运行时的
        // 输入契约要求有效指针；长度为零时内容不再被读）。
        const char* data = source.empty() ? "" : source.data();
        TSTree*     raw = ts_parser_parse_string(
            parser_,
            nullptr,
            data,
            static_cast<std::uint32_t>(source.size()));
        if (raw == nullptr)
        {
            throw std::runtime_error{
                "cg::util::treesit::Parser::parse: runtime returned no tree"};
        }
        return Tree{raw};
    }

    auto collect_diagnostics(const Tree& tree) -> std::vector<Diag>
    {
        std::vector<Diag> out;
        const Node        root = tree.root_node();
        if (root.is_null() || !root.has_error())
        {
            return out;
        }
        for_each_descendant(
            root,
            [&out](Node node)
            {
                if (node.is_error())
                {
                    out.push_back(Diag{node, DiagKind::ErrorNode});
                }
                else if (node.is_missing())
                {
                    out.push_back(Diag{node, DiagKind::MissingToken});
                }
            });
        return out;
    }

    auto bash_language() -> const TSLanguage& { return *tree_sitter_bash(); }

} // namespace cg::util::treesit
