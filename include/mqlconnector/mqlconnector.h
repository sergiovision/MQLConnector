/*
 * MQLConnector - the C ABI that MetaTrader 5 imports.
 *
 *   #import "MQLConnector.dll"
 *   int    InitLib(string json);
 *   ...
 *   #import
 *
 * MQL `string` arrives as a NUL-terminated UTF-16 `const wchar_t*`; `long` is int64, `int` is
 * int32. A returned string stays valid until the next call of the same function on the same
 * thread (MT5 copies it immediately). No function ever throws; failures return the neutral value
 * ("" / 0) and are logged.
 */
#ifndef MQLCONNECTOR_MQLCONNECTOR_H
#define MQLCONNECTOR_MQLCONNECTOR_H

#include <stdint.h>
#include <wchar.h>

#if defined(_WIN32)
#define MQLC_CALL __stdcall /* what MT5 uses; the same as the only x64 convention */
#else
#define MQLC_CALL
#endif

#ifdef __cplusplus
#define MQLC_NOEXCEPT noexcept
extern "C" {
#else
#define MQLC_NOEXCEPT
#endif

/* Opens (first call) or shares the process-wide connection. 1 = connected, 0 = not (yet).
 * The first call configures the connection; see README for the keys (Profile, RouteKey,
 * CorrelationKey, Framing, ...). */
int32_t MQLC_CALL InitLib(const wchar_t* json) MQLC_NOEXCEPT;
/* Releases one InitLib reference and the chart's mailbox; the last one closes the connection. */
void MQLC_CALL DestroyLib(int64_t chart_id) MQLC_NOEXCEPT;
/* Sends a JSON object and waits up to WaitTimeout for the reply on its route: the route's next
 * frame or, with CorrelationKey, the first one carrying the request's value ("" if none). */
const wchar_t* MQLC_CALL DoSendMessage(const wchar_t* json) MQLC_NOEXCEPT;
/* Sends a JSON object without waiting. */
void MQLC_CALL DoPostMessage(const wchar_t* json) MQLC_NOEXCEPT;
/* The next frame queued for route `key` (a chart id by default), or "" - never blocks. */
const wchar_t* MQLC_CALL DoListenMessage(int64_t key) MQLC_NOEXCEPT;
/* fincore profile: SIGNAL_LEVELS4SYMBOL, the levels text from the reply's Data. generic: "". */
const wchar_t* MQLC_CALL GetLevels(const wchar_t* sym, int64_t chart_id) MQLC_NOEXCEPT;
/* fincore profile: SIGNAL_GET_SIGNAL_FROMCACHE, the whole reply frame. generic: "". */
const wchar_t* MQLC_CALL GetSignalCache(const wchar_t* key, const wchar_t* sym,
                                        int64_t chart_id) MQLC_NOEXCEPT;
/* The DLL's version, e.g. L"1.1.0". A static string: valid forever, callable before InitLib.
 * Exported under the name "GetVersion" (MQL: `string GetVersion();`). The C name differs only
 * because <windows.h> already declares a GetVersion; resolve it with
 * GetProcAddress(module, "GetVersion") as an MQLC_GetVersionFn. */
const wchar_t* MQLC_CALL MQLC_GetVersion(void) MQLC_NOEXCEPT;
typedef const wchar_t*(MQLC_CALL* MQLC_GetVersionFn)(void);

#ifdef __cplusplus
}
#endif

#endif /* MQLCONNECTOR_MQLCONNECTOR_H */
