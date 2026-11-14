# Third-Party Notices

ccg statically links the following third-party components into `ccg.exe`.
License texts for the pinned revisions are available in the respective
upstream repositories.

| Component | Pinned revision | License | Copyright |
|---|---|---|---|
| yyjson | `0.13.0` | MIT | Copyright (c) 2020 YaoYuan `<ibireme@gmail.com>` |
| yyjson-cpp (cpp_yyjson) | `2ed9d20` | MIT | Copyright (c) 2023 Yoshiki Matsuda, (c) 2026 Dusk |
| Boost.PFR | `boost-1.92.0` | Boost Software License 1.0 | see the BSL-1.0 text |
| sqlpp23 | `0.70` | BSD-2-Clause | Copyright (c) 2013-2016 Roland Bock |
| tree-sitter | `v0.27.1` | MIT | Copyright (c) 2018 Max Brunsfeld |
| tree-sitter-bash | `v0.25.1` | MIT | Copyright (c) 2017 Max Brunsfeld |
| SQLite amalgamation | `3.54.0` | Public Domain | — |

The libclang engine is **not** part of this package: ccg resolves a local
LLVM installation (containing `bin/libclang.dll`) at run time, licensed
under the Apache License v2.0 with LLVM Exception by the LLVM Project.
