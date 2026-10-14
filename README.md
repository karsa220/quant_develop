# 低延迟期货行情数据链路（C++17）

C++17 零依赖实现的低延迟行情链路：**行情采集 → 网关编码 → 共享内存零拷贝 → 跨进程无锁消费 → 策略信号**。
面向量化私募 / 高频交易（HFT）C++ 岗，覆盖真实生产链路的关键模块：mmap 共享内存、无锁环形队列、timerfd 调度、二进制协议、性能基准。

- **零第三方依赖**：仅 C++17 标准库 + 平台 API（Linux `shm_open`/`mmap`/`timerfd`；Windows `CreateFileMapping`/`WaitableTimer`）。
- **跨平台**：同一份代码在 Windows（zig 内置 clang 验证）与 Linux 编译运行。

---

## 架构

```
行情源(tick) ─► FuturesFeed(合成) ─► CtpLikeCodec(编码) ─► SpscRingBuffer.tryPublish()
                                                          │
                                           ┌──────────────┴──────────────┐
                                           │ 命名共享内存 mmap /dev/shm   │  ← 跨进程零拷贝
                                           │ 无锁 SPSC 环形缓冲          │
                                           └──────────────┬──────────────┘
                                                          │
                                          SpscRingBuffer.tryConsume() ─► CtpLikeCodec(解码)
                                                          │
                                                          ▼
                                          TickConsumer: 重建盘口 + 信号 + 延迟统计

旁路: PeriodicTimer(timerfd 09:30开盘/15:00收盘 + 心跳) · systemd 保活 · cron 收盘快照
```

核心：生产者只写 `head`、消费者只写 `tail`，各自独占缓存行 → 天然无锁、无伪共享；数据驻留共享内存，跨进程直接读内存，不走 socket、不拷贝。

---

## 核心实现：怎么做

### 1. 无锁 SPSC 环形缓冲（消息主干）
- **问题**：`mutex` 在线程/进程间串行化，吞吐受限、延迟抖动。
- **做法**：单生产者写 `head`、单消费者写 `tail`，两个原子指针各占一条缓存行；发布/消费只做模运算定位槽位，**O(1)**；内存序用 `acquire/release`（非 `seq_cst`），把 cache 同步开销压到最低。
- **收益**：较 `std::mutex + deque` 吞吐 **数倍提升**（本机多次实测 **1.3×~8.2×**，取决于线程调度，互斥队列争用大时掉到 1M 档）；更稳的硬指标是 lock-free 的 **p50 稳定 ~200ns**，而互斥队列 **p50 数十 µs、p99 随争用暴涨**——这点任意复现都成立。

### 2. 共享内存 mmap 零拷贝（跨进程）
- **问题**：socket/管道跨进程要内核拷贝，延迟高。
- **做法**：`shm_open`+`mmap` 把同一段环形缓冲映射到网关与策略两进程地址空间，数据只写一次、策略侧直接读内存；满则精确计数丢弃（背压可控）。
- **收益**：两独立进程间 **13,236,632 ticks 发布 = 消费，零丢失**。

### 3. 定长二进制协议（线路编码）
- **做法**：`#pragma pack(1)` 的 `magic+type+len+payload+checksum` 帧，固定槽位 **O(1)** 编解码。
- **收益**：编解码 **31.8M ticks/s（≈30ns/tick）**，远快于 JSON/文本。

### 4. 防伪共享 + 热路径零分配
- `head`/`tail`/`lost` 各 `alignas(64)` 独占缓存行，消除 false sharing；
- 预分配缓冲循环复用，撮合/转发路径不 `new`/`malloc`，延迟稳定不抖。

### 5. timerfd 定时调度（Linux 侧）
- **做法**：`CLOCK_MONOTONIC` 周期心跳 + `CLOCK_REALTIME` 绝对定时（09:30 开盘 / 15:00 收盘），内核唤醒不 busy-wait；配套 systemd 保活 + cron 收盘快照。

### 6. 百分位延迟度量
- `LatencyRecorder` 记录 p50/p90/p99/p99.9（不看平均），端到端 tick→signal **p50 200ns / p99 1.35ms**。

### 7. 网络层：C++ 网络编程 / 抓包 / 多协议数据接口
真实行情系统里，行情从网卡进来，要经「网络收包 → 解析 → 入环 → 对外暴露」整条链路。本项目 `net/` 模块把这一层也做出来：

- **行情网络采集（`MdReceiver`）**：UDP（组播/单播）/ TCP 接收「内部线格式消息」(`MsgHeader+TickPayload`)，解析为 `MarketTick` 后发布到 `SpscRingBuffer` 或触发回调；这正是期货公司网关「网络收包 → 入环」的入口。
- **链路层抓包（`PcapCapture`）**：Linux `AF_PACKET(SOCK_RAW)` / Windows 原始套接字 + `SIO_RCVALL` 抓以太网/IPv4/UDP 报文，逐层解析出行情载荷；用于延迟核对、原始报文留痕、合规。无权限时优雅降级（解析逻辑由单测独立覆盖）。
- **多协议数据接口（`MdServer`）**：同一份行情向不同消费方提供三种接口——
  - **TCP 二进制流**（`:9001`，面向低延迟 C++ 客户端，直接发线格式消息）；
  - **HTTP REST/JSON**（`:8080`，`GET /snapshot`、`GET /book?sym=IF`，面向研究/Python）；
  - **UDP 组播**（`:9002`→`239.1.1.1`，面向多订阅者 fan-out，交易所行情经典模型）。

网络层与采集层、共享内存层解耦：都从同一份 `MarketTick` 派生，互不阻塞。

### 8. 风控模块（`RiskControl`）
- **做法**：行情进入策略前做合规校验——价格区间、行情时效（拒绝来自未来的重放）、净持仓限额（`maxLong`/`maxShort`）、下单频率（令牌桶限流），以及全局**熔断开关（kill-switch）**；热路径零分配、零锁（per-instrument 状态单线程访问，仅 `halted_` 跨线程用 `acquire/release`），缓存行隔离。
- **收益**：坏点/越界/乌龙指被拦截在策略执行前；监控越界时可联动 `triggerHalt()` 拉闸，系统快速进入安全态。

### 9. 监控告警与交易日志（`Monitor`）
- **做法**：分级日志（INFO/WARN/ERROR/ALERT，spinlock 保护低开销）；指标计数器（发布/消费/丢弃/信号/风控拒绝/告警）；端到端延迟直方图（p50/p90/p99/p99.9）；**看门狗线程**周期自检——丢弃率越界 / p99 延迟越界 / 行情卡顿（无心跳）即告警，严重越界可联动熔断；`telemetryJson()` 供 `/metrics` 对外暴露（接 Grafana 思路）。
- **收益**：线上问题可快速定位（延迟分位、丢弃率、卡顿一目了然），并支持自动恢复（熔断后 `resume()`）。

### 10. 无锁对象池 / 内存池（`ObjectPool`，模板）
- **做法**：热路径避免反复 `new`/`delete`（系统调用 + 锁 + 碎片），改为从预分配池 `acquire`/`release`；Treiber 栈（无锁 LIFO 自由表），多生产者多消费者安全。
- **收益**：降低尾延迟与缓存抖动，面试可直接讲「为什么高频系统要对象池」。

### 11. I/O 多路复用（epoll / WSAPoll，`IoMultiplexer`）
- **做法**：Linux 生产路径用 `epoll`（O(1) 就绪通知，支撑上万并发连接），Windows 用 `WSAPoll` 提供等价能力；`MdServer` 监听口注册到多路复用器，`accept` 前先 `wait()` 确认可读，纳入 reactor 事件循环，避免阻塞。
- **收益**：单线程即可驱动大量连接，是「高并发网络 + 零拷贝」的基础设施。

---

## JD 能力对照（量化交易系统 C++ 开发实习）

| JD 要求 / 职责 | 本项目对应实现 |
|---|---|
| 行情接入（C++ 实现） | `MdReceiver`（UDP/TCP 收包）、`PcapCapture`（链路层抓包）、`FuturesFeed`（合成行情源） |
| 策略执行 | `TickConsumer`：解码 → 重建盘口 → 反转信号生成 → 下单前 `RiskControl.checkOrder` |
| 风控模块 | `RiskControl`：价格/时效/持仓/频率校验 + 熔断 kill-switch |
| 性能分析与瓶颈排查、降低延迟 | `LatencyRecorder` 分位延迟、`bench_main` 对照（无锁 vs 互斥）、cache-line 隔离、`Monitor` 看门狗定位卡顿/丢弃 |
| 优化内存/CPU/网络 IO | 无锁 SPSC（CPU）、mmap 零拷贝（内存/网络 IO）、对象池（内存）、epoll（网络 IO） |
| 单元测试 / 集成测试 | `test_system`（985k 断言）、`test_net`（23 断言）、`test_integration`（端到端 95 断言：零丢失/背压/看门狗/熔断/对象池） |
| 交易日志、监控告警、快速定位 | `Monitor`：分级日志 + 指标 + 延迟直方图 + 看门狗告警 + `telemetryJson` |
| C++11/14/17 特性 | lambda、移动语义、`std::atomic`、RAII、`std::chrono`、`std::array`（模板） |
| STL 与底层原理 / 多线程 | `std::mutex`、`condition_variable`、`atomic`（acquire/release）、无锁队列/对象池 |
| Linux / gdb·valgrind·perf | timerfd、mmap、`scripts/`（systemd/cron）；可在 Linux 下 `perf record ./bench_main` 定位热点 |
| TCP/IP·UDP·socket·零拷贝·epoll | `net/` 全模块 + `IoMultiplexer`（epoll/WSAPoll）+ mmap 零拷贝 |
| 算法与数据结构 | 环形缓冲（O(1)）、Treiber 栈（无锁）、令牌桶（限流）、定长协议解析 |
| 加分：量化/HFT/做市经验 | 期货行情链路、低延迟、跨进程零丢失、熔断/监控 |
| 加分：协程·无锁队列·内存池·对象池 | 无锁 SPSC 队列、无锁对象池（已含）；协程可后续接入 |

---

## 实测（Release -O2，本机）

| 项 | 结果 |
|---|---|
| 编解码 | **31.8 M ticks/s**（≈30 ns/tick） |
| 无锁 SPSC ring | **8.10 M msg/s**，p50=200ns，p99=47.8µs |
| 互斥队列 mutex+deque | 0.99 M msg/s，p50=103µs，p99=2.05ms（对照） |
| 进程内链路 | 21.5M ticks、0 丢弃、5.37M ticks/s，tick→signal p50=200ns / p99=1.35ms |
| 跨进程 mmap | 13,236,632 发布 = 13,236,632 消费，零丢失 |
| 网络层回环 | UDP 80k+ 报文收发、0 坏帧、ring 0 丢弃；HTTP `/snapshot` 返回正确 JSON |
| 集成测试 | 端到端 95 断言全过：零丢失(20w ticks)、背压(小环丢 38k)、看门狗告警(3 次)、熔断 HALT、持仓限额拒绝、对象池并发 4w 操作 |
| 单测 | 985,044 次断言全过 + 网络层 23 项断言全过 |

---

## 构建与运行

```bash
make            # 构建全部；make test / itest / bench / demo / sched / net / nettest
# 或 CMake: cmake -S . -B build && cmake --build build && ctest --test-dir build

# 端到端集成测试（风控 + 监控 + 背压 + 看门狗 + 对象池）
./build/test_integration

# 跨进程演示（两个终端）
./build/gateway_main /md_xprocess 262144 4000
./build/strategy_main /md_xprocess 4000

# 网络层端到端演示（网络采集 / 抓包 / TCP·HTTP·UDP 三接口）
./build/net_demo
curl http://127.0.0.1:8080/snapshot        # 看 REST 接口
curl http://127.0.0.1:8080/book?sym=IF     # 看单合约
./build/test_net                           # 网络层单测

# Linux 下用 perf 定位热点（JD 要求的性能分析工具）
perf record -g ./build/bench_main && perf report
```
> 本机无原生 C++ 编译器时用 `make CXX="python -m ziglang c++"`（zig 内置 clang）。

---

## 目录

```
include/  common(Types) shm(mmap) ipc(SpscRingBuffer)
          feed(FuturesFeed,CtpLikeCodec) sched(PeriodicTimer) bench(LatencyRecorder)
          gateway(MdGateway) strategy(TickConsumer)
          risk(RiskControl) monitor(Monitor)
          util(ObjectPool) net(Socket,MdReceiver,PacketCapture,MdServer,IoMultiplexer,WireParse,JsonBook)
src/      demo_pipeline gateway_main strategy_main bench_main sched_main net_demo
          gateway/MdGateway strategy/TickConsumer risk/RiskControl monitor/Monitor
          net/(Socket,MdReceiver,PacketCapture,MdServer,IoMultiplexer).cpp
tests/    test_system.cpp  test_net.cpp  test_integration.cpp
scripts/  md-gateway.service  daily_snapshot.cron  snapshot.sh
docs/     quant-knowledge-checklist.md（金融/统计/C++ 工程面试知识）
```
