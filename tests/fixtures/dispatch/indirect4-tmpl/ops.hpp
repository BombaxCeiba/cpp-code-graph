// 界外字段槽夹具的头（roots 外，-I 指入）——def/use 都发生在界外
// 实例体内，复刻 stdexec 池 run 调用链的核心心跳（static_thread_pool.hpp 的
// `__execute` 存储+取出调用）的最小形态。
#pragma once

using R8Fn = void (*)(int);

struct r8_op_state {
    R8Fn execute_;
};

// use：run 循环对形参的函数指针字段发起调用（形参 0 的字段槽锚点）。
inline void r8_run_loop(r8_op_state* task, int tid)
{
    task->execute_(tid);
}

// def：构造模板体内，形参（指针）的函数指针字段被赋一个就地 lambda。
// 触发链头（main）经 cpo/实例桥进入本模板体，entry_caller 随之继承。
template <class R>
inline void r8_make_op(r8_op_state* s, R recv)
{
    (void)recv;
    s->execute_ = [](int tid) { (void)tid; };
    // 链闭环：构造后交给 run 循环（stdexec 池 run 调用链形态——池的 run
    // 循环从 operation state 取 __execute 调用）。缺这一跳时
    // r8_run_loop 无人进入，use 侧永远零落（夹具缺陷，已修）。
    r8_run_loop(s, 0);
}
