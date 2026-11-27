#include "notes.h"

namespace Gamma
{

    int note_create() { return 0; }

    int note_edit(int id) { return id > 0 ? 1 : 0; }

    int note_erase(int id) { return id > 0 ? 1 : 0; }

    int note_find(int keyword) { return keyword; }

    int note_list() { return 8; }

    int note_move(int id, int target) { return id + target; }

    int note_pin(int id) { return id > 0 ? 1 : 0; }

    int note_unpin(int id) { return id > 0 ? 1 : 0; }

}
