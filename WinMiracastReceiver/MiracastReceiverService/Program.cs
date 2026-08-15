using System;
using System.Collections.Generic;
using System.IO;
using System.Threading;
using System.Threading.Tasks;
using Windows.Media;
using Windows.Media.Core;
using Windows.Media.Miracast;
using Windows.Media.Playback;

namespace MiracastReceiverService
{
    /// <summary>
    /// Miracast 接收服务(桌面进程,无窗口 → 隐藏 D3D11 swap chain 渲染窗)。
    ///
    /// 链路: 投屏设备 → Wi-Fi Direct → MiracastReceiver → MediaPlayer(VideoFrameAvailable)
    ///       → CopyFrameToVideoSurface → SoftwareBitmap CPU 读回 → TCP 帧协议发给宿主 Qt
    ///
    /// 多路支持(2026-08-12):
    ///   Windows.Media.Miracast API 原生支持多路同时连接: MiracastReceiverSession.MaxSimultaneousConnections = N。
    ///   每个投屏连接触发独立 ConnectionCreated/MediaSourceCreated(args.Connection 区分),
    ///   为每连接创建独立 MediaPlayer + FrameServerSocket(独立帧端口)。
    ///   硬件上限: MiracastReceiverStatus.MaxSimultaneousConnections(启动时打印)。
    ///
    /// 用法(单路,向后兼容): MiracastReceiverService.exe --port 33890 --name 会话名
    /// 用法(多路):           MiracastReceiverService.exe --ports 33890,33891,33892,33893 --max 4 --name 会话名
    /// </summary>
    internal static class Program
    {
        private static MiracastReceiver _receiver;
        private static MiracastReceiverSession _session;

        private static string _sessionName = "miracast";
        private static List<int> _ports = new List<int>();
        private static int _maxConnections = 1;

        // 每路连接状态:连接 → (MediaPlayer + FrameServerSocket)
        private static readonly Dictionary<MiracastReceiverConnection, ConnectionState> _connections = new();
        internal static readonly object ConnectionsLock = new();   // 保护并发连接分配(多设备同时连入)

        // FrameServerSocket 发送循环用: 通道断开时判断是否还有其它存活连接。
        // >1 只结束本路(多路共享进程, 移除单路不能杀整组); ==1 视为宿主离开退出进程。
        internal static int ActiveConnectionCount
        {
            get { lock (ConnectionsLock) return _connections.Count; }
        }

        private sealed class ConnectionState : IDisposable
        {
            public int Index;
            public int Port;              // 帧端口(从 _ports 空闲分配, 连接断开后复用)
            public MediaPlayer MediaPlayer;
            public MediaSource MediaSource;   // 保留源用于失败重试
            public bool MediaRetried;         // 已重试过一次(避免循环重试)
            public FrameServerSocket FrameServer;
            public long FrameCount;
            public int LogT;
            public long LogN;
            // 最近收到帧的时刻(Environment.TickCount, int 毫秒)。设备断开但
            // session.Disconnected 事件未及时触发时, 定时器据此判定"无帧超时"
            // 主动清理, 保证宿主帧通道及时关闭。
            public volatile int LastFrameTick;

            public void Dispose()
            {
                // 全程 try-catch: 回调线程(ConnectionDisconnected/MediaFailed 重试)上
                // 任何一步抛未捕获异常都会直接杀死进程, 且崩溃瞬间来不及写日志。
                // 实测 2026-08-15: 设备断开时进程在正常收帧中"戛然而止"无任何异常堆栈。
                try { if (MediaPlayer != null) { MediaPlayer.IsVideoFrameServerEnabled = false; } }
                catch (Exception ex) { Program.Log("Dispose", ex); }
                try { MediaPlayer?.Dispose(); } catch (Exception ex) { Program.Log("Dispose", ex); }
                MediaPlayer = null;
                try { FrameServer?.Dispose(); } catch (Exception ex) { Program.Log("Dispose", ex); }
                FrameServer = null;
            }
        }

        private static void Main(string[] args)
        {
            // 全局兜底: 任何线程的未捕获异常(含 Miracast 回调线程)都先留日志再退出,
            // 避免"进程悄然消失、日志停在正常收帧处"无法定位。
            AppDomain.CurrentDomain.UnhandledException += (s, e) =>
                Log("Fatal", e.ExceptionObject as Exception ?? new Exception("Unhandled"));
            TaskScheduler.UnobservedTaskException += (s, e) =>
            {
                Log("TaskFault", e.Exception);
                e.SetObserved();
            };

            ParseArgs(args);
            Log("Info", new Exception($"Service starting ports=[{string.Join(",", _ports)}] max={_maxConnections} name={_sessionName}"));
            try
            {
                RunAsync().Wait();
            }
            catch (Exception ex)
            {
                Log("Fatal", ex);
                Environment.Exit(1);
            }
        }

        private static void ParseArgs(string[] args)
        {
            for (int i = 0; i < args.Length; i++)
            {
                if (args[i] == "--port" && i + 1 < args.Length && int.TryParse(args[i + 1], out var p))
                    _ports.Add(p);   // 兼容单路
                else if (args[i] == "--ports" && i + 1 < args.Length)
                {
                    foreach (var part in args[i + 1].Split(new[] { ',', ';' }, StringSplitOptions.RemoveEmptyEntries))
                    {
                        if (int.TryParse(part.Trim(), out var pt))
                            _ports.Add(pt);
                    }
                }
                else if (args[i] == "--max" && i + 1 < args.Length && int.TryParse(args[i + 1], out var m))
                    _maxConnections = m;
                else if (args[i] == "--name" && i + 1 < args.Length)
                    _sessionName = args[i + 1];
            }
            if (_ports.Count == 0)
                _ports.Add(0);
        }

        private static async Task RunAsync()
        {
            await InitializeMiracastAsync();

            // 保持进程运行(被宿主 kill 或 stdin 关闭时退出)
            var ev = new ManualResetEventSlim(false);
            Console.CancelKeyPress += (s, e) => { e.Cancel = true; ev.Set(); };
            AppDomain.CurrentDomain.ProcessExit += (s, e) => Cleanup();
            ev.Wait();
        }

        private static async Task InitializeMiracastAsync()
        {
            try
            {
                Log("Info", new Exception("Creating MiracastReceiver..."));
                _receiver = new MiracastReceiver();
                _receiver.StatusChanged += (r, o) =>
                    Log("Info", new Exception($"StatusChanged: listening={r.GetStatus().ListeningStatus}"));

                var settings = _receiver.GetDefaultSettings();
                // Win+K 设备名 = FriendlyName(即 Wi-Fi Direct P2P 设备名)。
                // 用户选中设备后 Windows 会解析该名字的 hostname 决定走
                // Infrastructure(MS-MICE,mDNS _display._tcp)还是标准 Wi-Fi Direct。
                // 必须与 MirrorCenter 的 mDNS 服务名/主机名(mirrorcenter.local)一致,
                // 否则解析失败回退 P2P(USB 网卡单射频易崩溃)。
                settings.FriendlyName = "MirrorCenter";
                settings.AuthorizationMethod = MiracastReceiverAuthorizationMethod.None;
                settings.RequireAuthorizationFromKnownTransmitters = false;

                Log("Info", new Exception("ApplySettings..."));
                var apply = await _receiver.DisconnectAllAndApplySettingsAsync(settings);
                Log("Info", new Exception($"ApplySettings={apply.Status}"));

                // 打印硬件支持的最大同时连接数(决定多路 Miracast 可行性)
                try
                {
                    var st = _receiver.GetStatus();
                    Log("Info", new Exception($"HW MaxSimultaneousConnections={st.MaxSimultaneousConnections}"));
                }
                catch (Exception ex)
                {
                    Log("GetStatus", ex);
                }

                Log("Info", new Exception("CreateSessionAsync..."));
                _session = await _receiver.CreateSessionAsync(null);
                _session.AllowConnectionTakeover = true;
                if (_maxConnections > 1)
                {
                    _session.MaxSimultaneousConnections = _maxConnections;
                    Log("Info", new Exception($"MaxSimultaneousConnections set to {_maxConnections}"));
                }
                _session.ConnectionCreated += OnConnectionCreated;
                _session.Disconnected += OnConnectionDisconnected;
                _session.MediaSourceCreated += OnMediaSourceCreated;

                Log("Info", new Exception("StartAsync..."));
                var start = await _session.StartAsync();
                Log("Info", new Exception($"Session.Start={start.Status}"));

                // 无帧超时检测(1s 粒度): 设备断开但 session.Disconnected 事件未及时
                // 触发时(重连后第二次断开实测会延迟), 3s 无帧 → 主动清理该连接,
                // 关闭帧通道 → 宿主 FrameClient 立即断链清画面, 无需等系统超时。
                new System.Threading.Timer(CheckIdleConnections, null, 1000, 1000);
            }
            catch (Exception ex)
            {
                Log("InitError", ex);
            }
        }

        private static void OnConnectionCreated(MiracastReceiverSession sender,
            MiracastReceiverConnectionCreatedEventArgs args)
        {
            Log("Info", new Exception($"ConnectionCreated #{_connections.Count} {args.Connection.Transmitter.Name}"));
        }

        private static async void OnMediaSourceCreated(MiracastReceiverSession sender,
            MiracastReceiverMediaSourceCreatedEventArgs args)
        {
            try
            {
                var conn = args.Connection;
                var state = new ConnectionState();

                // 分配路索引/端口:并发保护 + 提前登记(防同连接重复事件)
                lock (ConnectionsLock)
                {
                    if (_connections.ContainsKey(conn))
                    {
                        Log("Info", new Exception("MediaSourceCreated: duplicate connection, ignore"));
                        return;
                    }
                    // 空闲端口分配:不能用 _connections.Count 当索引——连接断开后
                    // Count 变小会复用仍在线的连接端口(实测: conn#0 断开后新
                    // 连接拿到 conn#1 的 3030 → 共享内存同名冲突 → 服务退出)。
                    // 改为扫描 _ports, 取第一个未被当前活跃连接占用的端口。
                    var used = new HashSet<int>();
                    foreach (var s in _connections.Values)
                        if (s.FrameServer != null)
                            used.Add(s.FrameServer.Port);
                    int freePort = -1;
                    foreach (var p in _ports)
                    {
                        if (!used.Contains(p)) { freePort = p; break; }
                    }
                    if (freePort < 0)
                    {
                        Log("Error", new Exception($"MediaSourceCreated: no free frame port (configured={string.Join(",", _ports)})"));
                        return;
                    }
                    state.Index = _connections.Count;  // 仅用于日志编号
                    state.Port = freePort;
                    _connections[conn] = state;
                }

                Log("Info", new Exception($"Connection#{state.Index} MediaSourceCreated name={conn.Transmitter.Name}"));

                // 帧通道:每路独立端口 → 宿主对应 FrameClient 监听 127.0.0.1:<port>
                state.FrameServer = new FrameServerSocket(state.Port, $"{_sessionName}-{state.Index}");
                await state.FrameServer.StartAsync();
                Log("Info", new Exception($"Connection#{state.Index} FrameServer connected port {state.Port}"));

                // 设备名 → 宿主(信息栏/控制列表显示真实设备名, 如 "Honor 10").
                // 连接建立初期发送, 帧数据开始前, 与帧头无竞争。
                try { state.FrameServer.SendControl("NAME:" + (conn.Transmitter.Name ?? "")); }
                catch (Exception ex) { Log("Name", ex); }

                // 宿主移除该投屏源(SETDISC): 主动断开本连接。
                // 注意: 文档明确"应用自己 Disconnect 时 session.Disconnected 事件不会触发",
                // 因此断开后必须手动清理本路状态(不能等 OnConnectionDisconnected)。
                // 实测 2026-08-16: 仅 Disconnect 不生效(连接仍收帧), 需组合
                // 停止 MediaPlayer + Disconnect + Close(IDisposable) 兜底。
                state.FrameServer.RequestDisconnect = () =>
                {
                    try
                    {
                        Log("Ctrl", new Exception($"RequestDisconnect conn#{state.Index} begin"));

                        // 1) 停止媒体消费,释放帧链路
                        try
                        {
                            var mp = state.MediaPlayer;
                            if (mp != null)
                            {
                                mp.IsVideoFrameServerEnabled = false;
                                mp.Pause();
                                mp.Dispose();
                            }
                        }
                        catch (Exception ex) { Log("Disconnect", ex); }
                        state.MediaPlayer = null;
                        state.FrameServer.MediaPlayerRef = null;

                        // 2) 通知设备断开
                        try
                        {
                            conn.Disconnect(
                                Windows.Media.Miracast.MiracastReceiverDisconnectReason.DisconnectedByUser,
                                "removed by host");
                        }
                        catch (Exception ex) { Log("Disconnect", ex); }
                        // 3) IClosable 兜底: 直接关闭连接(Disconnect 实测可能静默无效)
                        try { ((IDisposable)conn).Dispose(); }
                        catch (Exception ex) { Log("Disconnect", ex); }

                        // 4) 手动清理本路(主动断开不会触发 session.Disconnected 事件)
                        ConnectionState removed = null;
                        lock (ConnectionsLock)
                        {
                            if (_connections.ContainsKey(conn))
                            {
                                removed = _connections[conn];
                                _connections.Remove(conn);
                            }
                        }
                        removed?.Dispose();
                        Log("Info", new Exception(
                            $"Connection removed by host, remaining={_connections.Count}"));
                        // 连接数变化 → 刷新剩余帧通道读回尺寸
                        UpdateFrameScales();
                        Log("Ctrl", new Exception($"RequestDisconnect conn#{state.Index} done"));
                    }
                    catch (Exception ex)
                    {
                        Log("Disconnect", ex);
                    }
                };

                // 自适应缩放:按当前活跃连接数刷新所有帧通道的读回尺寸
                UpdateFrameScales();

                state.MediaSource = args.MediaSource;   // 失败重试用
                state.MediaPlayer = new MediaPlayer();
                state.MediaPlayer.IsVideoFrameServerEnabled = true;
                // 实时模式:不缓冲,避免"缓冲耗尽 → 交付停摆"
                state.MediaPlayer.RealTimePlayback = true;
                state.FrameServer.MediaPlayerRef = state.MediaPlayer;   // SETMUTE 按连接静音
                state.MediaPlayer.VideoFrameAvailable += (s, o) => OnVideoFrameAvailable(s, state);
                state.MediaPlayer.MediaFailed += (s, e) =>
                {
                    Log("MediaFailed", new Exception($"conn#{state.Index} err={e.Error} msg={e.ErrorMessage}"));
                    // 一次性自愈:媒体源首次建立失败(SourceNotSupported 常见于 P2P 媒体协商
                    // 偶发失败/解码器瞬时不可用), 释放后用同一源重建 MediaPlayer 再试一次。
                    if (state.MediaRetried)
                        return;
                    state.MediaRetried = true;
                    try
                    {
                        var src = state.MediaSource;
                        state.MediaPlayer?.Dispose();
                        state.MediaPlayer = new MediaPlayer();
                        state.MediaPlayer.IsVideoFrameServerEnabled = true;
                        state.MediaPlayer.RealTimePlayback = true;
                        if (state.FrameServer != null)
                            state.FrameServer.MediaPlayerRef = state.MediaPlayer;   // 重建后重新挂接 SETMUTE
                        state.MediaPlayer.VideoFrameAvailable += (pl, o) => OnVideoFrameAvailable(pl, state);
                        state.MediaPlayer.PlaybackSession.NaturalVideoSizeChanged += (pl, e2) =>
                            Log("Size", new Exception($"conn#{state.Index} w={pl.NaturalVideoWidth} h={pl.NaturalVideoHeight}"));
                        state.MediaPlayer.MediaFailed += (pl, e2) =>
                            Log("MediaFailed2", new Exception($"conn#{state.Index} err={e2.Error} msg={e2.ErrorMessage}"));
                        state.MediaPlayer.Source = src;
                        state.MediaPlayer.Play();
                        Log("Retry", new Exception($"conn#{state.Index} recreated MediaPlayer after {e.Error}"));
                    }
                    catch (Exception ex)
                    {
                        Log("Retry", ex);
                    }
                };
                state.MediaPlayer.PlaybackSession.NaturalVideoSizeChanged += (s, e) =>
                    Log("Size", new Exception($"conn#{state.Index} w={state.MediaPlayer.PlaybackSession.NaturalVideoWidth} h={state.MediaPlayer.PlaybackSession.NaturalVideoHeight}"));
                state.MediaPlayer.Source = args.MediaSource;
                state.MediaPlayer.Play();
            }
            catch (Exception ex)
            {
                Log("MediaSourceCreated", ex);
            }
        }

        private static void OnVideoFrameAvailable(MediaPlayer sender, ConnectionState state)
        {
            state.LastFrameTick = Environment.TickCount;
            var ps = sender.PlaybackSession;
            int w = (int)ps.NaturalVideoWidth;
            int h = (int)ps.NaturalVideoHeight;
            long n = Interlocked.Increment(ref state.FrameCount);

            int nowT = Environment.TickCount;
            if (nowT - state.LogT >= 1000)
            {
                Log("VFrate", new Exception($"conn#{state.Index} rate={n - state.LogN}/s total={n}"));
                state.LogT = nowT;
                state.LogN = n;
            }

            // 关键:帧服务器模式下每次回调都必须消费当前帧,否则交付停摆
            if (w <= 0 || h <= 0)
                return;
            state.FrameServer?.QueueFrame(sender, w, h);
        }

        private static void OnConnectionDisconnected(MiracastReceiverSession sender,
            MiracastReceiverDisconnectedEventArgs args)
        {
            var conn = args.Connection;
            Log("Info", new Exception($"Disconnected {conn.Transmitter.Name}"));
            ConnectionState state = null;
            lock (ConnectionsLock)
            {
                if (_connections.TryGetValue(conn, out state))
                    _connections.Remove(conn);
            }
            if (state != null)
            {
                state.Dispose();
                Log("Info", new Exception($"Connection cleaned, remaining={_connections.Count}"));
                // 自适应缩放:连接数变化, 刷新剩余帧通道的读回尺寸(多路↔单路切换)
                UpdateFrameScales();
            }
        }

        // 无帧超时阈值: 连续 3s 未收到任何视频帧 → 设备已断开(P2P/媒体层静默退出)。
        // 限帧场景(SETFPS 1)帧间隔 1s, 阈值 3s 留有 2 帧余量, 不会误判。
        private const int kIdleTimeoutMs = 3000;

        private static void CheckIdleConnections(object _)
        {
            List<MiracastReceiverConnection> stale = null;
            int nowT = Environment.TickCount;
            lock (ConnectionsLock)
            {
                foreach (var kv in _connections)
                {
                    var st = kv.Value;
                    if (st.FrameServer != null && st.LastFrameTick > 0 &&
                        (nowT - st.LastFrameTick) > kIdleTimeoutMs)
                    {
                        (stale ??= new List<MiracastReceiverConnection>()).Add(kv.Key);
                    }
                }
            }
            if (stale == null)
                return;
            foreach (var conn in stale)
            {
                ConnectionState state = null;
                lock (ConnectionsLock)
                {
                    if (_connections.TryGetValue(conn, out state))
                        _connections.Remove(conn);
                }
                if (state == null)
                    continue;
                Log("Info", new Exception(
                    $"Source idle {kIdleTimeoutMs}ms, removing conn#{state.Index}"));
                // 停播放器 + 关帧通道(TCP) → 宿主 FrameClient 断链 → 宿主清画面。
                // 顺序保持"先停播放器": _running=false 让 SendLoop 正常退出,
                // 不会走 Environment.Exit(0) 分支误杀共享服务进程。
                state.Dispose();
                // 尽力通知设备断开(不依赖, 通道关闭后设备侧终会感知)
                try { ((IDisposable)conn).Dispose(); }
                catch (Exception ex) { Log("Idle", ex); }
                try
                {
                    conn.Disconnect(MiracastReceiverDisconnectReason.DisconnectedByUser,
                                    "source idle");
                }
                catch (Exception ex) { Log("Idle", ex); }
                Log("Info", new Exception(
                    $"Connection removed (source idle), remaining={_connections.Count}"));
                UpdateFrameScales();
            }
        }

        /// <summary>
        /// 自适应读回尺寸(按活跃连接数分档):
        ///   1~2 路      → 最大边 1280(约 1280x720)
        ///   3~4 路      → 最大边 960(约 960x540)
        ///   5~9 路      → 最大边 640(约 640x360)
        ///   >9 路       → 最大边 480(约 480x270)
        /// 2026-08-15 用户确认单路也限 1280: 1080p 全尺寸读回 8MB/帧是 CPU 大头,
        /// 限 1280 后读回量降 ~4 倍, 服务端 CPU 明显下降, 宿主 1280x800 显示画质无感损失。
        /// </summary>
        private static void UpdateFrameScales()
        {
            List<FrameServerSocket> servers;
            lock (ConnectionsLock)
            {
                servers = new List<FrameServerSocket>(_connections.Count);
                foreach (var s in _connections.Values)
                    if (s.FrameServer != null)
                        servers.Add(s.FrameServer);
            }
            int n = servers.Count;
            int edge = n >= 10 ? 480
                     : n >= 5  ? 640
                     : n >= 3  ? 960
                     : 1280;
            // 主动限帧(2026-08-15): 多路 GPU 竞争下帧回调会持续全链路处理导致 CPU 增长,
            // 按连接数限制输出帧率。单路不限制(源通常 ~30fps)。
            int fps = n >= 10 ? 10
                    : n >= 5  ? 12
                    : n >= 3  ? 15
                    : n >= 2  ? 20
                    : 30;
            foreach (var fs in servers)
            {
                fs.ScaleEdge = edge;
                fs.MaxFps = fps;
            }
            Log("Scale", new Exception($"active={n} edge={edge} fps={fps}"));
        }

        private static void Cleanup()
        {
            List<ConnectionState> states;
            lock (ConnectionsLock)
            {
                states = new List<ConnectionState>(_connections.Values);
                _connections.Clear();
            }
            foreach (var st in states)
                st.Dispose();
            _session?.Dispose();
            _session = null;
            Log("Info", new Exception("Service exit"));
        }

        private static readonly object _logLock = new();

        internal static void Log(string tag, Exception ex)
        {
            string line = $"[{DateTime.Now:yyyy-MM-dd HH:mm:ss}] {tag}: {ex?.ToString()}\r\n";
            lock (_logLock)
            {
                try
                {
                    string dir = Path.Combine(
                        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                        "MirrorCenter");
                    Directory.CreateDirectory(dir);
                    string file = Path.Combine(dir, "miracast.log");
                    // FileShare.ReadWrite:并发/跨进程读写互不阻塞。
                    // 修复 2026-08-15 实测日志停摆: File.AppendAllText 默认 FileShare.Read,
                    // 被其他持有者(杀软扫描/挂起的写)占用后, 后续所有 Log 静默失败,
                    // 导致 read=/copy=/VFrate 观测数据全丢。
                    using (var fs = new FileStream(file, FileMode.Append, FileAccess.Write, FileShare.ReadWrite))
                    {
                        byte[] bytes = System.Text.Encoding.UTF8.GetBytes(line);
                        fs.Write(bytes, 0, bytes.Length);
                    }
                }
                catch { /* 日志失败不能影响主流程 */ }
            }
        }
    }
}
