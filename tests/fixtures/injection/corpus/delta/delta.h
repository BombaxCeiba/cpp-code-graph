#pragma once

namespace delta
{

    struct Item
    {
        int value;
    };

    /// 带模板实参的盒：显式实例化让实例名进入符号名表。
    template <class T>
    struct Box
    {
        T value;
    };

    extern Box<Item> g_box;

    int dv_zzq0tail();
    int dv_zzq1tail();
    int dv_zzq2tail();
    int dv_zzq3tail();
    int dv_zzq4tail();
    int dv_zzq5tail();
    int dv_zzq6tail();

    int x_solo_tail();

    int dv_tri_1();
    int dv_tri_2();
    int dv_tri_3();

    /// 六十八字节的长名字：前段夹着 solo_，后面拖着长尾。带两个贪婪分组
    /// 的模式在它上面切分方式过多，步数上限之内判不完，按不命中处理。
    int azqsolo_012345678901234567890123456789012345678901234567890123456789();

    /// 一组重载运算符：名字里带正则特殊字符（加号、星号、圆括号、方括
    /// 号、竖线、脱字符）的，原样当模式重查时匹配不到名字自身；只带尖
    /// 括号、等号一类普通字符的照常能查回来。
    struct DvOps
    {
        DvOps& operator+=(int) { return *this; }

        DvOps& operator-=(int) { return *this; }

        bool operator==(const DvOps&) const { return true; }

        bool operator!=(const DvOps&) const { return false; }

        bool operator<(const DvOps&) const { return false; }

        DvOps operator<<(int) const { return *this; }

        DvOps operator+(const DvOps&) const { return *this; }

        DvOps operator*(int) const { return *this; }

        int operator[](int) const { return 0; }

        int operator()(int) const { return 0; }

        DvOps operator|(const DvOps&) const { return *this; }

        DvOps operator^(const DvOps&) const { return *this; }
    };

}
