// A reference server for MQLConnector on .NET 8 with NetCoreServer (NuGet). The same behaviour as
// examples/python/server.py: every frame is one UTF-8 JSON object; each request is answered on
// its connection with a reply carrying the request's route member (ChartId) and correlation
// member (ReqId); --broadcast pushes a frame to every route seen. See docs/PROTOCOL.md.
//
//   dotnet run -- [--host 127.0.0.1] [--port 2023] [--framing delimiter|length] [--delimiter 26]
//                 [--route-key ChartId] [--correlation-key ReqId] [--broadcast SECONDS]
using System.Buffers.Binary;
using System.Collections.Concurrent;
using System.Net;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using NetCoreServer;

var options = Options.Parse(args);
var server = new MqlcServer(options) { OptionNoDelay = true };
if (!server.Start())
{
    Console.Error.WriteLine($"cannot listen on {options.Host}:{options.Port}");
    return 1;
}
Console.WriteLine($"listening on {options.Host}:{options.Port}, " +
                  (options.Length ? "length prefix" : $"delimiter {options.Delimiter}") + " framing (Ctrl+C to stop)");

using var timer = options.BroadcastSeconds > 0
    ? new Timer(_ => server.BroadcastAll(), null, TimeSpan.FromSeconds(options.BroadcastSeconds),
                TimeSpan.FromSeconds(options.BroadcastSeconds))
    : null;
using var quit = new ManualResetEventSlim();
Console.CancelKeyPress += (_, e) => { e.Cancel = true; quit.Set(); };
quit.Wait();
server.Stop();
return 0;

sealed record Options(string Host, int Port, bool Length, byte Delimiter, string RouteKey, string CorrelationKey,
                      double BroadcastSeconds)
{
    public static Options Parse(string[] args)
    {
        var o = new Options("127.0.0.1", 2023, false, 26, "ChartId", "ReqId", 0);
        for (var i = 0; i + 1 < args.Length; i += 2)
        {
            var value = args[i + 1];
            o = args[i] switch
            {
                "--host" => o with { Host = value },
                "--port" => o with { Port = int.Parse(value) },
                "--framing" => o with { Length = value == "length" },
                "--delimiter" => o with { Delimiter = byte.Parse(value) },
                "--route-key" => o with { RouteKey = value },
                "--correlation-key" => o with { CorrelationKey = value },
                "--broadcast" => o with { BroadcastSeconds = double.Parse(value, System.Globalization.CultureInfo.InvariantCulture) },
                _ => throw new ArgumentException($"unknown option {args[i]}"),
            };
        }
        if (o.Delimiter is < 1 or > 31) throw new ArgumentException("--delimiter must be 1..31");
        return o;
    }
}

sealed class MqlcServer(Options options) : TcpServer(IPAddress.Parse(options.Host), options.Port)
{
    public const int MaxFrameBytes = 16 * 1024 * 1024;  // the connector's cap
    public Options Options { get; } = options;
    readonly ConcurrentDictionary<Guid, MqlcSession> _sessions = new();
    long _sequence;

    protected override TcpSession CreateSession() => new MqlcSession(this);

    public void Register(MqlcSession session) => _sessions[session.Id] = session;
    public void Unregister(MqlcSession session) => _sessions.TryRemove(session.Id, out _);

    /// One frame: UTF-8 JSON (System.Text.Json escapes every control character, so the payload
    /// never contains a delimiter byte 1..31), then the framing.
    public byte[] Frame(JsonObject message)
    {
        var payload = Encoding.UTF8.GetBytes(message.ToJsonString());
        if (!Options.Length) return [.. payload, Options.Delimiter];
        var framed = new byte[payload.Length + 4];
        BinaryPrimitives.WriteUInt32LittleEndian(framed, (uint)framed.Length);  // includes the prefix
        payload.CopyTo(framed, 4);
        return framed;
    }

    public void BroadcastAll()
    {
        var sequence = Interlocked.Increment(ref _sequence);
        foreach (var session in _sessions.Values) session.Broadcast(sequence);
    }
}

sealed class MqlcSession(MqlcServer server) : TcpSession(server)
{
    const int GetMainLogPath = 1026;  // sent by the connector's default "fincore" profile only
    readonly MqlcServer _server = server;
    readonly List<byte> _pending = [];
    readonly ConcurrentDictionary<string, JsonNode?> _routes = new();

    protected override void OnConnected()
    {
        _server.Register(this);
        Console.WriteLine($"client {Socket.RemoteEndPoint} connected");
    }

    protected override void OnDisconnected()
    {
        _server.Unregister(this);
        Console.WriteLine($"client {Id} disconnected");
    }

    protected override void OnReceived(byte[] buffer, long offset, long size)
    {
        _pending.AddRange(new ArraySegment<byte>(buffer, (int)offset, (int)size));
        while (TryTakeFrame(out var payload))
            Handle(payload);
    }

    bool TryTakeFrame(out byte[] payload)
    {
        payload = [];
        var o = _server.Options;
        if (o.Length)
        {
            if (_pending.Count < 4) return false;
            var total = BinaryPrimitives.ReadUInt32LittleEndian(_pending.GetRange(0, 4).ToArray());
            if (total < 4 || total - 4 > MqlcServer.MaxFrameBytes)
            {
                Console.WriteLine($"invalid length prefix {total}: closing {Id}");  // cannot resynchronise
                _pending.Clear();
                Disconnect();
                return false;
            }
            if (_pending.Count < total) return false;
            payload = _pending.GetRange(4, (int)total - 4).ToArray();
            _pending.RemoveRange(0, (int)total);
            return true;
        }
        var end = _pending.IndexOf(o.Delimiter);
        if (end < 0)
        {
            if (_pending.Count > MqlcServer.MaxFrameBytes) _pending.Clear();  // oversized: drop it
            return false;
        }
        payload = _pending.GetRange(0, end).ToArray();
        _pending.RemoveRange(0, end + 1);
        return true;
    }

    void Handle(byte[] payload)
    {
        JsonObject? request;
        try
        {
            request = JsonNode.Parse(payload) as JsonObject;
        }
        catch (JsonException e)
        {
            Console.WriteLine($"bad frame: {e.Message}");
            return;
        }
        if (request is null) return;
        var o = _server.Options;
        var route = request[o.RouteKey]?.DeepClone() ?? JsonValue.Create(0);
        _routes[route.ToJsonString()] = route;
        Console.WriteLine($"<- {Encoding.UTF8.GetString(payload)}");

        var reply = new JsonObject { [o.RouteKey] = route.DeepClone() };
        if (request[o.CorrelationKey] is { } corr) reply[o.CorrelationKey] = corr.DeepClone();  // echo it
        if (request["Id"]?.GetValueKind() == JsonValueKind.Number && request["Id"]!.GetValue<long>() == GetMainLogPath)
        {
            reply["Id"] = GetMainLogPath;
            reply["Data"] = "";  // "" = keep the connector's own log file
        }
        else
        {
            reply["Type"] = "reply";
            reply["ServerTime"] = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds() / 1000.0;
            reply["Echo"] = request.DeepClone();
        }
        SendAsync(_server.Frame(reply));
    }

    public void Broadcast(long sequence)
    {
        var o = _server.Options;
        foreach (var route in _routes.Values)
        {
            SendAsync(_server.Frame(new JsonObject
            {
                [o.RouteKey] = route?.DeepClone(),
                ["Type"] = "broadcast",
                ["Seq"] = sequence,
                ["ServerTime"] = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds() / 1000.0,
            }));
        }
    }
}
