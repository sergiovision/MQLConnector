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
#import

#endif // MQLCONNECTOR_MQH
