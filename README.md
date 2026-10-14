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
- **收益**：较 `std::mutex + deque` 吞吐 **8.2×**（8.10M vs 0.99M msg/s），p99 从 2.05ms 降到 47.8µs。

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

---

## 实测（Release -O2，本机）

| 项 | 结果 |
|---|---|
| 编解码 | **31.8 M ticks/s**（≈30 ns/tick） |
| 无锁 SPSC ring | **8.10 M msg/s**，p50=200ns，p99=47.8µs |
| 互斥队列 mutex+deque | 0.99 M msg/s，p50=103µs，p99=2.05ms（对照） |
| 进程内链路 | 21.5M ticks、0 丢弃、5.37M ticks/s，tick→signal p50=200ns / p99=1.35ms |
| 跨进程 mmap | 13,236,632 发布 = 13,236,632 消费，零丢失 |
| 单测 | 985,044 次断言全过 |

---

## 构建与运行

```bash
make            # 构建全部；make test / bench / demo / sched
# 或 CMake: cmake -S . -B build && cmake --build build && ctest --test-dir build

# 跨进程演示（两个终端）
./build/gateway_main /md_xprocess 262144 4000
./build/strategy_main /md_xprocess 4000
```
> 本机无原生 C++ 编译器时用 `make CXX="python -m ziglang c++"`（zig 内置 clang）。

---

## 目录

```
include/  common(Types) shm(SpscRingBuffer 所在 mmap) ipc(SpscRingBuffer)
          feed(FuturesFeed,CtpLikeCodec) sched(PeriodicTimer) bench(LatencyRecorder)
          gateway(MdGateway) strategy(TickConsumer)
src/      demo_pipeline gateway_main strategy_main bench_main sched_main
tests/    test_system.cpp
scripts/  md-gateway.service  daily_snapshot.cron  snapshot.sh
docs/     quant-knowledge-checklist.md（金融/统计/C++ 工程面试知识）
```
