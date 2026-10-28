# simlog — 为 EDA 仿真中的 spdlog 提供仿真时间戳

[English](README.md) | **中文**

一个极小的 [spdlog](https://github.com/gabime/spdlog) 扩展，在 EDA 仿真中为日志行打印**仿真时间**
戳。时间来自两个编译期后端之一：

| 后端 | 宏 | 仿真时间来源 | 依赖 |
|---|---|---|---|
| **SVDPI**（默认） | `SIMLOG_BACKEND_SVDPI` | DPI 时间 API `svGetTime()` / `svGetTimePrecision()`（IEEE 1800-2023），或在较老仿真器上退回 `vpi_get_time()` / `tf_gettimeprecision()` | spdlog + `libsimlog.so`（编译型后端：`src/simlog.cpp`） |
| SystemC | `SIMLOG_BACKEND_SYSTEMC` | `sc_time_stamp()` × `sc_get_time_resolution()` | spdlog + SystemC（header-only） |

- **没有自定义 logger 类型**：`simlog::get()` 返回原生 `spdlog::logger`，`add_sink` / `set_level`
  照常可用
- **你的编译单元里没有任何仿真器相关代码**：`<svdpi.h>` 和 VPI/PLI 声明都在编译型 SVDPI 后端里，
  不在头文件中
- 格式串中的 `%*` 就是仿真时间（使用 spdlog 的 `custom_flag_formatter`，不需要任何 `init()` 调用）
- 两个后端只差“如何读取时间”：`%*`、格式串、级别配置和 `get()` 都是共享代码

## 快速开始

### SVDPI（默认后端）— 在 SystemVerilog 仿真器中的 DPI-C 代码

```cpp
#include <simlog/simlog.h>  // SIMLOG_BACKEND_SVDPI 是默认值：不需要任何宏

extern "C" void tb_step(int cycle) {  // 任意被 SystemVerilog 侧 import 的函数
  auto log = simlog::get("tb_top");   // 首次调用：创建 + 注册 + 挂上格式化器
  log->info("cycle {}", cycle);       // 之后就是原生 spdlog API
}
```

照常把它编进 DPI-C 共享库（`-fPIC -shared`，加上 spdlog 和 `svdpi` 包的 include 路径，无需链接任何
仿真器库）**再加上 `src/simlog.cpp`**，或者直接链接 `libsimlog.so`。仿真器的时间函数在该库中是弱引
用，仿真器加载库时才解析（见 [SVDPI 后端](#svdpi-后端)）。1 ps 仿真精度下输出：

```
[       12340 ps][tb_top][info] cycle 42
```

#### 从 SystemVerilog 使用：`simlog_logger`（`sv/simlog_pkg.sv`）

测试平台不应该为每次打日志写一个 DPI import，因此 SV 侧为每个 logger 提供一个对象，底层是
[`src/simlog.cpp`](src/simlog.cpp) 中三个 `extern "C"` 函数——与 SVDPI 后端在同一个文件里。因此没有
任何 C++ 代码的 testbench 只需加载 `libsimlog.so`：

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

该库不链接任何仿真器库：其时间函数保持未解析的弱引用，由仿真器在加载时绑定，和手写 DPI-C 文件完全
一样。**如果你的 DPI-C 库已经包含 simlog 的 C++ 代码，请把 `src/simlog.cpp` 编进那个库，而不是再加
载第二个库** —— 每个共享对象各有自己的 logger 注册表、默认 sink 和 `SIMLOG_LEVEL` 快照，两个库就会
在同名 logger 上出现两份彼此独立的状态。

| SystemVerilog | C ABI（`libsimlog.so`） | 说明 |
|---|---|---|
| `new(name)` | `simlog_open(name)` | 每个 logger 一次；handle *就是* 已注册的 spdlog logger，所以同名两次 open 是同一个 logger |
| `lg.trace/debug/info/warn/error/critical(msg)` | `simlog_log(handle, level, msg)` | `SIMLOG_TRACE` … `SIMLOG_CRITICAL` 就是 spdlog 的级别 0 … 5 |
| `lg.flush()` | `simlog_flush(handle)` | `$finish` 前调用以冲刷缓冲 |

- **消息是已经格式化好的字符串**：SystemVerilog 既没有可变参数函数也没有可变参数宏，所以
  `$sformatf` 在调用点执行；时间戳仍由后端读取，SV 侧永远不传 `$time`/`$realtime`（那会引入第二个时间
  源和一次取整）
- **名字是显式给出的**：在模块里 `new($sformatf("%m"))` 就是层次化名字；从 class 里打日志的代码自己
  传名字。package 不猜名字：`svGetScope()` 需要 `context` import，而且返回的是 import 被*声明*的
  作用域——也就是 package
- **错误参数只报告、不生效**：空名字、空 handle、或 0 … 5 之外的级别，每个进程在 stderr 打印一次提
  示，并丢弃该行，与非法 `SIMLOG_LEVEL` 同一规则
- **C ABI 只是门面**：它调用 `simlog::get()`、通过原生 logger 打日志，本身不添加任何策略（不重设级
  别、不设格式串、不引入第二个时间源）。这五个 `extern "C"` 函数就是 ABI（`SIMLOG_EXPORT`，默认可见
  性）；头文件里的 inline 函数仍以弱符号导出，链接 `libsimlog.so` 的 DPI-C 库正是靠这一点共享它的状态

### SystemC — 测试平台代码（`--backend=systemc`）

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

### 示例

每个后端都带一个最小示例；`xmake run example` 运行当前配置的那个。SVDPI 示例是纯 SystemVerilog
testbench：两个模块各自用 `$sformatf("%m")` 创建自己的 `simlog_logger`，因此时间戳和 logger 名字就是
真实 testbench 会看到的样子（DUT 那行的名字是 `tb.u_dut`，正是 `SIMLOG_LEVEL` 模块规则能匹配的名字）：

```
$ xmake f --backend=svdpi -y && xmake run example
[           0 ps][tb][info] sim start
[       10000 ps][tb][info] cnt=1
[       15000 ps][tb.u_dut][warning] unexpected burst
[       20000 ps][tb][info] cnt=2
[       30000 ps][tb][info] cnt=3
[       30000 ps][tb][error] sim end

$ xmake f --backend=systemc -y && xmake run example
[           0 ps][tb][warning] sim start, resolution: 1 ps
[       10000 ps][tb][info] cnt=1, wake at 10 ns
[       20000 ps][tb][info] cnt=2, wake at 20 ns
[       30000 ps][tb][info] cnt=3, wake at 30 ns
[       40000 ps][tb][info] cnt=4, wake at 40 ns
[       40000 ps][tb][error] sim end
```

[`examples/svdpi/run.sh`](examples/svdpi/run.sh) 用 VCS 编译并运行 `tb.sv`，逐行比对上面的输出，没有
`vcs` 时自动跳过；[`examples/systemc/tb.cpp`](examples/systemc/tb.cpp) 是 SystemC 的 `sc_main`
testbench。

## 时间源：每个后端只有一个

时间戳永远是**tick 数 × 分辨率倍数**，单位只打印纯符号。tick 来自哪个时钟是后端自己的事。

### SVDPI 后端

以空作用域向仿真器询问全局仿真时间（即“整个仿真”），并且要的是**整数 tick**，从不要 double。有两条
路径，按顺序尝试：

| 路径 | 取值 | 单位 |
|---|---|---|
| DPI 时间 API（IEEE 1800-2023） | `svGetTime(NULL, &t)` → `high`/`low` 中的 64 位 tick，`t.type = sv_sim_time` | `svGetTimePrecision(NULL, &exp)` → 精度指数 |
| VPI/PLI | `vpi_get_time(NULL, &t)`，`vpiSimTime` → 同样的 tick | `tf_gettimeprecision()` → 同样的指数 |

路径 1 直到 IEEE 1800-2023 才进入 DPI 附录，因此更早发布的仿真器并不实现它——例如 VCS O-2018.09
的 `<svdpi.h>` 里连函数声明都没有——写死引用会链接失败。所以这两个符号声明为**弱引用**：支持 2023
的仿真器在加载时绑定自己的定义，走路径 1；较老的仿真器让指针为空，改由路径 2 回答。无论哪条路径，每
个进程都只有一个时间源，每次打日志时读取，返回值是以仿真精度为单位的整数 tick（`initial` 开始前为
0）。

路径 2 是带空对象的纯取值查询：不查找 VPI handle、不注册 VPI 回调，而且 `sv_sim_time` 与
`vpiSimTime` 是同一种表示，所以两条路径给出的 tick 完全相同。在 VCS 上 `tf_gettimeprecision()` 报告的
是全局（最细）仿真精度，这正是 DPI 空作用域的含义——多 timescale 的 testbench 也是如此。

不是单一基本单位的精度会被**折算进数字**，与 SystemC 后端的分辨率倍数规则一致，所以单位列永远只是
`fs/ps/ns/us/ms/s`：

```
精度 1 ps    (exp -12) → {1, "ps"}     [       12340 ps]
精度 100 fs  (exp -13) → {100, "fs"}   [     1234000 fs]   (12340 tick × 100 fs = 1.234 ns)
精度 10 ps   (exp -11) → {10, "ps"}    [      123400 ps]
精度 1 ns    (exp -9)  → {1, "ns"}     [       12340 ns]
```

标准 `<svdpi.h>` 来自 header-only 的 [`svdpi`](https://github.com/kpskp77/svdpi) 包
（`add_requires("svdpi")`），VPI/PLI 函数则由 `src/simlog.cpp` 自己声明。两者都不在 `simlog.h` 里，
所以 DPI-C 编译单元根本看不到任何仿真器声明，也不需要为 `<vpi_user.h>` / `<veriuser.h>` 处理
include 顺序。两条 API 都问不到时间的主机（没有仿真在跑）会得到全 0 的时间戳，并在 stderr 打印一次提
示。

### SystemC 后端

时间源就是 SystemC，**不需要任何初始化调用**：

- 值：`sc_time_stamp().value()`（整数 tick，elaboration 阶段为 0）× 分辨率倍数
- 单位：分辨率的基准单位，只打印纯符号（`fs/ps/ns/us/ms/s`）

带倍数的分辨率（例如 100 fs per tick）不会把倍数放进单位，而是**折算进 tick 数**，因此所有运算都是
整数、没有浮点误差，`sc_set_time_resolution()` 立即生效：

```
分辨率 1 ps   → 1 ps per tick   → [       12340 ps]   (= 12.34 ns)
分辨率 100 fs → 100 fs per tick → [     1234000 fs]   (12340 tick × 100 fs = 1.234 ns)
分辨率 10 ns  → 10 ns per tick  → [      123400 ns]   (12340 tick × 10 ns = 123.4 us)
```

没有时间源注入、没有手工推进时间、也没有显示单位配置。

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

## 设计

**只有一个公开 C++ API**：`simlog::get()`。其余一切（读取时间 / 分辨率倍数与单位 / `%*` 标志 /
格式化器 / 环境解析 / 默认 sink）都在 `simlog::detail` 里。

唯一的补充是为 DPI 边界的另一侧准备的，而且它不是第二个公开头文件：三个 `extern "C"` 函数——
`simlog_open`、`simlog_log`、`simlog_flush`——放在 `src/simlog.cpp`（声明与定义在同一文件，没有头文
件）并编进 `libsimlog.so`，这样 SystemVerilog 无需手写 DPI import 就能拿到 logger；
`sv/simlog_pkg.sv` 把它们包装成 `simlog_logger`。`include/simlog/` 依然只有一个文件。

**只编译进一个后端。** 两个后端是互斥的，不是叠加的：各自一个时间源，用 `SIMLOG_BACKEND_SVDPI`
（默认）或 `SIMLOG_BACKEND_SYSTEMC` 选择；两个都定义是编译错误。它们只在两个函数处与库的其余部分
相接，所以别处不需要知道当前是哪个后端：

```
simlog::get(name)          ← 惰性工厂：创建原生 logger + 挂格式化器 + 注册
  └─ detail::sim_time_flag (%*)          ← custom_flag_formatter：每行日志读一次时钟
       └─ detail::now_ticks() + detail::resolution_unit()   ← 唯一与后端相关的两个函数
            ├─ SVDPI:   svGetTime / svGetTimePrecision       (IEEE 1800-2023)
            │           vpi_get_time / tf_gettimeprecision   (较老的仿真器)
            └─ SystemC: sc_time_stamp / sc_get_time_resolution
```

**单线程模型**：EDA 仿真只在主线程上运行，所以库中没有锁、没有原子操作——`simlog::get()` 的注册路径
没有 mutex，默认 sink 是 `_st` 单线程版本。时间戳用整数运算（tick × 倍数），两个后端都没有浮点误差。

## 已知限制

- **只支持同步 logger**：异步 logger 在后台线程调用 `format()`，时间戳会变成输出时刻而不是写日志时刻
- **后端是编译期选择**，每个程序只有一个（一个 DPI-C 共享库要么是 SystemC 构建，要么是 DPI 构建）
- **SVDPI：早于 IEEE 1800-2023 的仿真器**由 VPI/PLI 回退路径服务，这要求代码确实跑在仿真器里（裸的
  主机程序两条 API 都没有，时间戳为 0）。它返回的 tick 和精度指数与 DPI API 相同；已用 VCS
  O-2018.09-SP2 通过 `xmake run vcs-test` 验证
- **SystemVerilog 侧先格式化再打日志**：SV 没有可变参数调用，`$sformatf` 在调用点执行，所以被
  `SIMLOG_LEVEL` 过滤掉的行其实已经付出格式化的代价
- **一次调用一行**：消息中内嵌的换行按原样打印，只有第一行带时间戳
- **SVDPI 是编译型后端**：与 SystemC 不同，它不是 header-only——要么链接 `libsimlog.so`，要么把
  `src/simlog.cpp` 编进打日志的 DPI-C 库
- **每个共享对象一份 simlog**：`libsimlog.so` 自带 logger 注册表，因此既加载它、又加载一个也含 simlog
  C++ 代码的 DPI-C 库，会在同名 logger 上得到两份彼此独立的状态
- **没有 UVM 适配**（暂不在范围内）：`uvm_info` / `uvm_error` 不会自己进入 simlog，需要
  `uvm_report_server` 子类
- 直接用 `spdlog::get()` 创建、绕过 `simlog::get()` 的 logger 没有仿真时间戳（符合预期）

## 构建

项目使用 xmake；spdlog（header-only）、svdpi（header-only，即 DPI-C 的 `<svdpi.h>`，只有编译
`src/simlog.cpp` 时才需要）和 SystemC（共享库，仅 SystemC 后端）都来自 xrepo。

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

两个后端共用一个 `simlog` target——`include/` 和 `src/` 都是它的核心代码——其 kind 随后端变化：
SystemC 为 `headeronly`，SVDPI 为 `shared`（头文件加 `src/simlog.cpp`，链接为 `libsimlog.so`）。
所有使用该 API 的目标都依赖它并链接 `libsimlog.so`；DPI-C 测试库额外加上 `$ORIGIN` rpath，让仿真器
能在它们旁边找到该库。`src/simlog.cpp` 只被编译一次，就在 `simlog` target 里。

依赖要求：两个后端都需要 C++17 和 spdlog。SVDPI 后端额外需要 header-only 的 `svdpi` 包来编译
`src/simlog.cpp`，并链接 `libsimlog.so`（或把该文件编进来）。SystemC 后端需要 SystemC。
`xmake run example`（SVDPI 配置）和 `xmake run vcs-test` 还需要已安装的 VCS（`PATH` 上有 `vcs`）；
没有时两者都会跳过。

## 测试

测试分为四个 category。Category 1 和 2 运行同一套场景主体（[`tests/cases.cpp`](tests/cases.cpp)），
因此同一份源码在两个后端上都校验了 API 契约；category 3 和 4 存在的原因是与 SystemVerilog 侧是另一
个面。

| # | Category | 位置 | 方式 |
|---|---|---|---|
| 1 | SystemC 后端 | [`tests/systemc/tests.cpp`](tests/systemc/tests.cpp) + [`tests/cases.cpp`](tests/cases.cpp) | `xmake test` —— 15 个场景，各自独立进程 |
| 2 | SVDPI，C++ 侧使用 simlog | [`tests/svdpi/cpp/`](tests/svdpi/cpp) + 同一份 `cases.cpp` | `xmake run vcs-test` —— 共享场景在 C++ 中断言精确日志行，bench 只负责 `#delay` |
| 3 | SVDPI，SV 侧使用 simlog | [`tests/svdpi/sv/proxy.sv`](tests/svdpi/sv/proxy.sv) | `xmake run vcs-test` —— 期望行交给 runner 后逐行 diff |
| 4 | C++ 与 SV 混用同一 logger | [`tests/svdpi/mixed/`](tests/svdpi/mixed) | `xmake run vcs-test` —— diff 日志行并检查是同一实体 |

```bash
xmake f --backend=systemc -y && xmake test    # category 1：15 个场景
xmake test -v                                 # 同时显示通过用例的输出
xmake run tests stamp                         # 直接运行单个场景
xmake f --backend=svdpi -y   && xmake         # 构建 libsimlog.so
xmake run example                             # VCS 中的 SV 示例（没有 vcs 时跳过）
xmake run vcs-test                            # 真实仿真器（VCS）中的 category 2-4
```

- **Category 1 — SystemC**（朴素断言，无测试框架）。每个场景独立进程，因为库只读一次环境变量、
  SystemC 每次运行也只接受一次时间分辨率。只有 `resolution-100fs` 和 `resolution-10ns` 是
  SystemC 专属：SVDPI 的分辨率来自编译期的 timescale。
- **Category 2 — SVDPI，C++ 侧使用 simlog**。没有 host 侧 harness：DPI-C 无法推进仿真时间，所以
  `tb_cpp.sv` 只施加 `simlog_test_step()` 要求的 `#delay`，检查本身就是共享的 `cases.cpp` 主体。
  runner 按它编译所用的 timescale 传入期望的时间基准（`+scale`/`+unit`），因此后端答错会失败，而不
  会“自己和自己一致”。完整的共享套件以 1 ps 运行；`format` 和 `stamp` 另外通过
  `-override_timescale` 在 100 fs / 1 ns / 1 fs 下运行。
- **Category 3 — SVDPI，SystemVerilog 侧使用 simlog**。bench 只用 `simlog_logger`，从不 import C
  ABI：每个模块一个 logger、`$sformatf("%m")` 作为名字、每个级别一个方法、`$finish` 前显式 flush。
  runner 逐行比对精确日志，包括花括号/百分号作为消息文本原样保留，以及非法名字/handle 的提示。
- **Category 4 — C++ 与 SV 混用，同一个 logger 实体**。mixed 库和其他使用方一样链接
  `libsimlog.so` 并加 `$ORIGIN`；run.sh 只加载 mixed 库。因此 C++ 里的 `simlog::get()` 和
  `simlog_logger` 背后的 `simlog_open()` 解析到同一个 logger（由 `mixed_same_entity()` 断言），
  diff 出的日志行显示两侧在同一时间戳上交错、遵守同一份 `SIMLOG_LEVEL`。
- **为什么没有 fake-host 的 SVDPI harness**：它的整个接口面就是仿真器——DPI/VPI 时间函数、被加载
  的 DPI-C 库、导出的 C ABI、SystemVerilog package——所以 category 2-4 在它真正运行的地方验证。这也
  是验证 VCS O-2018.09-SP2（根本没有 `svGetTime`）上 VPI/PLI 回退路径的依据。`PATH` 上没有 `vcs` 时
  `xmake run example` 与 `xmake run vcs-test` 都会报告跳过并以 0 退出，此时能检查的只有
  `nm -D libsimlog.so`。

### 各场景覆盖的内容

Category 1 和 2 运行 [`tests/cases.cpp`](tests/cases.cpp) 中 13 个共享场景，外加两个 SystemC 专属的
分辨率场景：

- `format`、`stamp` —— `%*` 渲染（右对齐 12 列时间戳、格式串布局、标志克隆、elaboration 阶段为 0）
  以及 `simlog::get()` 契约（原生 logger、注册、共享默认 sink、逐次调用读时间、原生级别过滤）
- `resolution-100fs`、`resolution-10ns` —— SystemC 专属：倍数折算进数字（`12340` tick →
  `1234000 fs` / `123400 ns`）
- `env-default`、`env-warn`、`env-off`、`env-invalid` —— `SIMLOG_LEVEL` 设置的全局级别（未设置 →
  `info`、单独级别名、`off`、非法值 → 提示 + `info`）
- `env-modules`、`env-modules-wildcard`、`env-modules-global`、`env-modules-order`、
  `env-modules-invalid` —— `module=level` 规则：按原样解析的逗号分隔条目、`*` 匹配与优先级、与顺序
  无关、非法条目报告后忽略
- `registry`、`parse-level` —— 预先注册的 logger 原样返回（级别、格式串、sink 不变）；级别名与模式解
  析、进程级默认 sink

Category 3（[`tests/svdpi/sv/proxy.sv`](tests/svdpi/sv/proxy.sv)）：`levels`（每个级别一个方法，默认
`info` 过滤 trace/debug，`$sformatf("%m")` 作为模块名）、`format`（花括号和百分号作为消息文本原样保
留）、`modules`（同一时间步上的两个 logger 共享时间戳）、`env`（`SIMLOG_LEVEL=tb=warn` 作用于 SV 侧
logger）、`empty-name`（提示、丢弃该行、下一个 logger 仍可工作）。

Category 4（[`tests/svdpi/mixed/`](tests/svdpi/mixed)）：`order` 与 `sv-first`（两种创建顺序，最终是同
一实体、同一时间戳）、`level`（同一份 `SIMLOG_LEVEL` 同时过滤两侧）。

## 项目结构

```
include/simlog/simlog.h  # API：后端选择 / %* 标志 / 级别 / get()
                         # （SystemC 配置下整个后端也在这里）
src/simlog.cpp           # SVDPI 后端 + SV 侧的 extern "C" ABI → libsimlog.so
                         # （仅 SVDPI 配置 —— SystemC 不需要源文件）
sv/simlog_pkg.sv         # SystemVerilog 侧：simlog_logger（每个 logger 一个对象）
examples/svdpi/tb.sv     # SV 示例：使用 sv/simlog_pkg.sv 的纯 SystemVerilog bench
examples/svdpi/run.sh    # 用 VCS 编译并运行它（没有 vcs 时跳过）
examples/systemc/tb.cpp  # SystemC 集成示例（sc_main）
tests/cases.h            # 共享场景契约：分阶段执行的场景主体 + 期望时间戳辅助函数
tests/cases.cpp          # category 1+2：13 个与后端无关的场景
tests/systemc/tests.cpp  # category 1：SystemC host（sc_main）+ 两个分辨率场景
tests/svdpi/cpp/         # category 2：SVDPI，C++ 侧使用 simlog（阶段驱动 + 只做 #delay 的 bench）
tests/svdpi/sv/          # category 3：SVDPI，通过 sv/simlog_pkg.sv 使用 simlog
tests/svdpi/mixed/       # category 4：C++ 与 SV 通过一个 logger 实体共用（一个共享对象）
tests/svdpi/run.sh       # category 2-4：按 timescale 编译，每个场景一个进程
tests/check.h            # 断言宏 + stdout/stderr 捕获辅助
xmake.lua                # 构建脚本：后端选项、单个 simlog target（kind 随后端）、测试
```
