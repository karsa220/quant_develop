# Futures Market-Data System (低延迟期货行情数据链路)

> 面向**量化私募 / 高频交易 (HFT) C++ 交易系统 / 低延迟开发岗**的简历项目。
> 把「期货行情采集 → Linux 定时任务 → 行情网关 → 共享内存(mmap) → 低延迟消息处理 → 性能基准测试」
> **串成一条完整的数据链路**，而非孤立的订单簿 / 撮合引擎。

零第三方依赖：仅 C++17 标准库 + 平台 API
（Linux `shm_open`/`mmap`/`timerfd`；Windows `CreateFileMapping`/`WaitableTimer`）。
同一套代码**跨平台**编译运行（本仓库在 Windows 上用 zig 内置 clang 验证通过，生产部署在 Linux）。

---

## 1. 为什么是这个项目（岗位匹配）

量化私募 / HFT 的 C++ 岗，面试官最看重的是：
**你能不能写出一条在微秒级延迟下、不丢数据、可跨进程扩展的行情数据链路**。

单做一个订单簿 + 撮合引擎深度不够。本项目覆盖的真实生产链路模块：

| 招聘 JD 关键词            | 本项目对应实现                                              |
|--------------------------|-------------------------------------------------------------|
| 期货行情采集             | `FuturesFeed` 合成交易所行情（泊松到达 + 随机游走中间价），`CtpLikeCodec` 模拟 CTP 二进制线路协议 |
| Linux 定时任务           | `PeriodicTimer`（timerfd / 等待定时器）：09:30 开盘 / 15:00 收盘绝对定时 + 每秒心跳；`systemd` 保活 + `cron` 收盘快照 |
| 行情网关 (Gateway)       | `MdGateway`：采集→编解码→发布职责链，单生产者写入共享内存    |
| 共享内存 / mmap          | `ShmSegment` 跨平台命名共享内存；`SpscRingBuffer` 跑在共享内存里，进程间**零拷贝** |
| 低延迟消息处理          | **无锁 SPSC 环形缓冲**：head/tail 各占一条缓存行、acquire/release 内存序、O(1) 发布消费 |
| 性能基准测试            | `bench_main`：编解码吞吐、无锁 ring vs 互斥队列 8.2× 对比、tick→signal 延迟分位 |

---

## 2. 架构：一条完整的数据链路

```
                         ┌──────────────── 行情网关进程 (gateway_main) ────────────────┐
 交易所/合成行情            │                                                            │
   (tick 流)  ──►  FuturesFeed ──► CtpLikeCodec(编码) ──► SpscRingBuffer.tryPublish()   │
                 (泊松+随机游走)  (线路帧/校验)      │                                  │
                                                   ▼                                  │
                                           ┌───────────────────┐                      │
                                           │ 命名共享内存(mmap) │  /dev/shm/md_xprocess │
                                           │ 无锁 SPSC 环形缓冲 │  ← 跨进程零拷贝          │
                                           └───────────────────┘                      │
                                                   │                                  │
                         └─────────────────────────┼──────────────────────────────────┘
                                                   ▼
                         ┌──────────────── 策略/执行进程 (strategy_main) ──────────────┐
                                           SpscRingBuffer.tryConsume() ──► CtpLikeCodec(解码) │
                                                   │                                  │
                                                   ▼                                  │
                                           TickConsumer: 重建盘口 + 生成信号            │
                                           (book-cross / 反转信号) + 延迟统计           │
                         └──────────────────────────────────────────────────────────────┘

  旁路(Linux 定时任务):
    PeriodicTimer ── 09:30 开盘 / 15:00 收盘(绝对定时) + 每秒心跳/统计刷盘
    systemd        ── 网关常驻保活 (Restart=always)
    cron           ── 09:25 预热 / 15:15 收盘快照 / 15:30 平滑停止
```

**关键点**：生产者只写 `head`、消费者只写 `tail`，各自独占缓存行 —— 天然无竞争、无锁、
无伪共享；数据驻留在共享内存，跨进程消费侧**直接读内存**，不走 socket / 不拷贝。

---

## 3. 构建

### 3.1 Linux / macOS（常规）

```bash
# 需要 g++/clang++ (C++17)
make            # 构建全部目标
make test       # 单元测试（98.5 万次断言）
make bench      # 性能基准
make demo       # 进程内完整链路演示
```

或用 CMake：

```bash
cmake -S . -B build && cmake --build build
ctest --test-dir build        # 运行单元测试
```

### 3.2 本开发机（无原生 C++ 编译器，用 zig 内置 clang）

```bash
# ziglang 0.16 自带 clang 21，等价于 clang++ -std=c++17
make CXX="python -m ziglang c++"
# 或直接调用
python -m ziglang c++ -std=c++17 -O2 -Iinclude src/bench_main.cpp \
    src/ipc/SpscRingBuffer.cpp src/shm/ShmSegment.cpp \
    src/feed/FuturesFeed.cpp src/feed/CtpLikeCodec.cpp src/sched/PeriodicTimer.cpp \
    -o build/bench_main
```

---

## 4. 运行

### 4.1 单元 / 正确性测试
```bash
make test
# -> 985044 checks, 0 failures   (环形缓冲顺序/满丢弃/并发、编解码往返、行情合法性、定时触发)
```

### 4.2 性能基准
```bash
make bench
```
实测（本机，Release -O2）：

| 测试                    | 结果                                                        |
|-------------------------|-------------------------------------------------------------|
| 编解码 (codec)          | **31.8 M ticks/s**（≈31.5 ns/tick）                          |
| 无锁 SPSC ring          | **8.10 M msg/s**，p50=200 ns，p99=47.8 µs，max=386.5 µs      |
| 互斥队列 (mutex+deque)  | 0.99 M msg/s，p50=103.1 µs，p99=2.05 ms                     |
| **相对提升**            | **无锁 ring 约 8.2× 于互斥队列**                              |

### 4.3 进程内完整链路演示
```bash
make demo
```
实测：21.5M ticks 发布、**0 丢弃**、5.37M ticks/s；
tick→signal 延迟 p50=200 ns / p90=4.8 µs / p99=1.35 ms / p99.9=5.53 ms。

### 4.4 跨进程（真实 mmap IPC）
开两个终端 / 两个进程，共享同一段命名共享内存：
```bash
# 终端 A：行情网关
./build/gateway_main /md_xprocess 262144 4000
# 终端 B：策略进程（最多轮询 5s 等网关建好共享内存）
./build/strategy_main /md_xprocess 4000
```
实测：网关发布 **13,236,632** ticks，策略消费 **13,236,632** —— **跨进程零丢失**。
（ring 满时的 `drop` 计数是背压事件计数，已发布消息全部送达。）

### 4.5 Linux 定时任务演示
```bash
make sched            # 演示：开盘 +3s / 收盘 +6s / 心跳每秒
make sched --prod     # 生产：按当天 09:30 / 15:00 绝对时刻触发
```

---

## 5. 复杂度 / 低延迟设计要点

- **发布 / 消费均 O(1)**，与消息速率无关（环形缓冲，模运算定位槽位）。
- **无锁 SPSC**：生产者只动 `head`、消费者只动 `tail`，无 CAS、无互斥。
- **缓存行隔离**：`head`/`tail`/`lost` 各 `alignas(64)` 独占缓存行 → 消除伪共享 (false sharing)。
- **acquire/release 内存序**（非 seq_cst）：把 cache 同步开销压到最低，同时保证消费者看到完整写入。
- **共享内存零拷贝**：数据在 mmap 区域，跨进程直接读，绕开内核协议栈。
- **定时器由内核唤醒**（timerfd / 等待定时器），不 busy-wait、不占 CPU。
- **溢出即计数** (`lost`)：ring 满时生产者可选择丢弃并精确计数，便于容量规划与背压监控。

---

## 6. 目录结构

```
futures-md-system/
├── include/
│   ├── common/Types.h          # 二进制线路协议 (#pragma pack(1): 头+Tick+Snapshot+心跳)
│   ├── shm/ShmSegment.h/.cpp   # 跨平台命名共享内存 (shm_open / CreateFileMapping)
│   ├── ipc/SpscRingBuffer.h/.cpp # 无锁 SPSC 环形缓冲 (缓存行隔离 + acquire/release)
│   ├── feed/FuturesFeed.h/.cpp # 合成期货行情 (泊松到达 + 随机游走)
│   ├── feed/CtpLikeCodec.h/.cpp# CTP 风格二进制编解码 (LE16 + xor 校验)
│   ├── sched/PeriodicTimer.h/.cpp # 高精度周期/绝对定时 (timerfd / 等待定时器)
│   ├── bench/LatencyRecorder.h # 延迟分位统计 (p50/p90/p99/p99.9)
│   ├── gateway/MdGateway.h/.cpp# 行情网关：采集→编解码→发布 职责链
│   └── strategy/TickConsumer.h/.cpp # 策略侧：重建盘口 + 信号 + 延迟统计
├── src/
│   ├── demo_pipeline.cpp       # 进程内完整链路演示
│   ├── gateway_main.cpp        # 网关进程 (跨进程 mmap)
│   ├── strategy_main.cpp       # 策略进程 (跨进程 mmap)
│   ├── bench_main.cpp          # 基准：codec + ring vs mutex
│   └── sched_main.cpp          # Linux 定时任务演示
├── tests/test_system.cpp       # 单元/正确性测试 (零依赖)
├── scripts/
│   ├── md-gateway.service      # systemd 单元 (常驻保活)
│   ├── daily_snapshot.cron     # 收盘快照 crontab
│   └── snapshot.sh             # 收盘快照落地脚本
├── Makefile / CMakeLists.txt
└── docs/quant-knowledge-checklist.md  # 金融/统计/C++ 工程知识清单（面试用）
```

---

## 7. 简历 / 面试话术要点

- 「我搭了一条**低延迟期货行情数据链路**：行情网关用无锁 SPSC 环形缓冲写入共享内存，
  策略进程跨进程零拷贝消费，实测 tick→signal p50 200ns、p99 1.35ms，跨进程零丢失。」
- 「环形缓冲**无锁、缓存行隔离、acquire/release 内存序**，发布消费 O(1)；
  相对 `mutex+deque` 吞吐 **8.2×**。」
- 「开盘/收盘用 `timerfd` 绝对定时触发（微秒级 cron 替代），网关由 systemd 保活、
  cron 做收盘快照 —— 这是 Linux 侧调度。」
- 「共享内存用 `shm_open`/`mmap`（Linux）与 `CreateFileMapping`（Windows）双实现，
  同一份 C++ 代码跨平台。」
- 能展开讲：伪共享、内存序、环形缓冲满则丢弃的背压策略、泊松过程建模行情到达。

详见 [`docs/quant-knowledge-checklist.md`](docs/quant-knowledge-checklist.md)。
