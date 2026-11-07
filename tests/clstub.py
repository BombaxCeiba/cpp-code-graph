"""clstub——ccg 包装模式闸门夹具：一个冒充"真实编译器"的
替身（Python 形态；前身是构建期编译成 exe 的 C 替身，为 git 零二进制
零 C 编译物而脚本化）。它把实际收到的宽字符 argv 记录（UTF-8、
单行 JSON）到 $CCG_CLSTUB_OUT，闸门由此证明 ccg.exe 的 wrap 模式
按位精确转发了展开后的 argv 且传播了退出码。它是被解释执行的测试
数据，不是项目源码。

与 C 前身逐项对齐的行为契约（wrap 单测面）：
  * argv 数组包含 argv[0]（Windows 下 sys.argv 源自宽字符命令行
    GetCommandLineW，Unicode 保真）；JSON 是 {"argv":[...]} 单行
    + 换行符，紧凑分隔符（逗号冒号后无空格），UTF-8 无 BOM。
  * CCG_CLSTUB_OUT 未设置或为空时静默不写文件；路径打不开也
    静默跳过（C 前身的 fopen 失败同款）。
  * stdout/stderr 绝不输出任何字节；未覆盖时退出码恒 7。

e2e 扩展（环境变量驱动，缺省值与上列契约逐项相同）：
  * CCG_CLSTUB_RC 覆盖退出码（缺省 7 不变；非整数/越界回 7）。
    e2e 设 0 让 MSBuild 的 CL 任务把每次调用判成功，整条构建图
    走到底（记录面才完备）。
  * CCG_CLSTUB_OUT 指向已存在的目录时切换目录追加模式：每次
    调用写一个独立文件（call-<pid>-<序号>.jsonl，载荷加 cwd
    字段供保真闸核对工作目录）。多调用累计靠"一调用一文件"实现
    ——MSBuild /MP 下桩被多进程并发调用，Windows 上对同一文件
    的追加写不是原子操作（msvcrt 的 _O_APPEND 是 seek 到末尾再
    写两步），独立文件让并发零竞态。指向普通路径（不存在或已是
    文件）时保持单次覆盖写，wrap 单测的"全新 OUT 路径、单次调用"
    形态逐字节不变。
  * 退出码为 0（构建成功语义）时按 argv 里的 /Fo 目标 touch 空
    obj（连写 "/Fo<目录>\" 按目录+源文件名推导、"/Fo<路径>" 直接触
    touch；分写 "/Fo" "<值>" 同理）。MSBuild 的 CL 任务本身不校验
    产物在盘（工具退出 0 即任务成功），touch 是给依赖产物存在性的
    下游步骤（Lib/Link 之外的复制/校验类）兜底——空文件不是合法
    COFF，真 Lib/Link 仍会失败并被 ccg 如实归类，那是 e2e 已接受
    的记录面完备性换来的退出码（ccg 自身 exit 0）。产物物化失败
    静默跳过（桩绝不输出、绝不杀构建）。

拉起方式：ccg wrap 对 .py 后缀的 real_cl 自动前插 PATH 上的
python 解释器（CreateProcessW 不能直接执行 .py），见 wrap 模块。
"""

import json
import os
import sys

# cl 方言里可能成为编译输入的源扩展（权威集 = util.ixx 的
# is_tu_input_file：classify_source_path 表 561-581 行的 CSource/
# CppSource/ModuleUnit 三类并集；桩只按它推导 /Fo 目录形态下的
# 产物文件名，不做语义判断）。
_SOURCE_EXTS = {".cpp", ".cxx", ".cc", ".c++", ".c", ".cppm", ".cxxm",
                ".ixx", ".mpp", ".mxx", ".c++m"}


def _exit_code() -> int:
    """CCG_CLSTUB_RC 覆盖的退出码；缺省 7，非整数/越 0..255 界回 7。"""
    raw = os.environ.get("CCG_CLSTUB_RC")
    if raw is None:
        return 7
    try:
        code = int(raw, 10)
    except ValueError:
        return 7
    return code if 0 <= code <= 255 else 7


def _record(argv: list) -> None:
    """把本次 argv 落盘（单文件覆盖或目录追加），失败静默。"""
    out = os.environ.get("CCG_CLSTUB_OUT")
    if not out:
        return
    try:
        if os.path.isdir(out):
            # 目录追加模式（e2e）：一调用一文件，文件名 pid+递增序号
            # 唯一（同一桩进程内串行、跨进程 pid 互异），载荷带 cwd。
            seq = 0
            while True:
                path = os.path.join(
                    out, "call-%d-%d.jsonl" % (os.getpid(), seq))
                if not os.path.exists(path):
                    break
                seq += 1
            payload = {"argv": argv, "cwd": os.getcwd()}
        else:
            # 单文件模式（wrap 单测契约）：{"argv":[...]} 单行，与 C
            # 前身逐字节同形。
            path = out
            payload = {"argv": argv}
        line = json.dumps(
            payload, ensure_ascii=False, separators=(",", ":"))
        # newline="\n"：Windows 文本模式不把换行翻成 CRLF，
        # 与 C 前身的二进制写（"wb"）逐字节同形。
        with open(path, "w", encoding="utf-8", newline="\n") as sink:
            sink.write(line + "\n")
    except OSError:
        pass  # 打不开就静默跳过，退出码不受影响


def _touch_products(argv: list) -> None:
    """按 /Fo 目标 touch 空 obj（构建成功语义），失败静默。

    只解析 /Fo（连写 "/Fo值" 与分写 "/Fo" "值" 两种形态）与位置源
    文件；/Fo 值以分隔符结尾是目录形态（产物名 = 源文件主名.obj，
    cl 的既有推导），否则视为完整产物路径直接 touch。
    """
    fo_value = None
    sources = []
    i = 1
    while i < len(argv):
        a = argv[i]
        if a == "/Fo" and i + 1 < len(argv):
            fo_value = argv[i + 1]
            i += 2
            continue
        if a.startswith("/Fo") and len(a) > 3:
            fo_value = a[3:]
            i += 1
            continue
        if a and a[0] not in "/-" and \
                os.path.splitext(a)[1].lower() in _SOURCE_EXTS:
            sources.append(a)
        i += 1
    try:
        if fo_value is None:
            return
        if fo_value.endswith(("\\", "/")):
            os.makedirs(fo_value, exist_ok=True)
            for src in sources:
                stem = os.path.splitext(os.path.basename(src))[0]
                open(os.path.join(fo_value, stem + ".obj"), "wb").close()
        else:
            parent = os.path.dirname(fo_value)
            if parent:
                os.makedirs(parent, exist_ok=True)
            open(fo_value, "wb").close()
    except OSError:
        pass


def main() -> int:
    code = _exit_code()
    _record(sys.argv)
    if code == 0:
        _touch_products(sys.argv)
    return code


if __name__ == "__main__":
    sys.exit(main())
