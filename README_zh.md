# simlog — 为 EDA 仿真中的 spdlog 提供仿真时间戳

[English](README.md) | **中文**

一个轻量的 [spdlog](https://github.com/gabime/spdlog) 扩展，为 EDA 仿真中的日志行加上**仿真时间**戳。
时间由以下两个编译期后端之一提供：

| 后端 | 宏 | 仿真时间来源 | 依赖 |
|---|---|---|---|
| **SVDPI**（默认） | `SIMLOG_BACKEND_SVDPI` | DPI 时间 API `svGetTime()` / `svGetTimePrecision()`（IEEE 1800-2023）；旧版仿真器上回退到 `vpi_get_time()` / `tf_gettimeprecision()` | spdlog + `libsimlog.so`（编译型后端，实现在 `src/simlog.cpp`） |
| SystemC | `SIMLOG_BACKEND_SYSTEMC` | `sc_time_stamp()` × `sc_get_time_resolution()` | spdlog + SystemC（header-only） |

- **无需自定义 logger 类型**：`simlog::get()` 返回原生 `spdlog::logger`，`add_sink` / `set_level` 照常可用

## 快速开始

### SVDPI（默认后端）

#### SystemVerilog 仿真器中的 DPI-C 代码

```cpp
#include <simlog/simlog.h>  // SIMLOG_BACKEND_SVDPI 为默认值，无需定义任何宏

extern "C" void tb_step(int cycle) {  // 任意由 SystemVerilog 侧 import 的函数
  auto log = simlog::get("tb_top");   // 首次调用：创建、注册并绑定格式化器
  log->info("cycle {}", cycle);       // 此后即为原生 spdlog API
}
```

仿真精度为 1 ps 时输出：

```
[       12340 ps][tb_top][info] cycle 42
```

#### 在 SystemVerilog 中使用：`simlog_logger`（`sv/simlog_pkg.sv`）

```systemverilog
import simlog_pkg::*;

module tb;
  initial begin
    // %m 在此展开为模块的层次化名称，正是 SIMLOG_LEVEL 的模块规则所匹配的名字。
    simlog_logger lg = new($sformatf("%m"));
    lg.info("reset released");
    #10;
    lg.warn($sformatf("cnt=%0d", 1));  // SystemVerilog 不支持变参调用
    lg.flush();                        // $finish 不会调用析构函数
  end
endmodule
```

```
[           0 ps][tb][info] reset released
[       10000 ps][tb][warning] cnt=1
```

`xmake` 将库生成到 `build/<platform>/<arch>/release/libsimlog.so`，连同 SV 源码一起交给仿真器：

```bash
vcs -full64 -sverilog -sv_lib <build-dir>/libsimlog tb.sv   # 也可把 .so 作为文件参数传入
```

日志行的输出去向由后端决定：SVDPI 的默认 sink 通过仿真器自身的输出函数（`vpi_printf`）打印，因此
`simv -l sim.log` 中能与 `$display` 的输出并列看到这些行。DPI-C 库自己的 `printf` 则不会进入 `-l`
文件：`-l` 记录的是仿真器的输出流，而非 C 运行时的 stdout。若宿主不提供 VPI（例如普通宿主程序），
日志行仍输出到 stdout。

| SystemVerilog | C ABI（`libsimlog.so`） | 说明 |
|---|---|---|
| `new(name)` | `simlog_open(name)` | 每个 logger 调用一次；handle *就是*已注册的 spdlog logger，因此对同一名字两次 open 得到的是同一个 logger |
| `lg.trace/debug/info/warn/error/critical(msg)` | `simlog_log(handle, level, msg)` | `SIMLOG_TRACE` … `SIMLOG_CRITICAL` 对应 spdlog 的级别 0 … 5 |
| `lg.flush()` | `simlog_flush(handle)` | 在 `$finish` 之前调用，以便冲刷缓冲 |

### SystemC（`--backend=systemc`）

```cpp
#include <simlog/simlog.h>

int sc_main(int, char**) {
  auto log = simlog::get("tb_top");  // 首次调用：创建、注册并绑定格式化器
  log->info("reset released");       // 此后即为原生 spdlog API
  // 之后再用同名调用会返回已注册的实例
}
```

输出（分辨率 1 ps）：

```
[       12340 ps][tb_top][info] reset released
[       12340 ps][tb_top][warning] retry limit reached   ← 同一时间步，同一时间戳
[       12345 ps][tb_top][info] write complete, addr=0x1ff
```

## 配置

没有 `init()`。只有一个环境变量 `SIMLOG_LEVEL`，它同时指定全局级别与按模块的覆盖规则；其形式会被自动
识别，并在首次调用 `simlog::get()` 时读取一次：

| `SIMLOG_LEVEL` | 含义 |
|---|---|
| `info` | 单独一个级别名（`trace/debug/info/warn/err/critical/off`）表示设置全局级别 |
| `*=debug,dma=info` | `module=level` 规则，以 `,` 分隔，按模块生效 |

默认值为 `info`。非法配置不会被静默采纳：保留默认级别（或忽略该条目），并向 stderr 输出一行提示。

环境变量是**唯一的运行时开关**；不提供 `set_level()` 之类的 API（否则会出现两个配置来源）。级别仅在
创建 logger 时应用一次；要修改已有 logger 的级别，请调用原生的 `logger->set_level()`。格式串固定为
`[%*][%n][%^%l%$] %v`，不能通过环境变量修改。

### 按模块配置级别

```bash
SIMLOG_LEVEL="*=debug,dma=info"    # 全局 debug，模块 "dma" 为 info
SIMLOG_LEVEL="info"                # 仅设置全局级别
```

- 每个条目为 `module=level`；分隔符只有 `,`，且条目按原样解析，因此含空白的条目
  （`"*=debug, dma=info"`）会被拒绝并在 stderr 给出提示，而不会悄悄变成永远无法匹配的模式
- `module` 与传给 `simlog::get()` 的 logger 名字按大小写敏感的方式匹配
- `*` 匹配任意字符序列（含空序列），因此单独的 `*` 是全局规则，`dma*` / `*_tb` 可用于收窄范围
- **匹配最具体的规则生效**：精确名字优先于任何通配符；其次比较字面字符数，多者优先；仍然相同则后写的
  规则优先——因此 `*=debug,dma=info` 与 `dma=info,*=debug` 行为一致
- 没有匹配规则的模块使用全局级别；条目格式不合法时，该模块同样使用全局级别
- 仅在 `simlog::get()` 创建 logger 时生效；已存在的 logger（包括用户自行注册的）绝不会被重新设置级别

## 构建

项目使用 xmake；spdlog（共享库——整个进程只有一个 logger 注册表）、svdpi（header-only，提供 DPI-C 的
`<svdpi.h>`，仅在编译 `src/simlog.cpp` 时需要）与 SystemC（共享库，仅 SystemC 后端需要）均来自
xrepo。

```bash
xmake                          # 默认后端（svdpi）：libsimlog.so，由 include/ + src/ 构建
xmake run example              # 用 VCS 编译并运行 examples/svdpi/tb.sv；没有 vcs 时跳过
xmake run vcs-test             # 在真实仿真器（VCS）中运行 category 2-4；没有 vcs 时跳过
xmake f --backend=systemc -y   # 切换到 SystemC 后端
xmake run example              # SystemC 示例，6 行文档输出
xmake test                     # 单元测试——仅 SystemC 配置
xmake f --backend=svdpi -y     # 切回
```

后端选项即同名的编译期宏：`--backend=svdpi` 为所有依赖 `simlog` 目标的构建目标定义
`SIMLOG_BACKEND_SVDPI`，`--backend=systemc` 则定义 `SIMLOG_BACKEND_SYSTEMC`。不使用 xmake、直接包含
头文件的用户只需定义其中一个宏；使用默认后端时无需任何定义——或者，在 xmake 内直接使用
[`eda` 仓库中的包](#在其他项目中集成-simlog)。

`libsimlog.so` 依赖（NEEDED）`libspdlog.so`：在 xmake 之外使用（例如由仿真器手工加载）时，需确保
共享版 spdlog 可被找到，可借助 `LD_LIBRARY_PATH`。在 xmake 内，`xmake run` 与 `xmake test` 会
自动注入各包的 lib 目录。

## 在其他项目中集成 simlog

simlog 已收录到 [EDA xmake 包仓库](https://github.com/kpskp77/xmake-repo)。注册该仓库并 require 该包，
集成就完成了：

```lua
add_repositories("eda https://github.com/kpskp77/xmake-repo.git")

add_requires("simlog")  -- 默认 SVDPI 后端：头文件 + libsimlog.so + SV 侧的 C ABI
-- add_requires("simlog 1.0.0")                                   -- 固定版本
-- add_requires("simlog", { configs = { backend = "systemc" } })  -- SystemC 后端（header-only）

target("tb")
  set_kind("shared")      -- 由仿真器加载的 DPI-C 库：不能是可执行文件，也不能是静态库
  set_languages("c++17")  -- 头文件的最低要求
  add_files("src/*.cpp")
  add_packages("simlog", { public = true })
```

## 测试

测试分为四个 category。Category 1 与 2 共用同一套场景主体（[`tests/cases.cpp`](tests/cases.cpp)），
使同一份源码在两个后端上校验 API 契约；category 3 与 4 则覆盖 SystemVerilog 侧这一独立接口。

| # | Category | 位置 | 方式 |
|---|---|---|---|
| 1 | SystemC 后端 | [`tests/systemc/tests.cpp`](tests/systemc/tests.cpp) + [`tests/cases.cpp`](tests/cases.cpp) | `xmake test`——15 个场景，每个场景独立进程 |
| 2 | SVDPI，C++ 侧使用 simlog | [`tests/svdpi/cpp/`](tests/svdpi/cpp) + 同一份 `cases.cpp` | `xmake run vcs-test`——共享场景在 C++ 中断言精确日志行，bench 仅负责 `#delay` |
| 3 | SVDPI，SV 侧使用 simlog | [`tests/svdpi/sv/proxy.sv`](tests/svdpi/sv/proxy.sv) | `xmake run vcs-test`——期望行由 runner 提供，与 `simv -l sim.log` 逐行 diff |
| 4 | C++ 与 SV 共用同一 logger | [`tests/svdpi/mixed/`](tests/svdpi/mixed) | `xmake run vcs-test`——diff 日志行并校验为同一实体 |

```bash
xmake f --backend=systemc -y && xmake test    # category 1：15 个场景
xmake test -v                                 # 同时显示通过用例的输出
xmake run tests stamp                         # 直接运行单个场景
xmake f --backend=svdpi -y && xmake run vcs-test   # 在真实仿真器（VCS）中运行 category 2-4
```

各 category 的内部实现（每个场景独立进程的原因、为何不设宿主机侧的 SVDPI harness、各场景的覆盖范围）
属于维护细节，参见 [AGENTS.md](AGENTS.md)（`Test conventions`）与场景主体
[`tests/cases.cpp`](tests/cases.cpp)。
