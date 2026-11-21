#include "a.hpp"

// 声称消费 TU（本文件全程干净）：接收者类型证据只来自转型声称。
// 链形态一（别名链）：#ret(opaque_source) -> raw -> p，脏改上游 TU 时
// 闭包沿链卷入 p 槽，复位重算后声称缓冲是唯一的类型注入源。
int consume_claim_via_alias()
{
    IFace* p = nullptr;
    void* raw = opaque_source();
    p = reinterpret_cast<IFace*>(raw);
    p = g_obj;
    return p->emit();
}

// 链形态二（返回链）：q 槽直接挂 #ret(opaque_source) 的 ret_src 边。
int consume_claim_via_ret()
{
    IFace* q = static_cast<IFace*>(opaque_source());
    return q->emit();
}

int main()
{
    return consume_claim_via_alias() + consume_claim_via_ret();
}
