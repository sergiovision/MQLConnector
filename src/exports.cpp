// The DLL's C ABI: UTF-16 <-> UTF-8 conversion and exception containment around the session.
// Nothing else belongs here.
#include "mqlconnector/mqlconnector.h"

#include "connector/session.h"
#include "core/utf.h"
#include "mqlconnector/version.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <exception>
#include <string>
#include <string_view>

static_assert(sizeof(wchar_t) == 2, "MQL strings are UTF-16: the DLL is built for Windows only");

namespace {

using mqlc::connector::process_session;

std::string from_mql(const wchar_t* text) {
    return text == nullptr ? std::string() : mqlc::utf::to_utf8(std::wstring_view(text));
}

/// Converts into the caller's per-thread buffer; the pointer lives until the next conversion
/// into that buffer on this thread.
const wchar_t* to_mql(std::wstring& buffer, std::string_view utf8) {
    buffer.clear();
    mqlc::utf::append_utf16(utf8, buffer);
    return buffer.c_str();
}

void log_failure(const char* function, const char* what) noexcept {
    try {
        process_session().logger().log(mqlc::core::LogLevel::error,
                                       std::string("[") + function + "]: Exception: " + what);
    } catch (...) {
    }
}

/// Runs `body`, turning any exception into a log line and `fallback`: nothing may cross the
/// C ABI into MT5.
template <class Result, class Body>
Result guarded(const char* function, Result fallback, Body&& body) noexcept {
    try {
        return body();
    } catch (const std::exception& e) {
        log_failure(function, e.what());
    } catch (...) {
        log_failure(function, "unknown exception");
    }
    return fallback;
}

/// Live EAs never call DestroyLib and MT5 may FreeLibrary the DLL while the reader thread still
/// runs its code. Pinning keeps the module mapped until the process exits.
void pin_module() noexcept {
    static const bool pinned = [] {
        static const char anchor = 0;  // any address inside this module
        HMODULE self = nullptr;
        return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                                  reinterpret_cast<LPCWSTR>(&anchor), &self) != FALSE;
    }();
    (void)pinned;
}

const wchar_t kEmpty[] = L"";

}  // namespace

extern "C" {

int32_t MQLC_CALL InitLib(const wchar_t* json) noexcept {
    return guarded("InitLib", int32_t{0}, [&] {
        pin_module();
        return static_cast<int32_t>(process_session().init(from_mql(json)));
    });
}

void MQLC_CALL DestroyLib(int64_t chart_id) noexcept {
    guarded("DestroyLib", 0, [&] {
        process_session().destroy(chart_id);
        return 0;
    });
}

const wchar_t* MQLC_CALL DoSendMessage(const wchar_t* json) noexcept {
    thread_local std::wstring result;
    return guarded("DoSendMessage", kEmpty, [&] {
        return to_mql(result, process_session().send_and_wait(from_mql(json)));
    });
}

void MQLC_CALL DoPostMessage(const wchar_t* json) noexcept {
    guarded("DoPostMessage", 0, [&] {
        process_session().post(from_mql(json));
        return 0;
    });
}

const wchar_t* MQLC_CALL DoListenMessage(int64_t key) noexcept {
    thread_local std::wstring result;
    return guarded("DoListenMessage", kEmpty, [&] {
        return to_mql(result, process_session().listen(key));
    });
}

const wchar_t* MQLC_CALL GetLevels(const wchar_t* sym, int64_t chart_id) noexcept {
    thread_local std::wstring result;
    return guarded("GetLevels", kEmpty, [&] {
        return to_mql(result, process_session().get_levels(from_mql(sym), chart_id));
    });
}

const wchar_t* MQLC_CALL GetSignalCache(const wchar_t* key, const wchar_t* sym, int64_t chart_id) noexcept {
    thread_local std::wstring result;
    return guarded("GetSignalCache", kEmpty, [&] {
        return to_mql(result, process_session().get_signal_cache(from_mql(key), from_mql(sym), chart_id));
    });
}

const wchar_t* MQLC_CALL MQLC_GetVersion() noexcept {
    // A literal: valid forever, touches no session, safe before InitLib.
    return MQLC_VERSION_W;
}

}  // extern "C"

// No DllMain: the CRT's default one is enough. Nothing runs at load or unload, so no thread is
// ever started or joined under the loader lock. DisableThreadLibraryCalls is deliberately not
// called: with the static CRT (/MT) the CRT itself needs DLL_THREAD_DETACH to free per-thread
// data, including the thread_local result buffers above.
