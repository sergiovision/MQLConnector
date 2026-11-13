//+------------------------------------------------------------------+
//|                                          MQLConnector.mqh |
//|                                                 CapitalTools LLC |
//+------------------------------------------------------------------+
#property copyright "CapitalTools LLC"
#property strict

// Imports MQLConnector.dll. Copy this file to <data>\MQL5\Include\ and the DLL to
// <data>\MQL5\Libraries\, enable "Allow DLL imports", then call the functions qualified with
// the module name, e.g. MQLConnector::DoSendMessage(json). For a class wrapper see
// MQLConnector\CMqlConnector.mqh.

#ifndef MQLCONNECTOR_MQH
#define MQLCONNECTOR_MQH

#import "MQLConnector.dll"
int    InitLib(string json);                        // 1 connected, 0 not (yet)
void   DestroyLib(long chartId);
string DoSendMessage(string json);                  // send, wait up to WaitTimeout for the route's reply
void   DoPostMessage(string json);                  // send, don't wait
string DoListenMessage(long key);                   // next queued frame for a route, or ""
string GetLevels(string sym, long chartId);         // fincore profile: SIGNAL_LEVELS4SYMBOL -> reply Data
string GetSignalCache(string key, string sym, long chartId);  // fincore profile only
string GetVersion();                                // "1.1.0"; callable before InitLib
// Binary messages (InitLib "Protocol":"fbe"): one whole FBE final-model message per call, its u32
// size first; the body starts with the envelope ChartId:long, ReqId:long (0 = uncorrelated), Id:int.
// Results: N > 0 bytes copied, 0 nothing (timeout / bad input), -N needs N bytes (then DoFetchBinary
// with an array of at least N). ArrayResize reply[] to replyCap before the call.
int    DoSendBinary(const uchar &req[], int reqLen, uchar &reply[], int replyCap);
int    DoFetchBinary(uchar &reply[], int replyCap);
void   DoPostBinary(const uchar &req[], int reqLen);
int    DoListenBinary(long key, uchar &buf[], int cap);
#import

#endif // MQLCONNECTOR_MQH
