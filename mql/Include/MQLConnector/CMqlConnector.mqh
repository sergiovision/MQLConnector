//+------------------------------------------------------------------+
//|                                                CMqlConnector.mqh |
//|                 MQLConnector: MetaTrader 5 <-> your TCP server     |
//+------------------------------------------------------------------+
#property strict

// A small class around MQLConnector.dll for MQL5 programs that talk to their own server.
//
//   #include <MQLConnector/CMqlConnector.mqh>
//   CMqlConnector conn;
//   conn.Open("127.0.0.1", 2023, ChartID(), "\"Profile\":\"generic\",\"CorrelationKey\":\"ReqId\"");
//   string reply = conn.Request("{\"ChartId\":" + IntegerToString(ChartID()) + ",\"ReqId\":1}");
//   string pushed = conn.Poll();
//   conn.Close();
//
// Every frame is one JSON object. The DLL routes incoming frames by their route member (RouteKey,
// "ChartId" by default) to the program that opened that route id; Request() waits for the reply
// on the request's own route. With CorrelationKey set, Request() returns only the frame that
// echoes the request's correlation member, and other frames stay queued for Poll().
//
// One connection per terminal: the first Open() in the terminal configures it (host, port,
// options); later Open() calls from other programs share it. See README "Limitations".
//
// The include guard differs from FinCore's FINCORE_MQLCONNECTOR_MQH, so both can coexist.

#ifndef MQLCONNECTOR_CMQLCONNECTOR_MQH
#define MQLCONNECTOR_CMQLCONNECTOR_MQH

#include <MQLConnector.mqh>

class CMqlConnector
  {
private:
   long              m_routeId;
   bool              m_open;
   bool              m_correlated;
   uint              m_waitTimeoutMs;

   //--- A reply that did not fit: grow the array (with headroom) and copy the kept message.
   int               Fetch(uchar &buf[], const int need)
     {
      ArrayResize(buf, need, need);
      int n = MQLConnector::DoFetchBinary(buf, ArraySize(buf));
      return n > 0 ? n : 0;
     }

public:
                     CMqlConnector(void) : m_routeId(0), m_open(false), m_correlated(false), m_waitTimeoutMs(3000) {}
                    ~CMqlConnector(void) { Close(); }

   //--- Opens (or shares) the terminal's connection and subscribes routeId (usually ChartID()).
   //--- opts: extra InitLib members as JSON text without braces, e.g.
   //---   "\"Profile\":\"generic\",\"CorrelationKey\":\"ReqId\",\"WaitTimeout\":2000"
   //--- Returns true when connected. False means "not yet": the DLL keeps reconnecting in the
   //--- background, so Request()/Post() start working once the server is up. Call Close() either way.
   bool              Open(const string host, const int port, const long routeId, const string opts = "")
     {
      Close();
      string json = StringFormat("{\"Host\":\"%s\",\"Port\":%d,\"ChartId\":%I64d", host, port, routeId);
      if(opts != "")
         json += "," + opts;
      json += "}";
      m_routeId = routeId;
      m_open = true;             // InitLib counts a reference even when the connect fails
      m_correlated = StringFind(opts, "\"CorrelationKey\"") >= 0;
      int at = StringFind(opts, "\"WaitTimeout\":");
      if(at >= 0)
         m_waitTimeoutMs = (uint)StringToInteger(StringSubstr(opts, at + 14));
      return MQLConnector::InitLib(json) == 1;
     }

   //--- Sends one JSON object and waits for its reply (up to the connection's WaitTimeout).
   //--- timeoutMs > WaitTimeout keeps polling for the reply until timeoutMs; this only applies
   //--- without CorrelationKey (with it, a late reply cannot be told apart by Poll()).
   //--- Returns "" when no reply came in time.
   string            Request(const string json, const uint timeoutMs = 0)
     {
      if(!m_open)
         return "";
      uint started = GetTickCount();
      string reply = MQLConnector::DoSendMessage(json);
      if(reply != "" || m_correlated || timeoutMs <= m_waitTimeoutMs)
         return reply;
      while(GetTickCount() - started < timeoutMs && !IsStopped())
        {
         reply = MQLConnector::DoListenMessage(m_routeId);
         if(reply != "")
            return reply;
         Sleep(5);
        }
      return "";
     }

   //--- Sends one JSON object without waiting.
   void              Post(const string json) { if(m_open) MQLConnector::DoPostMessage(json); }

   //--- The next frame the server pushed to this route (or a late reply), or "". Never blocks.
   string            Poll(void) { return m_open ? MQLConnector::DoListenMessage(m_routeId) : ""; }

   //--- Releases this program's reference; the last one in the terminal closes the connection.
   void              Close(void)
     {
      if(!m_open)
         return;
      MQLConnector::DestroyLib(m_routeId);
      m_open = false;
     }

   //--- Protocol "fbe" (Open(..., "\"Protocol\":\"fbe\"")): binary FBE final-model messages.
   //--- `req` holds reqLen bytes (the message's own u32 size first). The reply lands in `reply`,
   //--- which is grown when needed and kept between calls, so steady traffic never reallocates.
   //--- Returns the reply's length, or 0 when none came within WaitTimeout.
   int               RequestBinary(const uchar &req[], const int reqLen, uchar &reply[])
     {
      if(!m_open || reqLen <= 0 || reqLen > ArraySize(req))   // the DLL reads reqLen bytes of req
         return 0;
      if(ArraySize(reply) < 256)
         ArrayResize(reply, 4096);
      int n = MQLConnector::DoSendBinary(req, reqLen, reply, ArraySize(reply));
      return n < 0 ? Fetch(reply, -n) : n;
     }

   //--- Sends one binary message without waiting.
   void              PostBinary(const uchar &req[], const int reqLen)
     {
      if(m_open && reqLen > 0 && reqLen <= ArraySize(req))   // the DLL reads reqLen bytes of req
         MQLConnector::DoPostBinary(req, reqLen);
     }

   //--- The next binary message queued for this route (its length), or 0. Never blocks.
   int               PollBinary(uchar &buf[])
     {
      if(!m_open)
         return 0;
      if(ArraySize(buf) < 256)
         ArrayResize(buf, 4096);
      int n = MQLConnector::DoListenBinary(m_routeId, buf, ArraySize(buf));
      return n < 0 ? Fetch(buf, -n) : n;
     }

   bool              IsOpen(void) const { return m_open; }
   long              RouteId(void) const { return m_routeId; }
   string            Version(void) const { return MQLConnector::GetVersion(); }
  };

#endif // MQLCONNECTOR_CMQLCONNECTOR_MQH
