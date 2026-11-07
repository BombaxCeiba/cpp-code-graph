// Bash/PowerShell 结构化查找的用例矩阵（Bash 62 条 + PowerShell 40
// 条，加 X 系列自造补充用例）：每条给出命令样例与期望的查找请求。
// 规则层行为与本矩阵逐条对照，补用例直接在此追加并同步核对口径。
#include <string>
#include <string_view>
#include <vector>

namespace cg::test::shellmatrix {

struct ExpectRequest {
    std::string_view engine;
    std::vector<std::string> patterns;
    bool single = true;
    bool fixed = false;
    std::vector<std::string> paths;
    std::vector<std::string> globs;
    std::vector<std::string> types;
    std::string_view path_kind;
    std::string_view cwd_hint;
};

struct MatrixCase {
    std::string_view id;
    std::string_view cmd;
    std::vector<ExpectRequest> requests;
    std::string_view skip;
    std::string_view skip_all;
    bool needs_engine = false;
};

inline const MatrixCase bash_cases[] = {
    MatrixCase{
    .id = "B01",
    .cmd = "rg foo",
    .requests = {
        ExpectRequest{.engine = "rg", .patterns = {"foo"}, .single = true, .fixed = false, .paths = {}, .globs = {}, .types = {}, .path_kind = "none", .cwd_hint = ""}
    },
    .skip = "",
    .skip_all = "",
    .needs_engine = false,
},
    MatrixCase{
    .id = "B02",
    .cmd = "rg 'foo'",
    .requests = {
        ExpectRequest{.engine = "rg", .patterns = {"foo"}, .single = true, .fixed = false, .paths = {}, .globs = {}, .types = {}, .path_kind = "none", .cwd_hint = ""}
    },
    .skip = "",
    .skip_all = "",
    .needs_engine = false,
},
    MatrixCase{
    .id = "B03",
    .cmd = "rg \"foo\"",
    .requests = {
        ExpectRequest{.engine = "rg", .patterns = {"foo"}, .single = true, .fixed = false, .paths = {}, .globs = {}, .types = {}, .path_kind = "none", .cwd_hint = ""}
    },
    .skip = "",
    .skip_all = "",
    .needs_engine = false,
},
    MatrixCase{
    .id = "B04",
    .cmd = "rg -e foo",
    .requests = {
        ExpectRequest{.engine = "rg", .patterns = {"foo"}, .single = true, .fixed = false, .paths = {}, .globs = {}, .types = {}, .path_kind = "none", .cwd_hint = ""}
    },
    .skip = "",
    .skip_all = "",
    .needs_engine = false,
},
    MatrixCase{
    .id = "B05",
    .cmd = "rg --regexp=foo",
    .requests = {
        ExpectRequest{.engine = "rg", .patterns = {"foo"}, .single = true, .fixed = false, .paths = {}, .globs = {}, .types = {}, .path_kind = "none", .cwd_hint = ""}
    },
    .skip = "",
    .skip_all = "",
    .needs_engine = false,
},
    MatrixCase{
    .id = "B06",
    .cmd = "rg --regexp foo",
    .requests = {
        ExpectRequest{.engine = "rg", .patterns = {"foo"}, .single = true, .fixed = false, .paths = {}, .globs = {}, .types = {}, .path_kind = "none", .cwd_hint = ""}
    },
    .skip = "",
    .skip_all = "",
    .needs_engine = false,
},
    MatrixCase{
    .id = "B07",
    .cmd = "rg -F 'a.b'",
    .requests = {
        ExpectRequest{.engine = "rg", .patterns = {"a.b"}, .single = true, .fixed = true, .paths = {}, .globs = {}, .types = {}, .path_kind = "none", .cwd_hint = ""}
    },
    .skip = "",
    .skip_all = "",
    .needs_engine = false,
},
    MatrixCase{
    .id = "B08",
    .cmd = "rg --fixed-strings a.b",
    .requests = {
        ExpectRequest{.engine = "rg", .patterns = {"a.b"}, .single = true, .fixed = true, .paths = {}, .globs = {}, .types = {}, .path_kind = "none", .cwd_hint = ""}
    },
    .skip = "",
    .skip_all = "",
    .needs_engine = false,
},
    MatrixCase{
    .id = "B09",
    .cmd = "rg foo src/",
    .requests = {
        ExpectRequest{.engine = "rg", .patterns = {"foo"}, .single = true, .fixed = false, .paths = {"src/"}, .globs = {}, .types = {}, .path_kind = "none", .cwd_hint = ""}
    },
    .skip = "",
    .skip_all = "",
    .needs_engine = false,
},
    MatrixCase{
    .id = "B10",
    .cmd = "rg foo src include",
    .requests = {
        ExpectRequest{.engine = "rg", .patterns = {"foo"}, .single = true, .fixed = false, .paths = {"src", "include"}, .globs = {}, .types = {}, .path_kind = "none", .cwd_hint = ""}
    },
    .skip = "",
    .skip_all = "",
    .needs_engine = false,
},
    MatrixCase{
    .id = "B11",
    .cmd = "rg -i foo",
    .requests = {
        ExpectRequest{.engine = "rg", .patterns = {"foo"}, .single = true, .fixed = false, .paths = {}, .globs = {}, .types = {}, .path_kind = "none", .cwd_hint = ""}
    },
    .skip = "",
    .skip_all = "",
    .needs_engine = false,
},
    MatrixCase{
    .id = "B12",
    .cmd = "rg -inH foo",
    .requests = {
        ExpectRequest{.engine = "rg", .patterns = {"foo"}, .single = true, .fixed = false, .paths = {}, .globs = {}, .types = {}, .path_kind = "none", .cwd_hint = ""}
    },
    .skip = "",
    .skip_all = "",
    .needs_engine = false,
},
    MatrixCase{
    .id = "B13",
    .cmd = "rg 'foo|bar'",
    .requests = {
        ExpectRequest{.engine = "rg", .patterns = {"foo|bar"}, .single = true, .fixed = false, .paths = {}, .globs = {}, .types = {}, .path_kind = "none", .cwd_hint = ""}
    },
    .skip = "",
    .skip_all = "",
    .needs_engine = false,
},
    MatrixCase{
    .id = "B14",
    .cmd = "rg -t cpp foo",
    .requests = {
        ExpectRequest{.engine = "rg", .patterns = {"foo"}, .single = true, .fixed = false, .paths = {}, .globs = {}, .types = {"cpp"}, .path_kind = "none", .cwd_hint = ""}
    },
    .skip = "",
    .skip_all = "",
    .needs_engine = false,
},
    MatrixCase{
    .id = "B15",
    .cmd = "rg -g '*.cpp' foo",
    .requests = {
        ExpectRequest{.engine = "rg", .patterns = {"foo"}, .single = true, .fixed = false, .paths = {}, .globs = {"*.cpp"}, .types = {}, .path_kind = "none", .cwd_hint = ""}
    },
    .skip = "",
    .skip_all = "",
    .needs_engine = false,
},
    MatrixCase{
    .id = "B16",
    .cmd = "rg --glob=*.hpp foo",
    .requests = {
        ExpectRequest{.engine = "rg", .patterns = {"foo"}, .single = true, .fixed = false, .paths = {}, .globs = {"*.hpp"}, .types = {}, .path_kind = "none", .cwd_hint = ""}
    },
    .skip = "",
    .skip_all = "",
    .needs_engine = false,
},
    MatrixCase{
    .id = "B17",
    .cmd = "rg --files-with-matches foo",
    .requests = {
        ExpectRequest{.engine = "rg", .patterns = {"foo"}, .single = true, .fixed = false, .paths = {}, .globs = {}, .types = {}, .path_kind = "none", .cwd_hint = ""}
    },
    .skip = "",
    .skip_all = "",
    .needs_engine = false,
},
    MatrixCase{
    .id = "B18",
    .cmd = "cd sub && rg foo",
    .requests = {
        ExpectRequest{.engine = "rg", .patterns = {"foo"}, .single = true, .fixed = false, .paths = {}, .globs = {}, .types = {}, .path_kind = "after-cd", .cwd_hint = "sub"}
    },
    .skip = "",
    .skip_all = "",
    .needs_engine = false,
},
    MatrixCase{
    .id = "B19",
    .cmd = "rg foo | head -5",
    .requests = {
        ExpectRequest{.engine = "rg", .patterns = {"foo"}, .single = true, .fixed = false, .paths = {}, .globs = {}, .types = {}, .path_kind = "none", .cwd_hint = ""}
    },
    .skip = "",
    .skip_all = "",
    .needs_engine = false,
},
    MatrixCase{
    .id = "B20",
    .cmd = "cat x.cpp | rg foo",
    .requests = {
        ExpectRequest{.engine = "rg", .patterns = {"foo"}, .single = true, .fixed = false, .paths = {}, .globs = {}, .types = {}, .path_kind = "stdin-pipe", .cwd_hint = ""}
    },
    .skip = "",
    .skip_all = "",
    .needs_engine = false,
},
    MatrixCase{
    .id = "B21",
    .cmd = "ls; rg foo; ls",
    .requests = {
        ExpectRequest{.engine = "rg", .patterns = {"foo"}, .single = true, .fixed = false, .paths = {}, .globs = {}, .types = {}, .path_kind = "none", .cwd_hint = ""}
    },
    .skip = "",
    .skip_all = "",
    .needs_engine = false,
},
    MatrixCase{
    .id = "B22",
    .cmd = "rg foo || echo notfound",
    .requests = {
        ExpectRequest{.engine = "rg", .patterns = {"foo"}, .single = true, .fixed = false, .paths = {}, .globs = {}, .types = {}, .path_kind = "none", .cwd_hint = ""}
    },
    .skip = "",
    .skip_all = "",
    .needs_engine = false,
},
    MatrixCase{
    .id = "B23",
    .cmd = "FOO=1 rg foo",
    .requests = {
        ExpectRequest{.engine = "rg", .patterns = {"foo"}, .single = true, .fixed = false, .paths = {}, .globs = {}, .types = {}, .path_kind = "none", .cwd_hint = ""}
    },
    .skip = "",
    .skip_all = "",
    .needs_engine = false,
},
    MatrixCase{
    .id = "B24",
    .cmd = "env rg foo",
    .requests = {
        ExpectRequest{.engine = "rg", .patterns = {"foo"}, .single = true, .fixed = false, .paths = {}, .globs = {}, .types = {}, .path_kind = "none", .cwd_hint = ""}
    },
    .skip = "",
    .skip_all = "",
    .needs_engine = false,
},
    MatrixCase{
    .id = "B25",
    .cmd = "command rg foo",
    .requests = {
        ExpectRequest{.engine = "rg", .patterns = {"foo"}, .single = true, .fixed = false, .paths = {}, .globs = {}, .types = {}, .path_kind = "none", .cwd_hint = ""}
    },
    .skip = "",
    .skip_all = "",
    .needs_engine = false,
},
    MatrixCase{
    .id = "B26",
    .cmd = "rg -e foo -e bar",
    .requests = {
        ExpectRequest{.engine = "rg", .patterns = {"foo", "bar"}, .single = false, .fixed = false, .paths = {}, .globs = {}, .types = {}, .path_kind = "none", .cwd_hint = ""}
    },
    .skip = "",
    .skip_all = "",
    .needs_engine = false,
},
    MatrixCase{
    .id = "B27",
    .cmd = "rg \"\\bdraw\\b\"",
    .requests = {
        ExpectRequest{.engine = "rg", .patterns = {"\\bdraw\\b"}, .single = true, .fixed = false, .paths = {}, .globs = {}, .types = {}, .path_kind = "none", .cwd_hint = ""}
    },
    .skip = "",
    .skip_all = "",
    .needs_engine = false,
},
    MatrixCase{
    .id = "B28",
    .cmd = "rg 'foo bar'",
    .requests = {
        ExpectRequest{.engine = "rg", .patterns = {"foo bar"}, .single = true, .fixed = false, .paths = {}, .globs = {}, .types = {}, .path_kind = "none", .cwd_hint = ""}
    },
    .skip = "",
    .skip_all = "",
    .needs_engine = false,
},
    MatrixCase{
    .id = "B29",
    .cmd = "rg -- foo -x",
    .requests = {
        ExpectRequest{.engine = "rg", .patterns = {"foo"}, .single = true, .fixed = false, .paths = {"-x"}, .globs = {}, .types = {}, .path_kind = "none", .cwd_hint = ""}
    },
    .skip = "",
    .skip_all = "",
    .needs_engine = false,
},
    MatrixCase{
    .id = "B30",
    .cmd = "rg -C 3 foo",
    .requests = {
        ExpectRequest{.engine = "rg", .patterns = {"foo"}, .single = true, .fixed = false, .paths = {}, .globs = {}, .types = {}, .path_kind = "none", .cwd_hint = ""}
    },
    .skip = "",
    .skip_all = "",
    .needs_engine = false,
},
    MatrixCase{
    .id = "B31",
    .cmd = "rg --count foo",
    .requests = {
        ExpectRequest{.engine = "rg", .patterns = {"foo"}, .single = true, .fixed = false, .paths = {}, .globs = {}, .types = {}, .path_kind = "none", .cwd_hint = ""}
    },
    .skip = "",
    .skip_all = "",
    .needs_engine = false,
},
    MatrixCase{
    .id = "B32",
    .cmd = "rg -f pats.txt",
    .requests = {
        
    },
    .skip = "pattern-from-file",
    .skip_all = "",
    .needs_engine = false,
},
    MatrixCase{
    .id = "B33",
    .cmd = "rg $pat",
    .requests = {
        
    },
    .skip = "dynamic-argument",
    .skip_all = "",
    .needs_engine = false,
},
    MatrixCase{
    .id = "B34",
    .cmd = "rg \"$pat\"",
    .requests = {
        
    },
    .skip = "dynamic-argument",
    .skip_all = "",
    .needs_engine = false,
},
    MatrixCase{
    .id = "B35",
    .cmd = "rg `echo foo`",
    .requests = {
        
    },
    .skip = "dynamic-argument",
    .skip_all = "",
    .needs_engine = false,
},
    MatrixCase{
    .id = "B36",
    .cmd = "rg 'foo'$(x)",
    .requests = {
        
    },
    .skip = "dynamic-argument",
    .skip_all = "",
    .needs_engine = false,
},
    MatrixCase{
    .id = "B37",
    .cmd = "echo rg foo",
    .requests = {
        
    },
    .skip = "",
    .skip_all = "",
    .needs_engine = false,
},
    MatrixCase{
    .id = "B38",
    .cmd = "rg() { grep -n \"$1\"; }; rg foo",
    .requests = {
        
    },
    .skip = "",
    .skip_all = "alias-shadow",
    .needs_engine = false,
},
    MatrixCase{
    .id = "B39",
    .cmd = "alias rg='rg -i'; rg foo",
    .requests = {
        
    },
    .skip = "",
    .skip_all = "alias-shadow",
    .needs_engine = false,
},
    MatrixCase{
    .id = "B40",
    .cmd = "xargs rg foo",
    .requests = {
        
    },
    .skip = "",
    .skip_all = "",
    .needs_engine = false,
},
    MatrixCase{
    .id = "B41",
    .cmd = "rg",
    .requests = {
        
    },
    .skip = "no-pattern",
    .skip_all = "",
    .needs_engine = false,
},
    MatrixCase{
    .id = "B42",
    .cmd = "rg src/",
    .requests = {
        ExpectRequest{.engine = "rg", .patterns = {"src/"}, .single = true, .fixed = false, .paths = {}, .globs = {}, .types = {}, .path_kind = "none", .cwd_hint = ""}
    },
    .skip = "",
    .skip_all = "",
    .needs_engine = false,
},
    MatrixCase{
    .id = "B43",
    .cmd = "rg --files",
    .requests = {
        
    },
    .skip = "files-mode",
    .skip_all = "",
    .needs_engine = false,
},
    MatrixCase{
    .id = "B44",
    .cmd = "rg -v foo",
    .requests = {
        
    },
    .skip = "invert",
    .skip_all = "",
    .needs_engine = false,
},
    MatrixCase{
    .id = "B45",
    .cmd = "rg --invert-match foo",
    .requests = {
        
    },
    .skip = "invert",
    .skip_all = "",
    .needs_engine = false,
},
    MatrixCase{
    .id = "B46",
    .cmd = "rg 'AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
    .requests = {
        
    },
    .skip = "unterminated-quote",
    .skip_all = "",
    .needs_engine = false,
},
    MatrixCase{
    .id = "B47",
    .cmd = "rg \"$(date)\"",
    .requests = {
        
    },
    .skip = "dynamic-argument",
    .skip_all = "",
    .needs_engine = false,
},
    MatrixCase{
    .id = "B48",
    .cmd = "rg -e '(foo'",
    .requests = {
        ExpectRequest{.engine = "rg", .patterns = {"(foo"}, .single = true, .fixed = false, .paths = {}, .globs = {}, .types = {}, .path_kind = "none", .cwd_hint = ""}
    },
    .skip = "",
    .skip_all = "",
    .needs_engine = false,
},
    MatrixCase{
    .id = "B49",
    .cmd = "rg foo \\\nsrc/",
    .requests = {
        ExpectRequest{.engine = "rg", .patterns = {"foo"}, .single = true, .fixed = false, .paths = {"src/"}, .globs = {}, .types = {}, .path_kind = "none", .cwd_hint = ""}
    },
    .skip = "",
    .skip_all = "",
    .needs_engine = false,
},
    MatrixCase{
    .id = "B50",
    .cmd = "rg foo src/; rg bar include/",
    .requests = {
        ExpectRequest{.engine = "rg", .patterns = {"foo"}, .single = true, .fixed = false, .paths = {"src/"}, .globs = {}, .types = {}, .path_kind = "none", .cwd_hint = ""},
        ExpectRequest{.engine = "rg", .patterns = {"bar"}, .single = true, .fixed = false, .paths = {"include/"}, .globs = {}, .types = {}, .path_kind = "none", .cwd_hint = ""}
    },
    .skip = "",
    .skip_all = "",
    .needs_engine = false,
},
    MatrixCase{
    .id = "B51",
    .cmd = "rg -P '(?=x)'",
    .requests = {
        
    },
    .skip = "regex-dialect",
    .skip_all = "",
    .needs_engine = false,
},
    MatrixCase{
    .id = "B52",
    .cmd = "rg -uu foo",
    .requests = {
        ExpectRequest{.engine = "rg", .patterns = {"foo"}, .single = true, .fixed = false, .paths = {}, .globs = {}, .types = {}, .path_kind = "none", .cwd_hint = ""}
    },
    .skip = "",
    .skip_all = "",
    .needs_engine = false,
},
    MatrixCase{
    .id = "B53",
    .cmd = "rg --max-count 3 foo",
    .requests = {
        ExpectRequest{.engine = "rg", .patterns = {"foo"}, .single = true, .fixed = false, .paths = {}, .globs = {}, .types = {}, .path_kind = "none", .cwd_hint = ""}
    },
    .skip = "",
    .skip_all = "",
    .needs_engine = false,
},
    MatrixCase{
    .id = "B54",
    .cmd = "rg -X foo",
    .requests = {
        
    },
    .skip = "unknown-flag",
    .skip_all = "",
    .needs_engine = false,
},
    MatrixCase{
    .id = "B55",
    .cmd = "rg --no-such-flag foo",
    .requests = {
        
    },
    .skip = "unknown-flag",
    .skip_all = "",
    .needs_engine = false,
},
    MatrixCase{
    .id = "B56",
    .cmd = "rg --max-filesize=1M foo",
    .requests = {
        ExpectRequest{.engine = "rg", .patterns = {"foo"}, .single = true, .fixed = false, .paths = {}, .globs = {}, .types = {}, .path_kind = "none", .cwd_hint = ""}
    },
    .skip = "",
    .skip_all = "",
    .needs_engine = false,
},
    MatrixCase{
    .id = "B57",
    .cmd = "(rg foo)",
    .requests = {
        ExpectRequest{.engine = "rg", .patterns = {"foo"}, .single = true, .fixed = false, .paths = {}, .globs = {}, .types = {}, .path_kind = "none", .cwd_hint = ""}
    },
    .skip = "",
    .skip_all = "",
    .needs_engine = false,
},
    MatrixCase{
    .id = "B58",
    .cmd = "rg foo > out.txt",
    .requests = {
        ExpectRequest{.engine = "rg", .patterns = {"foo"}, .single = true, .fixed = false, .paths = {}, .globs = {}, .types = {}, .path_kind = "none", .cwd_hint = ""}
    },
    .skip = "",
    .skip_all = "",
    .needs_engine = false,
},
    MatrixCase{
    .id = "B59",
    .cmd = "rg foo 2>/dev/null",
    .requests = {
        ExpectRequest{.engine = "rg", .patterns = {"foo"}, .single = true, .fixed = false, .paths = {}, .globs = {}, .types = {}, .path_kind = "none", .cwd_hint = ""}
    },
    .skip = "",
    .skip_all = "",
    .needs_engine = false,
},
    MatrixCase{
    .id = "B60",
    .cmd = "echo done | rg -q foo",
    .requests = {
        ExpectRequest{.engine = "rg", .patterns = {"foo"}, .single = true, .fixed = false, .paths = {}, .globs = {}, .types = {}, .path_kind = "stdin-pipe", .cwd_hint = ""}
    },
    .skip = "",
    .skip_all = "",
    .needs_engine = false,
},
    MatrixCase{
    .id = "B61",
    .cmd = "git grep foo",
    .requests = {
        
    },
    .skip = "",
    .skip_all = "",
    .needs_engine = false,
},
    MatrixCase{
    .id = "B62",
    .cmd = "x=rg; $x foo",
    .requests = {
        
    },
    .skip = "",
    .skip_all = "",
    .needs_engine = false,
},
    MatrixCase{
    .id = "X01",
    .cmd = "cat <<EOF\nrg evil_pattern\nEOF\nrg real",
    .requests = {
        ExpectRequest{.engine = "rg", .patterns = {"real"}, .single = true, .fixed = false, .paths = {}, .globs = {}, .types = {}, .path_kind = "none", .cwd_hint = ""}
    },
    .skip = "",
    .skip_all = "",
    .needs_engine = false,
},
    MatrixCase{
    .id = "X02",
    .cmd = "rg foo -e bar",
    .requests = {
        ExpectRequest{.engine = "rg", .patterns = {"bar"}, .single = true, .fixed = false, .paths = {"foo"}, .globs = {}, .types = {}, .path_kind = "none", .cwd_hint = ""}
    },
    .skip = "",
    .skip_all = "",
    .needs_engine = false,
},
    MatrixCase{
    .id = "X03",
    .cmd = "powershell -Command \"sls foo\"",
    .requests = {
        ExpectRequest{.engine = "select-string", .patterns = {"foo"}, .single = true, .fixed = false, .paths = {}, .globs = {}, .types = {}, .path_kind = "none", .cwd_hint = ""}
    },
    .skip = "",
    .skip_all = "",
    .needs_engine = true,
},
    MatrixCase{
    .id = "X04",
    .cmd = "echo ok # trailing \"quote\" then rg fake",
    .requests = {
        
    },
    .skip = "",
    .skip_all = "",
    .needs_engine = false,
},
};


inline const MatrixCase ps_cases[] = {
    MatrixCase{
    .id = "P01",
    .cmd = "Select-String -Pattern 'foo'",
    .requests = {
        ExpectRequest{.engine = "select-string", .patterns = {"foo"}, .single = true, .fixed = false, .paths = {}, .globs = {}, .types = {}, .path_kind = "none", .cwd_hint = ""}
    },
    .skip = "",
    .skip_all = "",
    .needs_engine = true,
},
    MatrixCase{
    .id = "P02",
    .cmd = "Select-String foo",
    .requests = {
        ExpectRequest{.engine = "select-string", .patterns = {"foo"}, .single = true, .fixed = false, .paths = {}, .globs = {}, .types = {}, .path_kind = "none", .cwd_hint = ""}
    },
    .skip = "",
    .skip_all = "",
    .needs_engine = true,
},
    MatrixCase{
    .id = "P03",
    .cmd = "sls foo",
    .requests = {
        ExpectRequest{.engine = "select-string", .patterns = {"foo"}, .single = true, .fixed = false, .paths = {}, .globs = {}, .types = {}, .path_kind = "none", .cwd_hint = ""}
    },
    .skip = "",
    .skip_all = "",
    .needs_engine = true,
},
    MatrixCase{
    .id = "P04",
    .cmd = "Select-String -Pattern foo -Path *.cpp",
    .requests = {
        
    },
    .skip = "wildcard-path",
    .skip_all = "",
    .needs_engine = false,
},
    MatrixCase{
    .id = "P05",
    .cmd = "Get-ChildItem -Recurse | Select-String foo",
    .requests = {
        ExpectRequest{.engine = "select-string", .patterns = {"foo"}, .single = true, .fixed = false, .paths = {}, .globs = {}, .types = {}, .path_kind = "stdin-pipe", .cwd_hint = ""}
    },
    .skip = "",
    .skip_all = "",
    .needs_engine = true,
},
    MatrixCase{
    .id = "P06",
    .cmd = "Select-String -Pattern a,b",
    .requests = {
        ExpectRequest{.engine = "select-string", .patterns = {"a", "b"}, .single = false, .fixed = false, .paths = {}, .globs = {}, .types = {}, .path_kind = "none", .cwd_hint = ""}
    },
    .skip = "",
    .skip_all = "",
    .needs_engine = true,
},
    MatrixCase{
    .id = "P07",
    .cmd = "Select-String -SimpleMatch 'a.b'",
    .requests = {
        ExpectRequest{.engine = "select-string", .patterns = {"a.b"}, .single = true, .fixed = true, .paths = {}, .globs = {}, .types = {}, .path_kind = "none", .cwd_hint = ""}
    },
    .skip = "",
    .skip_all = "",
    .needs_engine = true,
},
    MatrixCase{
    .id = "P08",
    .cmd = "Select-String -CaseSensitive foo",
    .requests = {
        ExpectRequest{.engine = "select-string", .patterns = {"foo"}, .single = true, .fixed = false, .paths = {}, .globs = {}, .types = {}, .path_kind = "none", .cwd_hint = ""}
    },
    .skip = "",
    .skip_all = "",
    .needs_engine = true,
},
    MatrixCase{
    .id = "P09",
    .cmd = "Select-String -NotMatch foo",
    .requests = {
        
    },
    .skip = "invert",
    .skip_all = "",
    .needs_engine = false,
},
    MatrixCase{
    .id = "P10",
    .cmd = "Select-String -Pattern $var",
    .requests = {
        
    },
    .skip = "dynamic-argument",
    .skip_all = "",
    .needs_engine = false,
},
    MatrixCase{
    .id = "P11",
    .cmd = "Select-String ($x)",
    .requests = {
        
    },
    .skip = "dynamic-argument",
    .skip_all = "",
    .needs_engine = false,
},
    MatrixCase{
    .id = "P12",
    .cmd = "gci -r | sls -Pattern 'foo'",
    .requests = {
        ExpectRequest{.engine = "select-string", .patterns = {"foo"}, .single = true, .fixed = false, .paths = {}, .globs = {}, .types = {}, .path_kind = "stdin-pipe", .cwd_hint = ""}
    },
    .skip = "",
    .skip_all = "",
    .needs_engine = true,
},
    MatrixCase{
    .id = "P13",
    .cmd = "\"text\" | Select-String foo",
    .requests = {
        ExpectRequest{.engine = "select-string", .patterns = {"foo"}, .single = true, .fixed = false, .paths = {}, .globs = {}, .types = {}, .path_kind = "stdin-pipe", .cwd_hint = ""}
    },
    .skip = "",
    .skip_all = "",
    .needs_engine = true,
},
    MatrixCase{
    .id = "P14",
    .cmd = "Select-String -Pattern 'foo' -List",
    .requests = {
        ExpectRequest{.engine = "select-string", .patterns = {"foo"}, .single = true, .fixed = false, .paths = {}, .globs = {}, .types = {}, .path_kind = "none", .cwd_hint = ""}
    },
    .skip = "",
    .skip_all = "",
    .needs_engine = true,
},
    MatrixCase{
    .id = "P15",
    .cmd = "Write-Host 'Select-String foo'",
    .requests = {
        
    },
    .skip = "",
    .skip_all = "",
    .needs_engine = false,
},
    MatrixCase{
    .id = "P16",
    .cmd = "Select-String -Pattern (Get-Date)",
    .requests = {
        
    },
    .skip = "dynamic-argument",
    .skip_all = "",
    .needs_engine = false,
},
    MatrixCase{
    .id = "P17",
    .cmd = "Select-String -Path a.cpp,b.cpp -Pattern foo",
    .requests = {
        ExpectRequest{.engine = "select-string", .patterns = {"foo"}, .single = true, .fixed = false, .paths = {"a.cpp", "b.cpp"}, .globs = {}, .types = {}, .path_kind = "none", .cwd_hint = ""}
    },
    .skip = "",
    .skip_all = "",
    .needs_engine = true,
},
    MatrixCase{
    .id = "P18",
    .cmd = "Select-String -Pattern 'a|b'",
    .requests = {
        ExpectRequest{.engine = "select-string", .patterns = {"a|b"}, .single = true, .fixed = false, .paths = {}, .globs = {}, .types = {}, .path_kind = "none", .cwd_hint = ""}
    },
    .skip = "",
    .skip_all = "",
    .needs_engine = true,
},
    MatrixCase{
    .id = "P19",
    .cmd = "$r = Select-String foo",
    .requests = {
        ExpectRequest{.engine = "select-string", .patterns = {"foo"}, .single = true, .fixed = false, .paths = {}, .globs = {}, .types = {}, .path_kind = "none", .cwd_hint = ""}
    },
    .skip = "",
    .skip_all = "",
    .needs_engine = true,
},
    MatrixCase{
    .id = "P20",
    .cmd = "if ($true) { Select-String foo }",
    .requests = {
        ExpectRequest{.engine = "select-string", .patterns = {"foo"}, .single = true, .fixed = false, .paths = {}, .globs = {}, .types = {}, .path_kind = "none", .cwd_hint = ""}
    },
    .skip = "",
    .skip_all = "",
    .needs_engine = true,
},
    MatrixCase{
    .id = "P21",
    .cmd = "Select-String -Pattern \"foo`ny\"",
    .requests = {
        
    },
    .skip = "control-char",
    .skip_all = "",
    .needs_engine = false,
},
    MatrixCase{
    .id = "P22",
    .cmd = "Select-String -Pattern 'foo",
    .requests = {
        
    },
    .skip = "unterminated-quote",
    .skip_all = "",
    .needs_engine = false,
},
    MatrixCase{
    .id = "P23",
    .cmd = "# Select-String foo",
    .requests = {
        
    },
    .skip = "",
    .skip_all = "",
    .needs_engine = false,
},
    MatrixCase{
    .id = "P24",
    .cmd = "Select-String -Pattern foo -Exclude *.txt",
    .requests = {
        ExpectRequest{.engine = "select-string", .patterns = {"foo"}, .single = true, .fixed = false, .paths = {}, .globs = {}, .types = {}, .path_kind = "none", .cwd_hint = ""}
    },
    .skip = "",
    .skip_all = "",
    .needs_engine = true,
},
    MatrixCase{
    .id = "P25",
    .cmd = "Select-String foo,bar",
    .requests = {
        ExpectRequest{.engine = "select-string", .patterns = {"foo", "bar"}, .single = false, .fixed = false, .paths = {}, .globs = {}, .types = {}, .path_kind = "none", .cwd_hint = ""}
    },
    .skip = "",
    .skip_all = "",
    .needs_engine = true,
},
    MatrixCase{
    .id = "P26",
    .cmd = "Select-String -Quiet foo",
    .requests = {
        ExpectRequest{.engine = "select-string", .patterns = {"foo"}, .single = true, .fixed = false, .paths = {}, .globs = {}, .types = {}, .path_kind = "none", .cwd_hint = ""}
    },
    .skip = "",
    .skip_all = "",
    .needs_engine = true,
},
    MatrixCase{
    .id = "P27",
    .cmd = "Select-String -Pattern 'foo','bar'",
    .requests = {
        ExpectRequest{.engine = "select-string", .patterns = {"foo", "bar"}, .single = false, .fixed = false, .paths = {}, .globs = {}, .types = {}, .path_kind = "none", .cwd_hint = ""}
    },
    .skip = "",
    .skip_all = "",
    .needs_engine = true,
},
    MatrixCase{
    .id = "P28",
    .cmd = "Select-String -NoSuchParam foo",
    .requests = {
        
    },
    .skip = "unknown-param",
    .skip_all = "",
    .needs_engine = false,
},
    MatrixCase{
    .id = "P29",
    .cmd = "sls -SimpleMatch a.b x.cpp",
    .requests = {
        ExpectRequest{.engine = "select-string", .patterns = {"a.b"}, .single = true, .fixed = true, .paths = {"x.cpp"}, .globs = {}, .types = {}, .path_kind = "none", .cwd_hint = ""}
    },
    .skip = "",
    .skip_all = "",
    .needs_engine = true,
},
    MatrixCase{
    .id = "P30",
    .cmd = "Select-String -Pattern \"a$(x)b\"",
    .requests = {
        
    },
    .skip = "dynamic-argument",
    .skip_all = "",
    .needs_engine = false,
},
    MatrixCase{
    .id = "P31",
    .cmd = "& (Get-Command Select-String) foo",
    .requests = {
        
    },
    .skip = "",
    .skip_all = "",
    .needs_engine = false,
},
    MatrixCase{
    .id = "P32",
    .cmd = "<# Select-String foo #>",
    .requests = {
        
    },
    .skip = "",
    .skip_all = "",
    .needs_engine = false,
},
    MatrixCase{
    .id = "P33",
    .cmd = "Select-String -Pattern ''",
    .requests = {
        
    },
    .skip = "no-pattern",
    .skip_all = "",
    .needs_engine = false,
},
    MatrixCase{
    .id = "P34",
    .cmd = "Select-String foo -NotMatch",
    .requests = {
        
    },
    .skip = "invert",
    .skip_all = "",
    .needs_engine = false,
},
    MatrixCase{
    .id = "P35",
    .cmd = "Select-String -Path *.cpp -Pattern foo",
    .requests = {
        
    },
    .skip = "wildcard-path",
    .skip_all = "",
    .needs_engine = false,
},
    MatrixCase{
    .id = "P36",
    .cmd = "sls -Pattern $env:PATH",
    .requests = {
        
    },
    .skip = "dynamic-argument",
    .skip_all = "",
    .needs_engine = false,
},
    MatrixCase{
    .id = "P37",
    .cmd = "Set-Alias sls Get-Process; sls foo",
    .requests = {
        
    },
    .skip = "",
    .skip_all = "alias-shadow",
    .needs_engine = false,
},
    MatrixCase{
    .id = "P38",
    .cmd = "Select-String foo,bar -NotMatch",
    .requests = {
        
    },
    .skip = "invert",
    .skip_all = "",
    .needs_engine = false,
},
    MatrixCase{
    .id = "P39",
    .cmd = "Select-String -Pattern AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
    .requests = {
        
    },
    .skip = "too-long",
    .skip_all = "",
    .needs_engine = false,
},
    MatrixCase{
    .id = "P40",
    .cmd = "sls foo x.cpp",
    .requests = {
        ExpectRequest{.engine = "select-string", .patterns = {"foo"}, .single = true, .fixed = false, .paths = {"x.cpp"}, .globs = {}, .types = {}, .path_kind = "none", .cwd_hint = ""}
    },
    .skip = "",
    .skip_all = "",
    .needs_engine = true,
},
    MatrixCase{
    .id = "X05",
    .cmd = "sls myfunc -Pattern:foo",
    .requests = {
        ExpectRequest{.engine = "select-string", .patterns = {"foo"}, .single = true, .fixed = false, .paths = {"myfunc"}, .globs = {}, .types = {}, .path_kind = "none", .cwd_hint = ""}
    },
    .skip = "",
    .skip_all = "",
    .needs_engine = true,
},
    MatrixCase{
    .id = "X06",
    .cmd = "Select-String -SimpleMatch:$false 'a.b'",
    .requests = {
        ExpectRequest{.engine = "select-string", .patterns = {"a.b"}, .single = true, .fixed = false, .paths = {}, .globs = {}, .types = {}, .path_kind = "none", .cwd_hint = ""}
    },
    .skip = "",
    .skip_all = "",
    .needs_engine = true,
},
    MatrixCase{
    .id = "X07",
    .cmd = "Select-String -Pattern:foo -SimpleMatch:$true",
    .requests = {
        ExpectRequest{.engine = "select-string", .patterns = {"foo"}, .single = true, .fixed = true, .paths = {}, .globs = {}, .types = {}, .path_kind = "none", .cwd_hint = ""}
    },
    .skip = "",
    .skip_all = "",
    .needs_engine = true,
},
};


}  // namespace cg::test::shellmatrix
