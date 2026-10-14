# 量化私募 / HFT C++ 岗 —— 知识清单（结合本项目）

> 用于简历项目 `futures-md-system` 的面试准备。分三块：**金融/期货**、**统计学**、**C++/低延迟工程**。
> 每条都标了「本项目哪里用到了」，方便面试时把知识点落到代码上。

---

## A. 金融 / 期货基础知识

- [ ] **期货合约 (Future)** 是什么：标准化、交易所集中竞价、保证金交易、T+0、双向。
      *本项目*：6 个仿真合约 IF/IC/TF/RB/AU/CU（股指期货/国债/商品）。
- [ ] **Tick / 最小变动价位 (tick size)**：价格是 tick 的整数倍。如 IF 0.2 点、RB 1 元。
      *本项目*：`Instrument.tickSize`，价格按 `delta * tickSize` 步进。
- [ ] **买卖盘口 (order book)**：买一卖一、买卖价、买卖量、价差 (spread)、深度 (depth)。
      *本项目*：`TickConsumer` 重建简化盘口，监测 book-cross（成交价穿越盘口）。
- [ ] **开盘 / 收盘时段**：国内期货日盘 09:30–11:30、13:30–15:00，部分品种有夜盘。
      *本项目*：`PeriodicTimer` 绝对定时触发 `SESSION OPEN(09:30)` / `SESSION CLOSE(15:00)`。
- [ ] **行情类型**：Tick（成交）、快照 (Snapshot)、深度行情 (L2)、心跳 (Heartbeat)。
      *本项目*：`MsgType`：MSG_TICK / MSG_SNAPSHOT / MSG_HEARTBEAT / MSG_CONTROL。
- [ ] **CTP / 飞马 / 易盛** 等柜台/行情 API：二进制协议、行情订阅、登录会话。
      *本项目*：`CtpLikeCodec` 用 magic+type+len+payload+checksum 模拟 CTP 风格线路帧。
- [ ] **保证金 / 杠杆 / 强平**：理解风控侧。
- [ ] **撮合规则**：价格优先、时间优先；市价/限价单。
      （本项目聚焦**行情侧**，撮合引擎在姊妹项目 `lob-engine`。）
- [ ] **滑点 / 冲击成本 / 流动性**：与延迟直接相关的交易成本。

---

## B. 统计学 / 量化方法

- [ ] **泊松过程 (Poisson process)**：描述「随机事件到达」——行情 tick 的到达可建模为泊松流，
      相邻到达间隔服从指数分布。
      *本项目*：`FuturesFeed::nextTick` 用指数分布 `nextExponential(rate)` 推进事件时刻（限速）。
- [ ] **随机游走 (random walk) / 几何布朗运动**：中间价的微观演化常用随机游走近似。
      *本项目*：中间价以 `±sigma` 随机步进化，`delta=0` 时随机给 ±1 tick 防停滞。
- [ ] **均匀分布 / 伪随机**：`xorshift64*` 快速 RNG（无锁、适合每 tick 取随机数）。
      *本项目*：`FastRng`（xorshift64* 变种）。
- [ ] **分位数 / 延迟分布**：p50/p90/p99/p99.9 比平均值更能刻画尾部延迟（HFT 命门）。
      *本项目*：`LatencyRecorder` 排序采样算分位；tick→signal p50=200ns, p99=1.35ms。
- [ ] **大数定律 / 统计显著性**：基准要跑足够样本（本项目 2000 万条）才有意义。
- [ ] **自相关 / 序列相关**：行情信号是否滞后可用（策略侧）。
- [ ] **回测偏差**：未来函数、幸存者偏差、滑点假设（盘后研究时必提）。

---

## C. C++ / 低延迟工程（面试重点）

### C.1 并发与无锁
- [ ] **SPSC 模型**：单生产者/单消费者，天然无竞争 —— 生产者只写 `head`、消费者只写 `tail`。
      *本项目*：`SpscRingBuffer`，发布/消费 O(1)、无锁。
- [ ] **无锁 (lock-free) vs 免等待 (wait-free)**：本项目发布/消费是 lock-free（遇到满/空自旋或返回）。
- [ ] **伪共享 (false sharing)**：多线程写同一缓存行的不同变量会互相 invalidate。
      *本项目*：`head`/`tail`/`lost` 各 `alignas(64)` 独占缓存行。
- [ ] **缓存行 (cache line) = 64 字节**：结构体按 64B 对齐，避免跨行。
- [ ] **内存对齐陷阱**：`std::atomic` 必须按其对齐要求放置，否则 UB（本项目曾因 vector 16 字节对齐
      导致 atomic 未 64 对齐，已用动态 64 对齐 offset 修复）。

### C.2 内存模型 / 原子
- [ ] **memory_order**：`relaxed` / `acquire` / `release` / `seq_cst` 的区别与代价。
      *本项目*：生产者 `head.store(release)`、消费者 `head.load(acquire)` —— 保证槽位写入对消费者可见，
      且比 `seq_cst` 便宜。
- [ ] **acquire/release 语义**：release 写之前的修改，对随后 acquire 读同一原子可见。
- [ ] **`std::atomic` 必须 lock-free**：`is_always_lock_free` 校验（本项目 attach 时检查并告警）。

### C.3 共享内存 / IPC
- [ ] **`shm_open` + `mmap`**（Linux）：命名共享内存落 `/dev/shm`，跨进程映射同一物理页 → 零拷贝。
      *本项目*：`ShmSegment::create/open`（Linux 分支）。
- [ ] **`CreateFileMapping` / `MapViewOfFile`**（Windows）：命名映射，`Local\` 前缀。
      *本项目*：`ShmSegment`（Windows 分支，UTF-16 名，命名映射避免空名）。
- [ ] **跨进程一致性**：依赖原子 + 内存序，不依赖进程间锁。
- [ ] **mmap 风险**：MAP_FAILED 检查、大小用 `ftruncate`/`VirtualQuery` 确定、`munmap`/`UnmapViewOfFile` 清理。

### C.4 定时器 / Linux 调度
- [ ] **`timerfd_create` + `timerfd_settime`**：`CLOCK_MONOTONIC` 周期、`CLOCK_REALTIME` + `TFD_TIMER_ABSTIME` 绝对。
      *本项目*：`PeriodicTimer::startPeriodic` / `startAtWallClock`（Linux 分支）。
- [ ] **绝对定时 vs 相对定时**：开盘/收盘用绝对时刻（不受进程启动时间影响）。
- [ ] **内核唤醒 vs busy-wait**：timerfd 由内核在到期时唤醒，`read()` 阻塞，不占 CPU。
- [ ] **systemd 保活**：`Restart=always` 让网关崩溃自动拉起。
- [ ] **cron 定时任务**：日终快照、预热拉起、平滑停止。

### C.5 性能 / 基准
- [ ] **吞吐 vs 延迟的权衡**：ring 容量越大越不易丢但占内存；容量小则背压多。
- [ ] **基准要可复现**：固定样本量、多跑取稳、隔离干扰核。
- [ ] **无锁 ring vs mutex 队列**：本项目实测 **8.2× 吞吐提升**（8.10M vs 0.99M msg/s）。
- [ ] **火焰图 / perf / 缓存命中率**（L1/L2 miss）是定位延迟的来源（进阶）。

### C.6 网络 / 协议（行情侧）
- [ ] **二进制协议设计**：定长头（magic/type/len）+ 变长 payload + 校验和；`#pragma pack(1)` 控对齐。
      *本项目*：`common/Types.h` 线路格式。
- [ ] **大小端 (LE/BE)**：线路帧统一小端（LE16 helpers）。
- [ ] **校验和**：xor 校验检测损坏（生产用 CRC32/字段哈希更强）。

---

## D. 面试高频追问 & 本项目可展开的点

1. Q：环形缓冲满了怎么办？
   A：本项目 `tryPublish` 返回 false 并 `lost++` 计数（背压/丢弃策略）；生产可选阻塞、丢弃或扩容。
2. Q：为什么不用 mutex 队列？
   A：mutex 有系统调用 + 缓存行 bouncing；实测吞吐低 8.2×、p99 高一个数量级。
3. Q：跨进程怎么保证不乱序 / 不丢？
   A：SPSC 单写单读天然 FIFO；head/tail 原子 + acquire/release；共享内存零拷贝。
4. Q：缓存行对齐怎么做？为什么重要？
   A：`alignas(64)` 让 head/tail/lost 各占一行，消除伪共享；曾因对齐不足导致 UB 已修复。
5. Q：延迟怎么测？
   A：在生产者写 `srcTs`、消费者读时算差，采样排序得 p50/p99/p99.9（`LatencyRecorder`）。
6. Q：开盘/收盘怎么触发？
   A：`timerfd` 绝对定时 09:30/15:00；另由 systemd 保活、cron 做日终快照。
7. Q：Windows 上怎么实现同款？
   A：`CreateFileMapping`/`MapViewOfFile` + `CreateWaitableTimerExW`（高分辨率），同一份 C++。

---

## E. 推荐延伸（把项目做深，对应更高级岗位）

- [ ] 真实 CTP/飞马行情接入（替换 `FuturesFeed` 合成源）。
- [ ] L2 深度行情 + 完整 OrderBook（合并 `lob-engine`）。
- [ ] 内核旁路网络 (Solarflare/Onload, DPDK, Solarflare TCPDirect) 降网卡栈延迟。
- [ ] 大页 (hugetlb) / `mlock` 锁内存防换页；CPU 亲和 + `SCHED_FIFO`。
- [ ] 行情降频/合并 (throttle)、乱序重排 (sequencer)。
- [ ] 回测框架 + 信号有效性统计。
