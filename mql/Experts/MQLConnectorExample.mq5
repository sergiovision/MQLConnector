//+------------------------------------------------------------------+
//|                                          MQLConnectorExample.mq5 |
//|      MQLConnector example: request/reply and pushed messages     |
//+------------------------------------------------------------------+
#property strict
#property description "Talks to examples/python/server.py (or examples/csharp) through MQLConnector.dll."
#property description "Every timer tick sends a correlated request and prints the reply; frames the"
#property description "server pushes in between are drained with Poll() (DoListenMessage)."

#include <MQLConnector/CMqlConnector.mqh>

input string InpHost    = "127.0.0.1"; // Server host
input int    InpPort    = 2023;        // Server port
input int    InpTimerMs = 1000;        // Request interval, ms

CMqlConnector g_conn;
long          g_reqId = 0;

int OnInit()
  {
   PrintFormat("MQLConnector %s", g_conn.Version());
   // generic: no FinCore extras. ReqId correlates each reply with its request.
   const string opts = "\"Profile\":\"generic\",\"CorrelationKey\":\"ReqId\",\"WaitTimeout\":2000";
   if(g_conn.Open(InpHost, InpPort, ChartID(), opts))
      PrintFormat("Connected to %s:%d, route %I64d", InpHost, InpPort, ChartID());
   else
      PrintFormat("%s:%d not reachable yet; the DLL keeps retrying in the background", InpHost, InpPort);
   EventSetMillisecondTimer(InpTimerMs);
   return INIT_SUCCEEDED;
  }

void OnDeinit(const int reason)
  {
   EventKillTimer();
   g_conn.Close();
  }

void OnTimer()
  {
   g_reqId++;
   const string request = StringFormat(
      "{\"ChartId\":%I64d,\"ReqId\":%I64d,\"Type\":\"ping\",\"Symbol\":\"%s\",\"Bid\":%s}",
      ChartID(), g_reqId, _Symbol, DoubleToString(SymbolInfoDouble(_Symbol, SYMBOL_BID), _Digits));
   const ulong started = GetMicrosecondCount();
   const string reply = g_conn.Request(request);
   if(reply == "")
      PrintFormat("ReqId %I64d: no reply", g_reqId);
   else
      PrintFormat("ReqId %I64d: reply in %I64u us: %s", g_reqId, GetMicrosecondCount() - started, reply);

   // Frames the server pushed meanwhile (broadcasts) waited in this route's mailbox.
   for(string pushed = g_conn.Poll(); pushed != ""; pushed = g_conn.Poll())
      Print("pushed: ", pushed);
  }
