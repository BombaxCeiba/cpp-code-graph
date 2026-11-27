/**
 * @file util.json.bridge.cpp
 *
 * @brief cg.util.json 门面的经典翻译单元：全工程唯一触碰 yyjson 的地方。
 *
 * 只使用钉定 yyjson 目标的原生 C API；cpp_yyjson 类型为何必须留在
 * MSVC 的模块视界之外，见 util.json.bridge.h 的说明。本文件里的一切
 * 在每个工具链上都不经模块机制编译。
 */

#include "util.json.bridge.h"

#include <yyjson.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <vector>

namespace
{

    /**
     * @brief 单个写出器的私有状态：可变文档、其根值，以及挂入树中的每个
     *        写出器。
     *
     * @internal
     * 被挂接的子写出器保有自己的分配（yyjson 按文档池释放，故跨文档链接
     * 的释放方式是每个池恰释放一次——与 yyjson-cpp 为其 writer 对象实现的
     * 所有权方案相同，省去了共享指针开销）。
     */
    struct ccg_json_writer
    {
        yyjson_mut_doc*               doc;
        yyjson_mut_val*               root;
        std::vector<ccg_json_writer*> adopted;
    };

    /**
     * @brief 构造键值（总是拷贝的字符串值，门面契约）。
     *
     * @internal 用带长度的 yyjson mutable 接口构建，内嵌 NUL 的键得以存活。
     */
    yyjson_mut_val* copied_key(ccg_json_writer* w, const char* key, size_t len)
    {
        return yyjson_mut_strncpy(w->doc, key, len);
    }

    void obj_add(ccg_json_writer* w, yyjson_mut_val* key, yyjson_mut_val* value)
    {
        (void)yyjson_mut_obj_add(w->root, key, value);
    }

    /**
     * @brief 把子写出器的根挂入父树（键非空挂对象成员，空键挂数组元素）。
     *
     * @internal
     * 子的根值并入父树，而其分配仍留在子的池上；父经 adopted 列表传递地
     * 释放各个池。先给收养列表扩容再并入：扩容可能抛出，抛出时父子两棵
     * 树都还没有被动过，子写出器仍归调用方所有；并入之后再登记就不会失
     * 败，父树里不会留下指向已交还的池的引用。并入本身失败（内存不足）
     * 时子的根值没有进父树，但子写出器仍被父收养——门面已经交出它的所
     * 有权，不收养就会漏掉整个池。
     */
    void attach(
        ccg_json_writer* parent,
        yyjson_mut_val*  key,
        ccg_json_writer* child)
    {
        if (child == nullptr || child->root == nullptr)
        {
            return;
        }
        parent->adopted.reserve(parent->adopted.size() + 1);
        if (key != nullptr)
        {
            (void)yyjson_mut_obj_add(parent->root, key, child->root);
        }
        else
        {
            (void)yyjson_mut_arr_add_val(parent->root, child->root);
        }
        child->root = nullptr;
        parent->adopted.push_back(child);
    }

    /**
     * @brief 创建拥有式写出器，其根为对象（is_array == 0）或数组
     *        （is_array != 0）。
     *
     * 键与字符串值总是被拷贝进文档（门面契约：调用方可传短命字符串上的
     * 视图）。仅被同翻译单元的 ccg_json_writer_ensure 消费，不进 C 边界。
     */
    void* writer_new(int is_array)
    {
        // 外壳一建成就交给唯一指针：可变文档建不成时，随本函数返回回收。
        std::unique_ptr<ccg_json_writer> w{new (std::nothrow)
                                               ccg_json_writer{}};
        if (w == nullptr)
        {
            return nullptr;
        }
        w->doc = yyjson_mut_doc_new(nullptr);
        if (w->doc == nullptr)
        {
            return nullptr;
        }
        w->root =
            is_array != 0 ? yyjson_mut_arr(w->doc) : yyjson_mut_obj(w->doc);
        yyjson_mut_doc_set_root(w->doc, w->root);
        return w.release();
    }

    /** @brief 视图句柄即裸 yyjson 值指针（借入式，永不拥有）。 */
    const yyjson_val* val(const void* v)
    {
        return static_cast<const yyjson_val*>(v);
    }

    thread_local char ccg_json_err_buf[256] = {};

} // namespace

extern "C"
{

    void* ccg_json_doc_parse(const char* data, size_t len, const char** err_msg)
    {
        yyjson_read_err err;
        yyjson_doc*     doc = yyjson_read_opts(
            const_cast<char*>(data),
            len,
            YYJSON_READ_NOFLAG,
            nullptr,
            &err);
        if (doc != nullptr)
        {
            return doc;
        }
        std::snprintf(
            ccg_json_err_buf,
            sizeof ccg_json_err_buf,
            "%s at position %zu (code %d)",
            err.msg,
            err.pos,
            static_cast<int>(err.code));
        if (err_msg != nullptr)
        {
            *err_msg = ccg_json_err_buf;
        }
        return nullptr;
    }

    void ccg_json_doc_free(void* doc)
    {
        yyjson_doc_free(static_cast<yyjson_doc*>(doc));
    }

    const void* ccg_json_doc_root(const void* doc)
    {
        if (doc == nullptr)
        {
            return nullptr;
        }
        return yyjson_doc_get_root(static_cast<const yyjson_doc*>(doc));
    }

    int ccg_json_is_string(const void* v)
    {
        return v != nullptr && yyjson_is_str(val(v));
    }

    int ccg_json_is_array(const void* v)
    {
        return v != nullptr && yyjson_is_arr(val(v));
    }

    int ccg_json_is_object(const void* v)
    {
        return v != nullptr && yyjson_is_obj(val(v));
    }

    int ccg_json_is_bool(const void* v)
    {
        return v != nullptr && yyjson_is_bool(val(v));
    }

    int ccg_json_is_number(const void* v)
    {
        return v != nullptr && yyjson_is_num(val(v));
    }

    const char* ccg_json_str(const void* v, size_t* len)
    {
        if (v == nullptr || !yyjson_is_str(val(v)))
        {
            return nullptr;
        }
        *len = yyjson_get_len(val(v));
        return yyjson_get_str(val(v));
    }

    int ccg_json_bool(const void* v, int* ok)
    {
        if (v != nullptr && yyjson_is_bool(val(v)))
        {
            *ok = 1;
            return yyjson_get_bool(val(v));
        }
        *ok = 0;
        return 0;
    }

    long long ccg_json_int(const void* v, int* ok)
    {
        if (v != nullptr && yyjson_is_int(val(v)))
        {
            *ok = 1;
            return yyjson_get_sint(val(v));
        }
        *ok = 0;
        return 0;
    }

    size_t ccg_json_size(const void* v)
    {
        if (v == nullptr)
        {
            return 0;
        }
        if (yyjson_is_arr(val(v)))
        {
            return yyjson_arr_size(val(v));
        }
        if (yyjson_is_obj(val(v)))
        {
            return yyjson_obj_size(val(v));
        }
        return 0;
    }

    const void* ccg_json_arr_at(const void* v, size_t index)
    {
        if (v == nullptr || !yyjson_is_arr(val(v)))
        {
            return nullptr;
        }
        return yyjson_arr_get(val(v), index);
    }

    const void* ccg_json_obj_get(const void* v, const char* key, size_t key_len)
    {
        if (v == nullptr || !yyjson_is_obj(val(v)))
        {
            return nullptr;
        }
        return yyjson_obj_getn(val(v), key, key_len);
    }

    const void* ccg_json_member_at(
        const void*  v,
        size_t       index,
        const char** key,
        size_t*      key_len)
    {
        if (v == nullptr || !yyjson_is_obj(val(v))
            || index >= yyjson_obj_size(val(v)))
        {
            return nullptr;
        }
        yyjson_obj_iter iter;
        if (!yyjson_obj_iter_init(val(v), &iter))
        {
            return nullptr;
        }
        yyjson_val* member_key = nullptr;
        for (size_t n = 0; n <= index; ++n)
        {
            member_key = yyjson_obj_iter_next(&iter);
            if (member_key == nullptr)
            {
                return nullptr;
            }
        }
        *key = yyjson_get_str(member_key);
        *key_len = yyjson_get_len(member_key);
        return yyjson_obj_iter_get_val(member_key);
    }

    void* ccg_json_writer_ensure(void* w, int is_array)
    {
        return w != nullptr ? w : writer_new(is_array);
    }

    void ccg_json_writer_free(void* w)
    {
        auto* writer = static_cast<ccg_json_writer*>(w);
        if (writer == nullptr)
        {
            return;
        }
        for (ccg_json_writer* child : writer->adopted)
        {
            ccg_json_writer_free(child);
        }
        yyjson_mut_doc_free(writer->doc);
        delete writer;
    }

    void ccg_json_obj_set_str(
        void*       w,
        const char* key,
        size_t      key_len,
        const char* value,
        size_t      value_len)
    {
        auto* writer = static_cast<ccg_json_writer*>(w);
        obj_add(
            writer,
            copied_key(writer, key, key_len),
            yyjson_mut_strncpy(writer->doc, value, value_len));
    }

    void
    ccg_json_obj_set_bool(void* w, const char* key, size_t key_len, int value)
    {
        auto* writer = static_cast<ccg_json_writer*>(w);
        obj_add(
            writer,
            copied_key(writer, key, key_len),
            yyjson_mut_bool(writer->doc, value != 0));
    }

    void ccg_json_obj_set_int(
        void*       w,
        const char* key,
        size_t      key_len,
        long long   value)
    {
        auto* writer = static_cast<ccg_json_writer*>(w);
        obj_add(
            writer,
            copied_key(writer, key, key_len),
            yyjson_mut_sint(writer->doc, value));
    }

    void ccg_json_obj_set_real(
        void*       w,
        const char* key,
        size_t      key_len,
        double      value)
    {
        auto* writer = static_cast<ccg_json_writer*>(w);
        obj_add(
            writer,
            copied_key(writer, key, key_len),
            yyjson_mut_real(writer->doc, value));
    }

    void ccg_json_obj_set_null(void* w, const char* key, size_t key_len)
    {
        auto* writer = static_cast<ccg_json_writer*>(w);
        obj_add(
            writer,
            copied_key(writer, key, key_len),
            yyjson_mut_null(writer->doc));
    }

    void ccg_json_obj_set_writer(
        void*       w,
        const char* key,
        size_t      key_len,
        void*       nested)
    {
        auto* writer = static_cast<ccg_json_writer*>(w);
        attach(
            writer,
            copied_key(writer, key, key_len),
            static_cast<ccg_json_writer*>(nested));
    }

    void ccg_json_arr_add_str(void* w, const char* value, size_t value_len)
    {
        auto* writer = static_cast<ccg_json_writer*>(w);
        (void)yyjson_mut_arr_add_val(
            writer->root,
            yyjson_mut_strncpy(writer->doc, value, value_len));
    }

    void ccg_json_arr_add_bool(void* w, int value)
    {
        auto* writer = static_cast<ccg_json_writer*>(w);
        (void)yyjson_mut_arr_add_val(
            writer->root,
            yyjson_mut_bool(writer->doc, value != 0));
    }

    void ccg_json_arr_add_int(void* w, long long value)
    {
        auto* writer = static_cast<ccg_json_writer*>(w);
        (void)yyjson_mut_arr_add_val(
            writer->root,
            yyjson_mut_sint(writer->doc, value));
    }

    void ccg_json_arr_add_real(void* w, double value)
    {
        auto* writer = static_cast<ccg_json_writer*>(w);
        (void)yyjson_mut_arr_add_val(
            writer->root,
            yyjson_mut_real(writer->doc, value));
    }

    void ccg_json_arr_add_writer(void* w, void* nested)
    {
        auto* writer = static_cast<ccg_json_writer*>(w);
        attach(writer, nullptr, static_cast<ccg_json_writer*>(nested));
    }

    char* ccg_json_writer_write(void* w, size_t* len)
    {
        auto* writer = static_cast<ccg_json_writer*>(w);
        return yyjson_mut_write(writer->doc, YYJSON_WRITE_NOFLAG, len);
    }

    /**
     * @brief 释放 ccg_json_writer_write 返回的缓冲区。
     *
     * @internal yyjson 的默认分配器是 libc，写出缓冲就是普通的 malloc
     * 结果（yyjson_mut_write_opts 的文档）。
     */
    void ccg_json_buf_free(char* buffer) { std::free(buffer); }

} // extern "C"
