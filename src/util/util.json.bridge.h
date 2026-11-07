#ifndef CCG_UTIL_JSON_BRIDGE_H
#define CCG_UTIL_JSON_BRIDGE_H
/**
 * @file util.json.bridge.h
 *
 * @brief cg.util.json 门面的 C 边界。
 *
 * 每个 yyjson 触点都住在 util.json.bridge.cpp——一个经典翻译单元：
 * MSVC 14.51 的模块 IFC writer 无法序列化命名模块单元内出现的任何
 * yyjson-cpp（cpp_yyjson.hpp）类型——即使这些类型只占据导出类的私有
 * 成员、且无论有无 import std，导出类都会令编译器以 C1001
 * "module writer.cpp:2210" 崩溃（同样的调用在经典翻译单元中实例化则
 * 正常；二分探针 jsonprobe 变体 v1-v6 收敛的结论）。门面模块因此只存
 * 不透明句柄并调用这些 extern 函数；下方的纯 C ABI 让模块的全局模块
 * 片段既无 STL 也无 yyjson 声明（紧邻 import std; 的经典 STL include
 * 会因重定义报 C1116、经导出模板回放全局模块片段静态内联帮助函数会令
 * 导入方报 C2129——另两颗 MSVC 模块地雷，故同样回避）。
 *
 * 句柄约定：
 *   - 文档/写出器句柄由调用方拥有（经配对的 bridge 调用恰释放一次）；
 *   - 值句柄是借入的裸 yyjson 值指针，在所属文档存活期间有效；
 *     可拷贝，永不释放。
 */

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief 解析一份完整的 UTF-8 JSON 文档。
     *
     * @return 拥有式文档句柄；输入非法时返回 NULL，且 *err_msg 指向
     *         线程局部消息（有效至下一次调用）。
     */
    void* ccg_json_doc_parse(
        const char*  data,
        size_t       len,
        const char** err_msg);
    /** @brief 释放拥有式文档句柄（NULL 为无操作）。 */
    void ccg_json_doc_free(void* doc);
    /** @brief 文档的根值句柄（空文档为 NULL）。 */
    const void* ccg_json_doc_root(const void* doc);

    int ccg_json_is_string(const void* v);
    int ccg_json_is_array(const void* v);
    int ccg_json_is_object(const void* v);
    int ccg_json_is_bool(const void* v);
    int ccg_json_is_number(const void* v);
    /** @brief 指向文档缓冲区的视图；值不是字符串时返回 NULL。 */
    const char* ccg_json_str(const void* v, size_t* len);
    /** @brief 类型不匹配时 *ok = 0（返回值语义：原值不动，返回 0）。 */
    int ccg_json_bool(const void* v, int* ok);
    /** @brief 整数值（64 位带符号）；类型不匹配时 *ok = 0、返回 0。 */
    long long ccg_json_int(const void* v, int* ok);
    /** @brief 数组或对象的元素计数；其他任何类型一律为 0。 */
    size_t ccg_json_size(const void* v);
    /** @brief 数组元素；越界或非数组时返回 NULL。 */
    const void* ccg_json_arr_at(const void* v, size_t index);
    /** @brief 对象成员；缺失或非对象时返回 NULL。 */
    const void* ccg_json_obj_get(
        const void* v,
        const char* key,
        size_t      key_len);
    /**
     * @brief 文档顺序中的第 index 个对象成员。
     *
     * @return 越界或非对象时返回 NULL；否则把 *key 与 *key_len 设为指向
     *         文档缓冲区的视图。
     */
    const void* ccg_json_member_at(
        const void*  v,
        size_t       index,
        const char** key,
        size_t*      key_len);

    /**
     * @brief w 非 NULL 时原样返回，否则按给定根种类新建写出器
     *        （is_array == 0 为对象根，非 0 为数组根）。
     *
     * 为默认构造的门面构建器提供惰性创建。
     */
    void* ccg_json_writer_ensure(void* w, int is_array);
    /** @brief 释放拥有式写出器句柄（NULL 为无操作），含挂接其上的每个写出器。
     */
    void ccg_json_writer_free(void* w);

    void ccg_json_obj_set_str(
        void*       w,
        const char* key,
        size_t      key_len,
        const char* value,
        size_t      value_len);
    void
    ccg_json_obj_set_bool(void* w, const char* key, size_t key_len, int value);
    void ccg_json_obj_set_int(
        void*       w,
        const char* key,
        size_t      key_len,
        long long   value);
    void ccg_json_obj_set_real(
        void*       w,
        const char* key,
        size_t      key_len,
        double      value);
    void ccg_json_obj_set_null(void* w, const char* key, size_t key_len);
    /**
     * @brief 把嵌套写出器的根挂到该键下。
     *
     * 嵌套句柄被消费：其分配经父节点存活，调用方此后不得再释放或复用它。
     */
    void ccg_json_obj_set_writer(
        void*       w,
        const char* key,
        size_t      key_len,
        void*       nested);

    void ccg_json_arr_add_str(void* w, const char* value, size_t value_len);
    void ccg_json_arr_add_bool(void* w, int value);
    void ccg_json_arr_add_int(void* w, long long value);
    void ccg_json_arr_add_real(void* w, double value);
    /** @brief ccg_json_obj_set_writer 的数组根孪生（同样的消费规则）。 */
    void ccg_json_arr_add_writer(void* w, void* nested);

    /**
     * @brief 把写出器的树序列化为紧凑 JSON。
     *
     * @return malloc 的 NUL 结尾 UTF-8 缓冲，并设置 *len（用
     *         ccg_json_buf_free 释放）；仅分配失败时返回 NULL。
     */
    char* ccg_json_writer_write(void* w, size_t* len);
    void  ccg_json_buf_free(char* buffer);

#ifdef __cplusplus
}
#endif

#endif /* CCG_UTIL_JSON_BRIDGE_H */
