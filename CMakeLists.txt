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
    src/net/MdServer.cpp
    src/net/IoMultiplexer.cpp)
set(GW_SRC   src/gateway/MdGateway.cpp)
set(STRAT_SRC src/strategy/TickConsumer.cpp)
set(RISK_SRC src/risk/RiskControl.cpp)
set(MON_SRC  src/monitor/Monitor.cpp)
set(CORO_SRC src/coro/AsyncMdServer.cpp)
# 协程层只依赖网络抽象（Socket + IoMultiplexer），不引入 MdReceiver/PacketCapture/MdServer
set(NET_CORO_SRC src/net/Socket.cpp src/net/IoMultiplexer.cpp)
# 真实 CTP/飞马 feed 接入：统一接口 + 仿真后端（默认可编译运行，无需 SDK）
set(FEED_VENDOR_SRC
    src/feed/MarketFeed.cpp
    src/feed/CtpMdFeed.cpp
    src/feed/FemasMdFeed.cpp
    src/feed/ctp/SimCtpMdApi.cpp
    src/feed/femas/SimFemasMdApi.cpp)

# 单元测试
add_executable(test_system tests/test_system.cpp ${IPC_SRC} ${FEED_SRC})
enable_testing()
add_test(NAME unit_tests COMMAND test_system)

# 网络层 / 抓包 / 多接口 单元测试
add_executable(test_net tests/test_net.cpp ${NET_SRC} ${FEED_SRC} ${IPC_SRC})
add_test(NAME net_tests COMMAND test_net)

# 端到端集成测试：风控 + 监控 + 背压 + 看门狗 + 对象池
add_executable(test_integration
    tests/test_integration.cpp
    ${GW_SRC} ${STRAT_SRC} ${RISK_SRC} ${MON_SRC} ${FEED_SRC} ${IPC_SRC})
add_test(NAME integration_tests COMMAND test_integration)

# 性能基准：无锁 SPSC ring vs 互斥队列
add_executable(bench_main src/bench_main.cpp ${IPC_SRC} ${FEED_SRC})

# 进程内完整数据链路演示
add_executable(demo_pipeline
    src/demo_pipeline.cpp
    ${GW_SRC} ${STRAT_SRC}
    ${FEED_SRC} ${IPC_SRC} ${RISK_SRC} ${MON_SRC})

# 跨进程：行情网关（生产者）
add_executable(gateway_main
    src/gateway_main.cpp
    ${GW_SRC}
    ${FEED_SRC} ${IPC_SRC})

# 跨进程：策略/执行进程（消费者）
add_executable(strategy_main
    src/strategy_main.cpp
    ${STRAT_SRC}
    ${FEED_SRC} ${IPC_SRC} ${RISK_SRC} ${MON_SRC})

# Linux 定时任务演示（09:30/15:00 会话 + 心跳）
add_executable(sched_main src/sched_main.cpp ${IPC_SRC})

# 网络层端到端演示：网络采集 / 抓包 / TCP·HTTP·UDP 三种数据接口
add_executable(net_demo
    src/net_demo.cpp
    ${NET_SRC} ${FEED_SRC} ${IPC_SRC})

# Windows 需要链接 Winsock2
if(WIN32)
  foreach(t net_demo test_net coro_main cotest)
    target_link_libraries(${t} ws2_32)
  endforeach()
endif()

# ---------------------------------------------------------------------------
# C++20 协程模块（JD 加分项：协程 / 无锁队列 / 内存池 / 对象池）
# 单线程事件循环驱动「服务器协程 + 多客户端协程」：收 CTP 风格 tick → 风控 → 回 ack
# ---------------------------------------------------------------------------
add_executable(coro_main
    src/coro_main.cpp
    ${CORO_SRC} ${FEED_SRC} ${RISK_SRC} ${NET_CORO_SRC})
set_target_properties(coro_main PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)

add_executable(cotest
    tests/test_coro.cpp
    ${CORO_SRC} ${FEED_SRC} ${RISK_SRC} ${NET_CORO_SRC})
set_target_properties(cotest PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
add_test(NAME coro_tests COMMAND cotest)

# ---------------------------------------------------------------------------
# 真实 CTP / 飞马 feed 接入（JD 职责：行情接入 C++ 实现）
# 默认编译仿真后端（无需 SDK）；定义 HAS_CTP / HAS_FEMAS 时接入真实柜台封装
# （需把 vendor 头文件/库放到 third_party/<ctp|femas>/ 并链接对应库）。
# ---------------------------------------------------------------------------
add_executable(feed_demo src/feed_demo.cpp ${FEED_VENDOR_SRC} ${FEED_SRC} ${RISK_SRC})
add_executable(test_feed tests/test_feed.cpp ${FEED_VENDOR_SRC} ${FEED_SRC} ${RISK_SRC})
add_test(NAME feed_tests COMMAND test_feed)

if(HAS_CTP)
  target_sources(feed_demo PRIVATE src/feed/ctp/RealCtpMdApi.cpp)
  target_compile_definitions(feed_demo PRIVATE HAS_CTP)
  target_sources(test_feed PRIVATE src/feed/ctp/RealCtpMdApi.cpp)
  target_compile_definitions(test_feed PRIVATE HAS_CTP)
endif()
if(HAS_FEMAS)
  target_sources(feed_demo PRIVATE src/feed/femas/RealFemasMdApi.cpp)
  target_compile_definitions(feed_demo PRIVATE HAS_FEMAS)
  target_sources(test_feed PRIVATE src/feed/femas/RealFemasMdApi.cpp)
  target_compile_definitions(test_feed PRIVATE HAS_FEMAS)
endif()
