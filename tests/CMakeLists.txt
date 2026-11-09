include(FetchContent)

# Test-only dependency; the DLL itself has none.
set(gtest_force_shared_crt OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)

# A CMake built against GnuTLS/OpenSSL (MSYS2, WinLibs) has no CA store on Windows and fails the
# download with "certificate signer not trusted". Borrow Git for Windows' bundle when nothing else
# is configured. Offline: -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=<unpacked googletest>.
if(WIN32 AND NOT DEFINED CMAKE_TLS_CAINFO AND NOT DEFINED ENV{CMAKE_TLS_CAINFO})
    find_file(MQLC_CA_BUNDLE ca-bundle.crt
        PATHS "$ENV{ProgramFiles}/Git/mingw64/etc/ssl/certs" "$ENV{ProgramFiles}/Git/usr/ssl/certs"
        NO_DEFAULT_PATH)
    if(MQLC_CA_BUNDLE)
        set(CMAKE_TLS_CAINFO "${MQLC_CA_BUNDLE}")
    endif()
endif()

FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
    URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SYSTEM)  # its headers do not have to pass our /W4 /WX
FetchContent_MakeAvailable(googletest)

include(GoogleTest)

add_executable(mqlc_tests
    codec_test.cpp
    config_test.cpp
    fincore_compat_test.cpp
    frame_decoder_test.cpp
    framer_test.cpp
    logger_test.cpp
    mailbox_test.cpp
    phase1_session_test.cpp
    session_test.cpp
    signal_json_test.cpp
    transport_test.cpp
    utf_test.cpp)
target_link_libraries(mqlc_tests PRIVATE mqlc_loopback GTest::gtest_main)
mqlc_configure_target(mqlc_tests)
gtest_discover_tests(mqlc_tests DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 120)

if(WIN32)
    add_executable(mqlc_dll_tests dll_exports_test.cpp)
    target_link_libraries(mqlc_dll_tests PRIVATE mqlc_loopback GTest::gtest_main)
    target_compile_definitions(mqlc_dll_tests PRIVATE "MQLC_DLL_PATH=L\"$<TARGET_FILE:mqlc_dll>\"")
    add_dependencies(mqlc_dll_tests mqlc_dll)
    mqlc_configure_target(mqlc_dll_tests)
    gtest_discover_tests(mqlc_dll_tests DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 120)
endif()
