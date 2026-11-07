// 件一夹具：界外字段槽（def 在界外构造模板体内、use 在界外 run
// 循环体内）——期望 indirect 边 run_loop -> 就地 lambda 的 operator()。
#include "ops.hpp"

int main()
{
    r8_op_state s;
    r8_make_op(&s, 1);
}
