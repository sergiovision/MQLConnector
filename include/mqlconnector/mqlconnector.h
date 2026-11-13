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

/* ---- Binary messages (InitLib "Protocol":"fbe": length framing, FbeCodec) ----
 * A message is one whole Fast Binary Encoding final-model message: u32 LE size (counting its own
 * 4 bytes, and equal to the length passed) | u32 type | fields. Its body must start with the
 * envelope ChartId:int64 (route), ReqId:int64 (correlation, 0 = none), Id:int32.
 * MQL: `int DoSendBinary(const uchar &req[], int reqLen, uchar &reply[], int replyCap);` etc. -
 * MT5 passes the arrays' data pointers.
 *
 * Results that return a message: its length (> 0) when it was copied to the buffer; 0 when there
 * is none (timeout, nothing queued, bad input); -N when it needs N bytes: the message is then
 * kept in a per-thread slot, and DoFetchBinary copies it out with a buffer of at least N. */

/* Sends `request` and waits up to WaitTimeout for the reply with the same ReqId (or, with ReqId 0,
 * the route's next message). */
int32_t MQLC_CALL DoSendBinary(const unsigned char* request, int32_t request_length, unsigned char* reply,
                               int32_t reply_capacity) MQLC_NOEXCEPT;
/* Copies the message kept by the last negative result on this thread (then forgets it). */
int32_t MQLC_CALL DoFetchBinary(unsigned char* reply, int32_t reply_capacity) MQLC_NOEXCEPT;
/* Sends `request` without waiting. */
void MQLC_CALL DoPostBinary(const unsigned char* request, int32_t request_length) MQLC_NOEXCEPT;
/* The next message queued for route `key`, never blocks. */
int32_t MQLC_CALL DoListenBinary(int64_t key, unsigned char* buffer, int32_t capacity) MQLC_NOEXCEPT;

#ifdef __cplusplus
}
#endif

#endif /* MQLCONNECTOR_MQLCONNECTOR_H */
