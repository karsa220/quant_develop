# ============================================================================
# Futures MD System — CMakeLists.txt (C++17)
#
#   cmake -S . -B build && cmake --build build
#   ctest --test-dir build        # 运行单元测试
#
# 无第三方依赖：仅依赖 C++17 标准库 + 平台 API
#   (Linux: shm_open/mmap/timerfd；Windows: CreateFileMapping/WaitableTimer)。
# ============================================================================
cmake_minimum_required(VERSION 3.16)
project(futures_md_system CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
add_compile_options(-Wall -Wextra)

set(IPC_SRC
    src/ipc/SpscRingBuffer.cpp
    src/shm/ShmSegment.cpp
    src/sched/PeriodicTimer.cpp)
set(FEED_SRC
    src/feed/FuturesFeed.cpp
    src/feed/CtpLikeCodec.cpp)
set(NET_SRC
    src/net/Socket.cpp
    src/net/MdReceiver.cpp
    src/net/PacketCapture.cpp
    src/net/MdServer.cpp)

# 单元测试
add_executable(test_system tests/test_system.cpp ${IPC_SRC} ${FEED_SRC})
enable_testing()
add_test(NAME unit_tests COMMAND test_system)

# 网络层 / 抓包 / 多接口 单元测试
add_executable(test_net tests/test_net.cpp ${NET_SRC} ${FEED_SRC} ${IPC_SRC})
add_test(NAME net_tests COMMAND test_net)

# 性能基准：无锁 SPSC ring vs 互斥队列
add_executable(bench_main src/bench_main.cpp ${IPC_SRC} ${FEED_SRC})

# 进程内完整数据链路演示
add_executable(demo_pipeline
    src/demo_pipeline.cpp
    src/gateway/MdGateway.cpp
    src/strategy/TickConsumer.cpp
    ${FEED_SRC} ${IPC_SRC})

# 跨进程：行情网关（生产者）
add_executable(gateway_main
    src/gateway_main.cpp
    src/gateway/MdGateway.cpp
    ${FEED_SRC} ${IPC_SRC})

# 跨进程：策略/执行进程（消费者）
add_executable(strategy_main
    src/strategy_main.cpp
    src/strategy/TickConsumer.cpp
    ${FEED_SRC} ${IPC_SRC})

# Linux 定时任务演示（09:30/15:00 会话 + 心跳）
add_executable(sched_main src/sched_main.cpp ${IPC_SRC})

# 网络层端到端演示：网络采集 / 抓包 / TCP·HTTP·UDP 三种数据接口
add_executable(net_demo
    src/net_demo.cpp
    ${NET_SRC} ${FEED_SRC} ${IPC_SRC})

# Windows 需要链接 Winsock2
if(WIN32)
  foreach(t net_demo test_net)
    target_link_libraries(${t} ws2_32)
  endforeach()
endif()
