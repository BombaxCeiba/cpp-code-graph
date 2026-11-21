#pragma once

// 跨 TU 声称链夹具的共享接口族：声称面 = IFace ∪ 实现族闭包。
struct IFace
{
    virtual int emit() = 0;
    virtual ~IFace() = default;
};
struct Impl1 : IFace
{
    int emit() override;
};
struct Impl2 : IFace
{
    int emit() override;
};

void* opaque_source();
extern IFace* g_obj;
