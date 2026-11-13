// Windows only: loads the built DLL the way MT5 does (LoadLibraryW + GetProcAddress) and calls
// every export against the loopback server.
#include "fbe/fbe_message.h"
#include "mqlconnector/version.h"
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
#include <optional>
#include <string>
#include <vector>

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
using GetVersionFn = const wchar_t*(__stdcall*)();
using DoSendBinaryFn = int32_t(__stdcall*)(const unsigned char*, int32_t, unsigned char*, int32_t);
using DoFetchBinaryFn = int32_t(__stdcall*)(unsigned char*, int32_t);
using DoPostBinaryFn = void(__stdcall*)(const unsigned char*, int32_t);
using DoListenBinaryFn = int32_t(__stdcall*)(int64_t, unsigned char*, int32_t);

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
    const auto version = find<GetVersionFn>(dll, "GetVersion");
    const auto send_binary = find<DoSendBinaryFn>(dll, "DoSendBinary");
    const auto fetch_binary = find<DoFetchBinaryFn>(dll, "DoFetchBinary");
    const auto post_binary = find<DoPostBinaryFn>(dll, "DoPostBinary");
    const auto listen_binary = find<DoListenBinaryFn>(dll, "DoListenBinary");
    ASSERT_TRUE(init && destroy && send && post && listen && levels && cache && version);
    ASSERT_TRUE(send_binary && fetch_binary && post_binary && listen_binary);

    // Before InitLib: GetVersion needs no session.
    EXPECT_STREQ(version(), MQLC_VERSION_W);
    EXPECT_STREQ(version(), L"1.1.0");
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

    // A new connection lifetime with the generic profile: no FinCore extras.
    ASSERT_EQ(init(widen(R"({"ChartId":23,"Profile":"generic","Port":)" + std::to_string(port) + "}").c_str()), 1);
    EXPECT_STREQ(levels(L"EURUSD", 23), L"");
    EXPECT_STREQ(cache(L"key", L"EURUSD", 23), L"");
    const std::wstring generic_reply = send(L"{\"Id\":1001,\"ChartId\":23,\"Data\":\"g\"}");
    EXPECT_NE(generic_reply.find(L"\"ChartId\":23"), std::wstring::npos);
    EXPECT_STREQ(version(), MQLC_VERSION_W);
    destroy(23);

    // A third lifetime on Protocol "fbe": the four binary exports against a length-framed mirror.
    {
        mqlc::core::FramingOptions length;
        length.kind = mqlc::core::FramingKind::length;
        mqlc::tools::LoopbackServer fbe_server(
            0, [](std::string_view request) { return std::optional<std::string>(request); }, length);
        ASSERT_TRUE(fbe_server.listening());
        ASSERT_EQ(init(widen(R"({"ChartId":24,"Protocol":"fbe","Port":)" + std::to_string(fbe_server.port()) + "}")
                           .c_str()),
                  1);
        fbe::TestMessage m;
        m.chart_id = 24;
        m.req_id = 77;
        m.id = 1039;
        m.payload = std::string(1000, 'z');
        const std::string request = fbe::encode(m);
        const auto* req = reinterpret_cast<const unsigned char*>(request.data());
        const auto req_len = static_cast<int32_t>(request.size());

        std::vector<unsigned char> buffer(4096);
        ASSERT_EQ(send_binary(req, req_len, buffer.data(), static_cast<int32_t>(buffer.size())), req_len);
        EXPECT_EQ(std::string(reinterpret_cast<const char*>(buffer.data()), request.size()), request);

        // Too small: -N, the message is kept for DoFetchBinary.
        std::vector<unsigned char> small(16);
        m.req_id = 78;
        const std::string second = fbe::encode(m);
        EXPECT_EQ(send_binary(reinterpret_cast<const unsigned char*>(second.data()), req_len, small.data(), 16),
                  -req_len);
        EXPECT_EQ(fetch_binary(small.data(), 16), -req_len);  // still too small: kept
        ASSERT_EQ(fetch_binary(buffer.data(), static_cast<int32_t>(buffer.size())), req_len);
        EXPECT_EQ(std::string(reinterpret_cast<const char*>(buffer.data()), second.size()), second);
        EXPECT_EQ(fetch_binary(buffer.data(), static_cast<int32_t>(buffer.size())), 0);  // nothing kept

        m.req_id = 0;
        m.payload = "posted";
        const std::string posted = fbe::encode(m);
        post_binary(reinterpret_cast<const unsigned char*>(posted.data()), static_cast<int32_t>(posted.size()));
        int32_t got = 0;
        ASSERT_TRUE(eventually([&] {
            return (got = listen_binary(24, buffer.data(), static_cast<int32_t>(buffer.size()))) > 0;
        }));
        EXPECT_EQ(std::string(reinterpret_cast<const char*>(buffer.data()), static_cast<std::size_t>(got)), posted);

        // Bad input never crosses the ABI as an exception.
        EXPECT_EQ(send_binary(nullptr, 10, buffer.data(), 10), 0);
        EXPECT_EQ(send_binary(req, req_len - 1, buffer.data(), static_cast<int32_t>(buffer.size())), 0);
        EXPECT_EQ(listen_binary(24, nullptr, 0), 0);
        EXPECT_STREQ(send(L"{\"ChartId\":24}"), L"");  // JSON on the binary protocol
        destroy(24);
    }

    // InitLib pinned the module: FreeLibrary must not unmap it while threads may still run.
    FreeLibrary(dll);
    HMODULE still = nullptr;
    EXPECT_TRUE(GetModuleHandleExW(0, dll_path().c_str(), &still));
    if (still != nullptr) FreeLibrary(still);
}

}  // namespace
