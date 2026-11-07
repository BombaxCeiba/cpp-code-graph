// 界外模板实例化夹具的头（roots 外，-I 指入）——模拟 <algorithm>
// 形态：模板体随使用方 TU 实例化、AST 可见，但定义位置在索引界外。
#pragma once

template <class F>
void algo(F f)
{
    f(1);  // 界外实例体内的 invoke（callee=形参 0）
}

template <class A, class F>
void algo2(A a, F f)
{
    f(a);  // 形参 1 的 invoke
}
