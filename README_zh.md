# simlog — 为 EDA 仿真中的 spdlog 提供仿真时间戳

[English](README.md) | **中文**

一个极小的 [spdlog](https://github.com/gabime/spdlog) 扩展，在 EDA 仿真中为日志行打印**仿真时间**
戳。时间来自两个编译期后端之一：

| 后端 | 宏 | 仿真时间来源 | 依赖 |
|---|---|---|---|
| **SVDPI**（默认） | `SIMLOG_BACKEND_SVDPI` | DPI 时间 API `svGetTime()` / `svGetTimePrecision()`（IEEE 1800-2023），或在较老仿真器上退回 `vpi_get_time()` / `tf_gettimeprecision()` | spdlog + `libsimlog.so`（编译型后端：`src/simlog.cpp`） |
| SystemC | `SIMLOG_BACKEND_SYSTEMC` | `sc_time_stamp()` × `sc_get_time_resolution()` | spdlog + SystemC（header-only） |

- **没有自定义 logger 类型**：`simlog::get()` 返回原生 `spdlog::logger`，`add_sink` / `set_level` 照常可用

## 快速开始

### SVDPI（默认后端）

#### 在 SystemVerilog 仿真器中的 DPI-C 代码

```cpp
#include <simlog/simlog.h>  // SIMLOG_BACKEND_SVDPI 是默认值：不需要任何宏

extern "C" void tb_step(int cycle) {  // 任意被 SystemVerilog 侧 import 的函数
  auto log = simlog::get("tb_top");   // 首次调用：创建 + 注册 + 挂上格式化器
  log->info("cycle {}", cycle);       // 之后就是原生 spdlog API
}
```

1 ps 仿真精度下输出：

```
[       12340 ps][tb_top][info] cycle 42
```

#### 从 SystemVerilog 使用：`simlog_logger`（`sv/simlog_pkg.sv`）

```systemverilog
import simlog_pkg::*;

module tb;
  initial begin
    // %m 在此处展开，即模块的层次化名字 —— 正是 SIMLOG_LEVEL 模块规则匹配的名字。
    simlog_logger lg = new($sformatf("%m"));
    lg.info("reset released");
    #10;
    lg.warn($sformatf("cnt=%0d", 1));  // SystemVerilog 没有可变参数调用
    lg.flush();                        // $finish 不会运行析构函数
  end
endmodule
```

```
[           0 ps][tb][info] reset released
[       10000 ps][tb][warning] cnt=1
```

`xmake` 把库生成在 `build/<platform>/<arch>/release/libsimlog.so`；连同 SV 源码一起交给仿真器：

```bash
vcs -full64 -sverilog -sv_lib <build-dir>/libsimlog tb.sv   # 或把 .so 当文件参数传入
```

日志行去哪里是后端的一部分：SVDPI 的默认 sink 通过仿真器自己的输出函数（`vpi_printf`）打印，所以
`simv -l sim.log` 里能和 `$display` 的行一起看到它们。而 DPI-C 库自己的 `printf` 不会进 `-l` 文件：
`-l` 镜像的是仿真器的输出流，不是 C 运行时的 stdout。没有 VPI 宿主（普通宿主程序）时，行还是走 stdout。

| SystemVerilog | C ABI（`libsimlog.so`） | 说明 |
|---|---|---|
| `new(name)` | `simlog_open(name)` | 每个 logger 一次；handle *就是* 已注册的 spdlog logger，所以同名两次 open 是同一个 logger |
| `lg.trace/debug/info/warn/error/critical(msg)` | `simlog_log(handle, level, msg)` | `SIMLOG_TRACE` … `SIMLOG_CRITICAL` 就是 spdlog 的级别 0 … 5 |
| `lg.flush()` | `simlog_flush(handle)` | `$finish` 前调用以冲刷缓冲 |

### SystemC (`--backend=systemc`)

```cpp
#include <simlog/simlog.h>

int sc_main(int, char**) {
  auto log = simlog::get("tb_top");  // 首次调用：创建 + 注册 + 挂上格式化器
  log->info("reset released");       // 之后就是原生 spdlog API
  // 之后再取同名会返回已注册的那个实例
}
```

输出（分辨率 1 ps）：

```
[       12340 ps][tb_top][info] reset released
[       12340 ps][tb_top][warning] retry limit reached   ← 同一时间步，同一时间戳
[       12345 ps][tb_top][info] write complete, addr=0x1ff
```

## 配置

没有 `init()`。只有一个环境变量 `SIMLOG_LEVEL`，同时承载全局级别和按模块的覆盖规则；形式自动识别，并
且在第一次 `simlog::get()` 时读取一次：

| `SIMLOG_LEVEL` | 含义 |
|---|---|
| `info` | 单独的级别名（`trace/debug/info/warn/err/critical/off`）设置全局级别 |
| `*=debug,dma=info` | `module=level` 规则，以 `,` 分隔，按模块生效 |

默认值为 `info`。非法值绝不会静默生效：保留默认级别（或忽略该条目），并在 stderr 打印一行提示。

环境变量是**唯一的运行时开关**；没有 `set_level()` 之类的 API（那会有两个真相来源）。级别只在创建
logger 时应用一次；要修改已存在 logger 的级别，请调用原生 `logger->set_level()`。格式串固定为
`[%*][%n][%^%l%$] %v`，不能通过环境变量修改。

### 按模块的级别

```bash
SIMLOG_LEVEL="*=debug,dma=info"    # 全部 debug，除了模块 "dma" 为 info
SIMLOG_LEVEL="info"                # 就是全局级别
```

- 每个条目是 `module=level`；唯一分隔符是 `,`，条目按原样使用，因此含空白的条目
  （`"*=debug, dma=info"`）会被拒绝并在 stderr 提示，而不是悄悄变成一个永远匹配不上的模式
- `module` 与传给 `simlog::get()` 的 logger 名字做区分大小写的匹配
- `*` 匹配任意字符序列（包括空），所以单独的 `*` 是全局规则，`dma*` / `*_tb` 用来收窄
- **最具体的匹配规则获胜**：精确名字优先于任何通配符，其次字面字符多者优先，再相同则后写的规则优先
  —— 所以 `*=debug,dma=info` 与 `dma=info,*=debug` 行为一致
- 没有匹配规则的模块用全局级别；条目不合法时该模块同样用全局级别
- 只影响 `simlog::get()` 创建 logger 的那一刻；已存在的 logger（包括用户自己注册的）绝不被重设级别

## 构建

项目使用 xmake；spdlog（共享库——每个进程只有一个 logger 注册表）、svdpi（header-only，即 DPI-C 的
`<svdpi.h>`，只有编译 `src/simlog.cpp` 时才需要）和 SystemC（共享库，仅 SystemC 后端）都来自 xrepo。

```bash
xmake                          # 默认后端（svdpi）：libsimlog.so，由 include/ + src/ 构建
xmake run example              # 用 VCS 编译并运行 examples/svdpi/tb.sv；没有 vcs 时跳过
xmake run vcs-test             # 在真实仿真器（VCS）中运行 category 2-4；没有 vcs 时跳过
xmake f --backend=systemc -y   # 切到 SystemC 后端
xmake run example              # SystemC 示例，6 行文档输出
xmake test                     # 单元测试 —— 仅 SystemC 配置
xmake f --backend=svdpi -y     # 切回
```

后端选项就是同名编译期宏：`--backend=svdpi` 为所有依赖 `simlog` target 的目标定义
`SIMLOG_BACKEND_SVDPI`，`--backend=systemc` 定义 `SIMLOG_BACKEND_SYSTEMC`。不用 xmake 而直接使用头文
件的使用方，只需定义其中一个宏，默认后端则什么都不用定义。

`libsimlog.so` 依赖 `libspdlog.so`（NEEDED）：在 xmake 之外（比如仿真器手工加载它）使用时，要确保共
享 spdlog 可被找到，例如通过 `LD_LIBRARY_PATH`。在 xmake 之内，`xmake run` 和 `xmake test` 会自动注
入包的 lib 目录。

## 测试

测试分为四个 category。Category 1 和 2 运行同一套场景主体（[`tests/cases.cpp`](tests/cases.cpp)），
因此同一份源码在两个后端上都校验了 API 契约；category 3 和 4 存在的原因是与 SystemVerilog 侧是另一
个面。

| # | Category | 位置 | 方式 |
|---|---|---|---|
| 1 | SystemC 后端 | [`tests/systemc/tests.cpp`](tests/systemc/tests.cpp) + [`tests/cases.cpp`](tests/cases.cpp) | `xmake test` —— 15 个场景，各自独立进程 |
| 2 | SVDPI，C++ 侧使用 simlog | [`tests/svdpi/cpp/`](tests/svdpi/cpp) + 同一份 `cases.cpp` | `xmake run vcs-test` —— 共享场景在 C++ 中断言精确日志行，bench 只负责 `#delay` |
| 3 | SVDPI，SV 侧使用 simlog | [`tests/svdpi/sv/proxy.sv`](tests/svdpi/sv/proxy.sv) | `xmake run vcs-test` —— 期望行交给 runner，与 `simv -l sim.log` 逐行 diff |
| 4 | C++ 与 SV 混用同一 logger | [`tests/svdpi/mixed/`](tests/svdpi/mixed) | `xmake run vcs-test` —— diff 日志行并检查是同一实体 |

```bash
xmake f --backend=systemc -y && xmake test    # category 1：15 个场景
xmake test -v                                 # 同时显示通过用例的输出
xmake run tests stamp                         # 直接运行单个场景
xmake f --backend=svdpi -y && xmake run vcs-test   # 真实仿真器（VCS）中的 category 2-4
```

category 的内部实现方式——为什么每个场景独立进程、为什么没有 host 侧的 SVDPI harness、每个场景覆
盖什么——属于维护细节，见 [AGENTS.md](AGENTS.md)（`Test conventions`）和场景主体
[`tests/cases.cpp`](tests/cases.cpp)。
