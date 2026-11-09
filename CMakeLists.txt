cmake_minimum_required(VERSION 3.25)

project(MQLConnector
    VERSION 1.1.0
    DESCRIPTION "Native MetaTrader 5 connector: MQL programs to any TCP server (FinCore and others)"
    LANGUAGES CXX)

set(MQLC_OUTPUT_NAME "MQLConnector" CACHE STRING "File name (without .dll) of the built connector")
option(MQLC_BUILD_TESTS "Build the GoogleTest suites" ${PROJECT_IS_TOP_LEVEL})
option(MQLC_BUILD_BENCH "Build the round-trip benchmark" ${PROJECT_IS_TOP_LEVEL})
option(MQLC_BUILD_TOOLS "Build the echo_server tool" ${PROJECT_IS_TOP_LEVEL})

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# One self-contained file for the tester agent folders: link the CRT statically everywhere,
# including the test dependencies, so nothing mixes runtimes.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)

include(CheckIPOSupported)
check_ipo_supported(RESULT MQLC_IPO_SUPPORTED OUTPUT MQLC_IPO_MESSAGE LANGUAGES CXX)

# Warnings as errors and whole-program optimisation for our own targets only.
function(mqlc_configure_target target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /permissive- /utf-8 /Zc:__cplusplus
            $<$<CONFIG:Release>:/O2>)
        target_compile_definitions(${target} PRIVATE _WIN32_WINNT=0x0A00 WIN32_LEAN_AND_MEAN NOMINMAX)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    if(MQLC_IPO_SUPPORTED)
        # MSVC: /GL for the compiler, /LTCG for lib.exe and link.exe.
        set_property(TARGET ${target} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
    endif()
endfunction()

find_package(Threads REQUIRED)

# GetVersion() reports PROJECT_VERSION.
configure_file(src/version.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/mqlconnector/version.h" @ONLY)

# --- Core: everything except the C ABI, platform-neutral apart from the socket layer ---------
add_library(mqlc_core STATIC
    src/core/codec.cpp
    src/core/config.cpp
    src/core/framer.cpp
    src/core/logger.cpp
    src/core/mailbox.cpp
    src/core/signal_json.cpp
    src/net/tcp_transport.cpp
    src/profiles/profile.cpp
    src/profiles/fincore/fincore_profile.cpp
    src/profiles/generic/generic_profile.cpp
    src/connector/session.cpp)
if(WIN32)
    target_sources(mqlc_core PRIVATE src/net/socket_winsock.cpp)
    target_link_libraries(mqlc_core PUBLIC ws2_32)
else()
    target_sources(mqlc_core PRIVATE src/net/socket_posix.cpp)
endif()
target_include_directories(mqlc_core PUBLIC src include "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(mqlc_core PUBLIC Threads::Threads)
mqlc_configure_target(mqlc_core)

# --- The DLL MetaTrader loads ----------------------------------------------------------------
if(WIN32)
    add_library(mqlc_dll SHARED src/exports.cpp src/MQLConnector.def)
    target_link_libraries(mqlc_dll PRIVATE mqlc_core)
    set_target_properties(mqlc_dll PROPERTIES
        OUTPUT_NAME ${MQLC_OUTPUT_NAME}
        PREFIX "")
    mqlc_configure_target(mqlc_dll)
endif()

# --- Loopback server shared by tests, bench and tools ----------------------------------------
if(MQLC_BUILD_TESTS OR MQLC_BUILD_BENCH OR MQLC_BUILD_TOOLS)
    add_library(mqlc_loopback STATIC tools/echo_server/loopback_server.cpp)
    target_include_directories(mqlc_loopback PUBLIC tools/echo_server)
    target_link_libraries(mqlc_loopback PUBLIC mqlc_core)
    mqlc_configure_target(mqlc_loopback)
endif()

if(MQLC_BUILD_TOOLS)
    add_executable(echo_server tools/echo_server/main.cpp)
    target_link_libraries(echo_server PRIVATE mqlc_loopback)
    mqlc_configure_target(echo_server)
endif()

if(MQLC_BUILD_BENCH)
    add_executable(mqlc_bench bench/roundtrip_bench.cpp)
    target_link_libraries(mqlc_bench PRIVATE mqlc_loopback)
    mqlc_configure_target(mqlc_bench)
endif()

if(MQLC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
