//+------------------------------------------------------------------+
//|                                              ConnectorBench.mq5 |
//|                                                 CapitalTools LLC |
//+------------------------------------------------------------------+
#property copyright "CapitalTools LLC"
#property version   "1.00"
#property script_show_inputs
#property strict

// Round-trip latency of MQLConnector.dll as MQL sees it: N DoSendMessage calls against
// tools/echo_server (default port 2023), then p50/p99/p99.9 and the total.
// Deploy: this file to <data>\MQL5\Scripts\, mql\Include\MQLConnector.mqh to
// <data>\MQL5\Include\, the DLL to <data>\MQL5\Libraries\.

#include <MQLConnector.mqh>

input string InpHost = "127.0.0.1";   // Echo server host
input int    InpPort = 2023;          // Echo server port
input int    InpRoundTrips = 1000;    // Round trips
input int    InpWaitTimeout = 3000;   // WaitTimeout, ms

double Percentile(const double &sorted[], double p)
{
   int n = ArraySize(sorted);
   int i = (int)MathRound(p * (n - 1));
   return sorted[MathMin(MathMax(i, 0), n - 1)];
}

void OnStart()
{
   long chartId = ChartID();
   string init = StringFormat("{\"Host\":\"%s\",\"Port\":%d,\"Account\":\"%I64d\",\"ChartId\":%I64d,"
                              "\"WaitTimeout\":%d}",
                              InpHost, InpPort, AccountInfoInteger(ACCOUNT_LOGIN), chartId, InpWaitTimeout);
   if (MQLConnector::InitLib(init) <= 0)
   {
      PrintFormat("ConnectorBench: cannot connect to %s:%d", InpHost, InpPort);
      MQLConnector::DestroyLib(chartId);
      return;
   }

   int n = MathMax(InpRoundTrips, 1);
   double micros[];
   ArrayResize(micros, n);
   int failures = 0;
   ulong begin = GetMicrosecondCount();
   for (int i = 0; i < n && !IsStopped(); i++)
   {
      string msg = StringFormat("{\"Id\":1001,\"ChartId\":%I64d,\"Sym\":\"BENCH\",\"Data\":\"%d\"}", chartId, i);
      ulong t0 = GetMicrosecondCount();
      string reply = MQLConnector::DoSendMessage(msg);
      micros[i] = (double)(GetMicrosecondCount() - t0);
      if (StringLen(reply) == 0)
         failures++;
   }
   double totalMs = (GetMicrosecondCount() - begin) / 1000.0;
   MQLConnector::DestroyLib(chartId);

   ArraySort(micros);
   PrintFormat("ConnectorBench: %d round trips, %d failures", n, failures);
   PrintFormat("ConnectorBench: p50 %.1f us, p99 %.1f us, p99.9 %.1f us, max %.1f us, total %.1f ms",
               Percentile(micros, 0.50), Percentile(micros, 0.99), Percentile(micros, 0.999),
               micros[n - 1], totalMs);
}
