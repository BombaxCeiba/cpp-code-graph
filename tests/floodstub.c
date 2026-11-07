/* ccg_msbuild_capture 闸门的 stderr 洪水发现桩。以如下方式调用
   `floodstub.exe /?` - the cl banner probe that goes through run_capture -
   it writes 128 KiB to stderr, far beyond any anonymous-pipe buffer, BEFORE
   its single stdout line: a parent that drains stdout to EOF first
   deadlocks (the stub blocks in the stderr write while the parent blocks in
   ReadFile on the stdout pipe), a concurrent drain completes. The same
   flood also rides `--version` - the driver partition's run_process
   channel (compiler_version's producer probe), the audit-found
   regression shape: a BMI build failing with a template-error cascade
   writes megabytes of stderr to an EMPTY stdout. Any other argv (the
   wrap-mode forward of a real cl invocation) is a silent exit 0, keeping
   the captured build log clean. Plain .c test data by the same plan
   sanction as sqlite_smoke.c (clstub 同族桩已脚本化为 clstub.py；
   本桩暂留 C：消费点把桩路径编译进 C++ 测试二进制直接派生，且考的
   是 stderr 洪水先于 stdout 的管道排空时序)，never a project source. */
#include <stdio.h>
#include <string.h>

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "/?") == 0 || strcmp(argv[i], "--version") == 0)
        {
            static const char chunk[] = "0123456789abcdef0123456789abcdef";
            for (int n = 0; n < 4096; ++n)
            {
                fwrite(chunk, 1, sizeof(chunk) - 1, stderr);
            }
            fflush(stderr);
            puts("floodstub probe done");
            return 0;
        }
    }
    return 0;
}
