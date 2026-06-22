cmake_minimum_required(VERSION 3.16)
project(bt_trading_system VERSION 2.0.0 LANGUAGES C)

# ── Compiler Settings ──────────────────────────────────────────────────

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Optimization flags
# NOTE: -ffast-math is intentionally NOT used — this is a financial system
# and IEEE 754 compliance is required for correct price calculations.
set(CMAKE_C_FLAGS_RELEASE "-O3 -march=native -flto -DNDEBUG -funroll-loops")

# Common flags
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers")

# Debug flags
set(CMAKE_C_FLAGS_DEBUG "-g -O0 -fsanitize=address,undefined")

# Link-time optimization
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)

# ── Include directories ───────────────────────────────────────────────
include_directories(${CMAKE_SOURCE_DIR}/include)

# ── Source files (all pure C, V7 compliant) ────────────────────────────

set(C_SOURCES
    utils/memory_pool.c
    utils/timer.c
    utils/cpu_affinity.c
    utils/logger.c
    utils/hugepage.c
    utils/numa.c
    utils/slab_allocator.c
    core/risk_engine.c
    core/oms.c
    core/sequencer.c
    core/order_gate.c
    core/event_bus.c
    core/clearing.c
    core/shard_ipc.c
    core/recovery.c
    core/scheduler.c
    core/order_book.c
    core/matching_engine.c
    utils/memory_tier.c
    net/gateway.c
    md/market_data.c
    persistence/journal.c
    ipc/shmem.c
    ${CMAKE_SOURCE_DIR}/../bench/benchmark.c
)

# Main entry point
set(MAIN_SOURCE main.c)

# ── Build targets ─────────────────────────────────────────────────────

# Core library (C objects only)
add_library(bt_core STATIC ${C_SOURCES})
target_link_libraries(bt_core PUBLIC pthread rt)

# Link with numa if available
find_library(NUMA_LIB numa)
if(NUMA_LIB)
    target_link_libraries(bt_core PUBLIC ${NUMA_LIB})
endif()

# Main executable
add_executable(bt_trading ${MAIN_SOURCE})
target_link_libraries(bt_trading bt_core)

# ── Install ───────────────────────────────────────────────────────────
install(TARGETS bt_trading RUNTIME DESTINATION bin)

# ── Print summary ─────────────────────────────────────────────────────
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "C compiler: ${CMAKE_C_COMPILER}")
message(STATUS "C++ compiler: ${CMAKE_CXX_COMPILER}")
message(STATUS "NUMA library: ${NUMA_LIB}")
