// 件一/件二夹具：界外模板实例桥 + def 事件四形态的消费。
#include "tmpl.hpp"

static int  cb(int x) { return x + 1; }
static int  cb2(int x) { return x + 2; }
static int  target_fn(int) { return 0; }
static void target_fn2() {}

// ---- 实例桥（形态：界外模板实例化体）----
void use_algo() { algo(&cb); }
void use_algo_multi() { algo(cb2); } // 同 host+param 第二目标（candidate 面）
void use_algo2() { algo2(7, cb); }   // param 1 桥
void use_lambda()
{
    algo([](int x) { return x * 2; });
}

// ---- 形态 1：函数内直取直调（局部变量槽）----
void local_var_call()
{
    int (*p)(int) = &target_fn;
    p(3);
}

// ---- 形态 2：一层跨函数传参（形参直配）----
void runner(int (*fp)(int)) { fp(1); }
void call_runner() { runner(&target_fn); }

// ---- 形态 4：成员字段中转 ----
struct Task
{
    void (*exec)();
};
void store_exec(Task* t) { t->exec = &target_fn2; }
void fire(Task* t) { t->exec(); }

// ---- 反例：经变量中转的界外实例调用（宁漏勿猜，不硬连）----
void via_var()
{
    void (*q)(int (*)(int)) = &algo<int (*)(int)>;
    q(&cb); // q 的槽目标是界外实例（不可连），站点保留 indirect 降级
}
