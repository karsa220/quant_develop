# ============================================================================
# Futures MD System — Makefile (C++17)
#
# 默认用系统 clang++/g++。本开发机无原生 C++ 编译器，使用 zig 内置 clang：
#   make CXX="python -m ziglang c++"
# （ziglang 0.16 自带 clang 21，等价于 clang++ -std=c++17）
#
# 目标:
#   make            # 构建全部
#   make test       # 单元 + 正确性测试
#   make itest      # 端到端集成测试（风控+监控+背压+看门狗+对象池）
#   make bench      # 性能基准（无锁 ring vs 互斥队列）
#   make demo       # 进程内完整数据链路演示
#   make gateway    # 行情网关（独立进程，跨进程 mmap）
#   make strategy   # 策略/执行进程（独立进程，消费共享内存）
#   make sched      # Linux 定时任务演示（09:30/15:00 会话 + 心跳）
#   make net        # 网络采集/抓包/多协议接口演示
#   make nettest    # 网络层单元测试
#   make coro       # C++20 协程异步行情服务演示（单线程事件循环 + 低延迟回环）
#   make cotest     # C++20 协程单元/集成测试（Generator/Task + 异步服务）
#   make clean
# ============================================================================

ifeq ($(OS),Windows_NT)
  BIN_EXT := .exe
  LIBS    := -lws2_32
else
  BIN_EXT :=
  LIBS    :=
endif

CXX      ?= clang++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Iinclude
# 协程模块需用 C++20（co_await/co_yield/协程句柄）。zig 内置 clang 21 与 MSVC 19.4x+ 均支持。
CXXFLAGS_CORO ?= -std=c++20 -O2 -Wall -Wextra -Iinclude
BUILD    := build

SRC_IPC  := src/ipc/SpscRingBuffer.cpp src/shm/ShmSegment.cpp src/sched/PeriodicTimer.cpp
SRC_FEED := src/feed/FuturesFeed.cpp src/feed/CtpLikeCodec.cpp
# 真实 CTP/飞马 feed 接入：统一接口 + 仿真后端（默认可编译运行，无需 SDK）
SRC_FEED_VENDOR := src/feed/MarketFeed.cpp src/feed/CtpMdFeed.cpp src/feed/FemasMdFeed.cpp \
                   src/feed/ctp/SimCtpMdApi.cpp src/feed/femas/SimFemasMdApi.cpp
# 真实封装：make feed HAS_CTP=1 / HAS_FEMAS=1 时加入并定义宏（需 third_party/<ctp|femas> SDK）
ifeq ($(HAS_CTP),1)
  SRC_FEED_VENDOR += src/feed/ctp/RealCtpMdApi.cpp
  FEED_DEFS += -DHAS_CTP
endif
ifeq ($(HAS_FEMAS),1)
  SRC_FEED_VENDOR += src/feed/femas/RealFemasMdApi.cpp
  FEED_DEFS += -DHAS_FEMAS
endif
SRC_NET  := src/net/Socket.cpp src/net/MdReceiver.cpp src/net/PacketCapture.cpp src/net/MdServer.cpp src/net/IoMultiplexer.cpp
SRC_GW   := src/gateway/MdGateway.cpp
SRC_STRAT:= src/strategy/TickConsumer.cpp
SRC_RISK := src/risk/RiskControl.cpp
SRC_MON  := src/monitor/Monitor.cpp
SRC_CORO := src/coro/AsyncMdServer.cpp
# 协程层只依赖网络抽象（Socket + IoMultiplexer），不引入 MdReceiver/PacketCapture/MdServer
SRC_NET_CORO := src/net/Socket.cpp src/net/IoMultiplexer.cpp
# 注：util/ObjectPool.h 为 header-only（模板），无需独立编译单元

.PHONY: all test itest bench demo gateway strategy sched net nettest coro cotest feed feedtest clean

all: test itest bench demo gateway strategy sched net nettest

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/test_system$(BIN_EXT): tests/test_system.cpp $(SRC_IPC) $(SRC_FEED) | $(BUILD)
	$(CXX) $(CXXFLAGS) tests/test_system.cpp $(SRC_IPC) $(SRC_FEED) -o $@

$(BUILD)/bench_main$(BIN_EXT): src/bench_main.cpp $(SRC_IPC) $(SRC_FEED) | $(BUILD)
	$(CXX) $(CXXFLAGS) src/bench_main.cpp $(SRC_IPC) $(SRC_FEED) -o $@

$(BUILD)/demo_pipeline$(BIN_EXT): src/demo_pipeline.cpp src/gateway/MdGateway.cpp src/strategy/TickConsumer.cpp $(SRC_FEED) $(SRC_IPC) $(SRC_RISK) $(SRC_MON) | $(BUILD)
	$(CXX) $(CXXFLAGS) src/demo_pipeline.cpp src/gateway/MdGateway.cpp src/strategy/TickConsumer.cpp $(SRC_FEED) $(SRC_IPC) $(SRC_RISK) $(SRC_MON) -o $@

$(BUILD)/gateway_main$(BIN_EXT): src/gateway_main.cpp src/gateway/MdGateway.cpp $(SRC_FEED) $(SRC_IPC) | $(BUILD)
	$(CXX) $(CXXFLAGS) src/gateway_main.cpp src/gateway/MdGateway.cpp $(SRC_FEED) $(SRC_IPC) -o $@

$(BUILD)/strategy_main$(BIN_EXT): src/strategy_main.cpp src/strategy/TickConsumer.cpp $(SRC_FEED) $(SRC_IPC) $(SRC_RISK) $(SRC_MON) | $(BUILD)
	$(CXX) $(CXXFLAGS) src/strategy_main.cpp src/strategy/TickConsumer.cpp $(SRC_FEED) $(SRC_IPC) $(SRC_RISK) $(SRC_MON) -o $@

$(BUILD)/sched_main$(BIN_EXT): src/sched_main.cpp $(SRC_IPC) | $(BUILD)
	$(CXX) $(CXXFLAGS) src/sched_main.cpp $(SRC_IPC) -o $@

$(BUILD)/net_demo$(BIN_EXT): src/net_demo.cpp $(SRC_NET) $(SRC_FEED) $(SRC_IPC) | $(BUILD)
	$(CXX) $(CXXFLAGS) src/net_demo.cpp $(SRC_NET) $(SRC_FEED) $(SRC_IPC) $(LIBS) -o $@

$(BUILD)/test_net$(BIN_EXT): tests/test_net.cpp $(SRC_NET) $(SRC_FEED) $(SRC_IPC) | $(BUILD)
	$(CXX) $(CXXFLAGS) tests/test_net.cpp $(SRC_NET) $(SRC_FEED) $(SRC_IPC) $(LIBS) -o $@

$(BUILD)/test_integration$(BIN_EXT): tests/test_integration.cpp $(SRC_GW) $(SRC_STRAT) $(SRC_RISK) $(SRC_MON) $(SRC_FEED) $(SRC_IPC) | $(BUILD)
	$(CXX) $(CXXFLAGS) tests/test_integration.cpp $(SRC_GW) $(SRC_STRAT) $(SRC_RISK) $(SRC_MON) $(SRC_FEED) $(SRC_IPC) -o $@

test: $(BUILD)/test_system$(BIN_EXT)
	./$<

itest: $(BUILD)/test_integration$(BIN_EXT)
	./$<

bench: $(BUILD)/bench_main$(BIN_EXT)
	./$<

demo: $(BUILD)/demo_pipeline$(BIN_EXT)
	./$<

gateway: $(BUILD)/gateway_main$(BIN_EXT)
strategy: $(BUILD)/strategy_main$(BIN_EXT)
sched: $(BUILD)/sched_main$(BIN_EXT)
net: $(BUILD)/net_demo$(BIN_EXT)
nettest: $(BUILD)/test_net$(BIN_EXT)
	./$<

# --- C++20 协程（JD 加分项：协程 / 无锁队列 / 内存池）---
$(BUILD)/coro_main$(BIN_EXT): src/coro_main.cpp $(SRC_CORO) $(SRC_FEED) $(SRC_RISK) $(SRC_NET_CORO) | $(BUILD)
	$(CXX) $(CXXFLAGS_CORO) src/coro_main.cpp $(SRC_CORO) $(SRC_FEED) $(SRC_RISK) $(SRC_NET_CORO) $(LIBS) -o $@

$(BUILD)/cotest$(BIN_EXT): tests/test_coro.cpp $(SRC_CORO) $(SRC_FEED) $(SRC_RISK) $(SRC_NET_CORO) | $(BUILD)
	$(CXX) $(CXXFLAGS_CORO) tests/test_coro.cpp $(SRC_CORO) $(SRC_FEED) $(SRC_RISK) $(SRC_NET_CORO) $(LIBS) -o $@

coro: $(BUILD)/coro_main$(BIN_EXT)
cotest: $(BUILD)/cotest$(BIN_EXT)
	./$<

# --- 真实 CTP/飞马 feed 接入（JD 职责：行情接入 C++ 实现）---
$(BUILD)/feed_demo$(BIN_EXT): src/feed_demo.cpp $(SRC_FEED_VENDOR) $(SRC_FEED) $(SRC_RISK) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(FEED_DEFS) src/feed_demo.cpp $(SRC_FEED_VENDOR) $(SRC_FEED) $(SRC_RISK) $(LIBS) -o $@

$(BUILD)/test_feed$(BIN_EXT): tests/test_feed.cpp $(SRC_FEED_VENDOR) $(SRC_FEED) $(SRC_RISK) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(FEED_DEFS) tests/test_feed.cpp $(SRC_FEED_VENDOR) $(SRC_FEED) $(SRC_RISK) $(LIBS) -o $@

feed: $(BUILD)/feed_demo$(BIN_EXT)
feedtest: $(BUILD)/test_feed$(BIN_EXT)
	./$<

clean:
	rm -rf $(BUILD)
