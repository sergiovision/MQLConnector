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

/* Opens (first call) or shares the process-wide connection. 1 = connected, 0 = not (yet). */
int32_t MQLC_CALL InitLib(const wchar_t* json) MQLC_NOEXCEPT;
/* Releases one InitLib reference and the chart's mailbox; the last one closes the connection. */
void MQLC_CALL DestroyLib(int64_t chart_id) MQLC_NOEXCEPT;
/* Sends a SignalInfo JSON and waits up to WaitTimeout for its chart's next frame ("" if none). */
const wchar_t* MQLC_CALL DoSendMessage(const wchar_t* json) MQLC_NOEXCEPT;
/* Sends a SignalInfo JSON without waiting. */
void MQLC_CALL DoPostMessage(const wchar_t* json) MQLC_NOEXCEPT;
/* The next frame queued for `key` (a chart id), or "" - never blocks. */
const wchar_t* MQLC_CALL DoListenMessage(int64_t key) MQLC_NOEXCEPT;
/* SIGNAL_LEVELS4SYMBOL: the levels text from the reply's Data. */
const wchar_t* MQLC_CALL GetLevels(const wchar_t* sym, int64_t chart_id) MQLC_NOEXCEPT;
/* SIGNAL_GET_SIGNAL_FROMCACHE: the whole reply frame. */
const wchar_t* MQLC_CALL GetSignalCache(const wchar_t* key, const wchar_t* sym,
                                        int64_t chart_id) MQLC_NOEXCEPT;

#ifdef __cplusplus
}
#endif

#endif /* MQLCONNECTOR_MQLCONNECTOR_H */
