// 平台回调 API 名单夹具（19 条取五形态）——TU 内 extern 声明
// 触发完整链路（名字匹配不看真身；externalout 夹具同哲学）。
extern "C"
{
    void     _beginthread(void (*)(void*), unsigned, void*);
    unsigned _beginthreadex(
        void*,
        unsigned,
        unsigned (*)(void*),
        void*,
        unsigned,
        unsigned*);
    unsigned long CreateThread(
        void*,
        unsigned,
        unsigned (*)(void*),
        void*,
        unsigned,
        unsigned*);
    void qsort(
        void*,
        unsigned long,
        unsigned long,
        int (*)(const void*, const void*));
    int EnumWindows(int (*)(void*, long), long);
    int
    SetWindowsHookExW(int, int (*)(int, unsigned, long), void*, unsigned long);
    int NotInList(void (*)(int)); // 反例：名单外，无线索边
}

static int      cmp(const void* a, const void* b) { return 0; }
static unsigned thread_fn(void*) { return 0; }
static void     begin_fn(void*) {}
static int      enum_fn(void*, long) { return 1; }
static int      hook_fn(int, unsigned, long) { return 0; }
static void     unused_cb(int) {}

void install()
{
    _beginthread(&begin_fn, 0, nullptr);                        // 位1 取址
    _beginthreadex(nullptr, 0, thread_fn, nullptr, 0, nullptr); // 位3 隐式
    CreateThread(nullptr, 0, thread_fn, nullptr, 0, nullptr);   // 位3 隐式
    qsort(nullptr, 0, 0, cmp);                 // 位4 函数名隐式
    EnumWindows(enum_fn, 0);                   // 位1 隐式
    SetWindowsHookExW(0, hook_fn, nullptr, 0); // 位2 隐式
    NotInList(unused_cb);                      // 反例
}
