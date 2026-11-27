#pragma once

namespace Gamma
{

    /// 新建一条空便笺，返回它的编号。
    int note_create();

    /// 修改便笺正文，返回受影响的行数。
    int note_edit(int id);

    /// 删除便笺，返回被删的行数。
    int note_erase(int id);

    /// 按关键字查找便笺，返回命中的条数。
    int note_find(int keyword);

    /// 列出全部便笺，返回条数。
    int note_list();

    /// 把便笺移到另一个笔记本，返回新的笔记本编号。
    int note_move(int id, int target);

    /// 把便笺钉住，返回钉住的条数。
    int note_pin(int id);

    /// 取消钉住，返回受影响的行数。
    int note_unpin(int id);

}
