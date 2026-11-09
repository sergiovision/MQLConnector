// Windows only: loads the built DLL the way MT5 does (LoadLibraryW + GetProcAddress) and calls
// every export against the loopback server.
#include "test_support.h"

#include <gtest/gtest.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <string>

#ifndef MQLC_DLL_PATH
#error "MQLC_DLL_PATH must name the built DLL"
#endif

namespace {

using namespace mqlc::test;

using InitLibFn = int32_t(__stdcall*)(const wchar_t*);
using DestroyLibFn = void(__stdcall*)(int64_t);
using DoSendMessageFn = const wchar_t*(__stdcall*)(const wchar_t*);
using DoPostMessageFn = void(__stdcall*)(const wchar_t*);
using DoListenMessageFn = const wchar_t*(__stdcall*)(int64_t);
using GetLevelsFn = const wchar_t*(__stdcall*)(const wchar_t*, int64_t);
using GetSignalCacheFn = const wchar_t*(__stdcall*)(const wchar_t*, const wchar_t*, int64_t);

std::wstring widen(std::string_view ascii) { return std::wstring(ascii.begin(), ascii.end()); }

/// LoadLibraryW wants backslashes; CMake hands over forward slashes.
std::wstring dll_path() {
    std::wstring path = MQLC_DLL_PATH;
    for (auto& c : path) c = c == L'/' ? L'\\' : c;
    return path;
}

template <class Fn>
Fn find(HMODULE module, const char* name) {
    return reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(module, name)));
}

TEST(DllExports, EveryExportWorksThroughGetProcAddress) {
    HMODULE dll = LoadLibraryW(dll_path().c_str());
    ASSERT_NE(dll, nullptr) << "LoadLibraryW failed: " << GetLastError();

    const auto init = find<InitLibFn>(dll, "InitLib");
    const auto destroy = find<DestroyLibFn>(dll, "DestroyLib");
    const auto send = find<DoSendMessageFn>(dll, "DoSendMessage");
    const auto post = find<DoPostMessageFn>(dll, "DoPostMessage");
    const auto listen = find<DoListenMessageFn>(dll, "DoListenMessage");
    const auto levels = find<GetLevelsFn>(dll, "GetLevels");
    const auto cache = find<GetSignalCacheFn>(dll, "GetSignalCache");
    ASSERT_TRUE(init && destroy && send && post && listen && levels && cache);

    // Before InitLib.
    EXPECT_STREQ(levels(L"EURUSD", 1), L"Failed to initialize MQLConnector.dll");
    EXPECT_STREQ(send(nullptr), L"");
    EXPECT_STREQ(listen(1), L"");

    mqlc::tools::LoopbackServer server(0, fincore_like());
    ASSERT_TRUE(server.listening());
    const std::uint16_t port = server.port();
    // MQL5Utils sends numbers, CBacktestDecisionClient strings: both must work.
    ASSERT_EQ(init(widen(init_json(port, 21)).c_str()), 1);
    ASSERT_EQ(init(widen(R"({"ChartId":"22","Port":")" + std::to_string(port) + R"("})").c_str()), 1);

    const std::wstring reply = send(L"{\"Id\":1001,\"ChartId\":21,\"Data\":\"\u20AC \U0001F600\"}");
    EXPECT_NE(reply.find(L"\"ChartId\":21"), std::wstring::npos) << reply.size();
    EXPECT_NE(reply.find(L"\u20AC \U0001F600"), std::wstring::npos);  // UTF-16 -> UTF-8 -> UTF-16

    EXPECT_STREQ(levels(L"EURUSD", 21), L"");  // fincore_like echoes the (empty) Data
    EXPECT_NE(std::wstring(cache(L"key", L"EURUSD", 21)).find(L"\"Id\":1033"), std::wstring::npos);

    post(LR"({"Id":1001,"ChartId":22,"Data":"posted"})");
    std::wstring heard;
    ASSERT_TRUE(eventually([&] { return !(heard = listen(22)).empty(); }));
    EXPECT_NE(heard.find(L"posted"), std::wstring::npos);

    destroy(22);
    destroy(21);
    EXPECT_STREQ(levels(L"EURUSD", 21), L"Failed to initialize MQLConnector.dll");

    // InitLib pinned the module: FreeLibrary must not unmap it while threads may still run.
    FreeLibrary(dll);
    HMODULE still = nullptr;
    EXPECT_TRUE(GetModuleHandleExW(0, dll_path().c_str(), &still));
    if (still != nullptr) FreeLibrary(still);
}

}  // namespace
