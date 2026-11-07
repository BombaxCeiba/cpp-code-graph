// 跨配置钉子探针：Debug 用默认工具集、Release 的 PropertyGroup 钉
// VCToolsVersion=14.44（只钉大版本；完整版本运行时替换，per-configuration
// 条件——不带配置求值时
// 钉子不命中的形态）。门禁断言 Release 的 cl 解析与 cdb 兼容版本都
// 来自 14.44 工具集。
#include <cstddef>
int vcfgpin_probe() { return _MSC_VER; }
int main() { return 0; }
