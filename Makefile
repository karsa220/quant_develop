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
#   make bench      # 性能基准（无锁 ring vs 互斥队列）
#   make demo       # 进程内完整数据链路演示
#   make gateway    # 行情网关（独立进程，跨进程 mmap）
#   make strategy   # 策略/执行进程（独立进程，消费共享内存）
#   make sched      # Linux 定时任务演示（09:30/15:00 会话 + 心跳）
#   make clean
# ============================================================================

ifeq ($(OS),Windows_NT)
  BIN_EXT := .exe
else
  BIN_EXT :=
endif

CXX      ?= clang++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Iinclude
BUILD    := build

SRC_IPC  := src/ipc/SpscRingBuffer.cpp src/shm/ShmSegment.cpp src/sched/PeriodicTimer.cpp
SRC_FEED := src/feed/FuturesFeed.cpp src/feed/CtpLikeCodec.cpp

.PHONY: all test bench demo gateway strategy sched clean

all: test bench demo gateway strategy sched

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/test_system$(BIN_EXT): tests/test_system.cpp $(SRC_IPC) $(SRC_FEED) | $(BUILD)
	$(CXX) $(CXXFLAGS) tests/test_system.cpp $(SRC_IPC) $(SRC_FEED) -o $@

$(BUILD)/bench_main$(BIN_EXT): src/bench_main.cpp $(SRC_IPC) $(SRC_FEED) | $(BUILD)
	$(CXX) $(CXXFLAGS) src/bench_main.cpp $(SRC_IPC) $(SRC_FEED) -o $@

$(BUILD)/demo_pipeline$(BIN_EXT): src/demo_pipeline.cpp src/gateway/MdGateway.cpp src/strategy/TickConsumer.cpp $(SRC_FEED) $(SRC_IPC) | $(BUILD)
	$(CXX) $(CXXFLAGS) src/demo_pipeline.cpp src/gateway/MdGateway.cpp src/strategy/TickConsumer.cpp $(SRC_FEED) $(SRC_IPC) -o $@

$(BUILD)/gateway_main$(BIN_EXT): src/gateway_main.cpp src/gateway/MdGateway.cpp $(SRC_FEED) $(SRC_IPC) | $(BUILD)
	$(CXX) $(CXXFLAGS) src/gateway_main.cpp src/gateway/MdGateway.cpp $(SRC_FEED) $(SRC_IPC) -o $@

$(BUILD)/strategy_main$(BIN_EXT): src/strategy_main.cpp src/strategy/TickConsumer.cpp $(SRC_FEED) $(SRC_IPC) | $(BUILD)
	$(CXX) $(CXXFLAGS) src/strategy_main.cpp src/strategy/TickConsumer.cpp $(SRC_FEED) $(SRC_IPC) -o $@

$(BUILD)/sched_main$(BIN_EXT): src/sched_main.cpp $(SRC_IPC) | $(BUILD)
	$(CXX) $(CXXFLAGS) src/sched_main.cpp $(SRC_IPC) -o $@

test: $(BUILD)/test_system$(BIN_EXT)
	./$<

bench: $(BUILD)/bench_main$(BIN_EXT)
	./$<

demo: $(BUILD)/demo_pipeline$(BIN_EXT)
	./$<

gateway: $(BUILD)/gateway_main$(BIN_EXT)
strategy: $(BUILD)/strategy_main$(BIN_EXT)
sched: $(BUILD)/sched_main$(BIN_EXT)

clean:
	rm -rf $(BUILD)
