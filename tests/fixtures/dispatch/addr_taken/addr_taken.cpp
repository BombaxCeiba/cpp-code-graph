/**
 * @file 取址证据（#addr 行）的最小夹具：&Class::method、&free_fn 与
 *       对照组 &obj 三形态各就各位——采集契约见 cg::analysis 的
 *       address_of_evidence 证据（source=AddressOfObject、var_name="#addr"、
 *       related_key=被取址符号 unique_key）与 cg::db::
 *       address_taken_hints（按 unique_key 精确文本消费）。
 */
struct Handler
{
    int  on_event(int v);
    void reset();
};

int  Handler::on_event(int v) { return v + 1; }
void Handler::reset() {}

int free_step(int v) { return v * 2; }

using MemberFn = int (Handler::*)(int);
using FreeFn = int (*)(int);

int register_all()
{
    MemberFn m = &Handler::on_event; // &Class::method：#addr 证据行
    FreeFn   f = &free_step;         // &free_fn：#addr 证据行
    Handler  h;
    Handler* ph = &h; // &obj：对象取址（related_key 恒空）
    (void)m;
    (void)f;
    return ph->on_event(0);
}
