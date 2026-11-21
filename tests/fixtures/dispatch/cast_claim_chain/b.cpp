#include "a.hpp"

// 上游 TU（增量轮的脏文件）：返回源与全局对象都在这里，脏改本文件即把
// 干净 TU 里的声称槽卷入增量闭包。
IFace* g_obj;
void* opaque_source()
{
    static char storage[8];
    return storage;
}
int Impl1::emit() { return 1; }
int Impl2::emit() { return 2; }
