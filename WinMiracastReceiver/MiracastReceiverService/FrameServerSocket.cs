using System;
using System.Collections.Generic;
using System.IO.MemoryMappedFiles;
using System.Net;
using System.Net.Sockets;
using System.Runtime.InteropServices.WindowsRuntime;
using System.Threading;
using System.Threading.Tasks;
using Windows.Graphics.Imaging;
using Windows.Media;
using Windows.Media.Playback;

namespace MiracastReceiverService
{
    /// <summary>
    /// 帧通道:MediaPlayer 帧回调 → VideoFrame 轮换 → SoftwareBitmap GPU→CPU 读回
    /// → 共享内存直传(TCP 仅发 28B 头)。v2(2026-08-15): 帧负载写入
    /// Local\MirrorCenterFrames_&lt;port&gt; 双槽共享内存, 宿主零 TCP 大负载接收。
    ///
    /// 关键设计(已验证):
    /// - 每次回调都执行 CopyFrameToVideoSurface,保持 MediaPlayer 帧服务器持续交付
    /// - 仅保留最新帧(发送跟不上时实时丢弃旧帧,避免延迟累积)
    /// - 发送循环空闲时做统计打点与宿主断链检测
    /// - SoftwareBitmap.CreateCopyFromSurfaceAsync 是唯一可靠的 GPU→CPU 路径
    ///   (IDirect3DSurface 不实现 IDirect3DDXGIInterfaceAccess, QI=E_NOINTERFACE,
    ///   无法拿原生 DXGI 共享句柄 → GPU 全链路不可行, 2026-08-15 实测确认)
    /// </summary>
    internal sealed class FrameServerSocket : IDisposable
    {
        private readonly int _port;
        private readonly string _name;
        private volatile bool _running;
        private Task _sendLoopTask;

        // 单槽最新帧(发送慢于回调时实时丢弃旧帧,避免延迟累积)
        // 重要:VideoFrame 的 D3D surface 不能跨帧复用(CopyFrameToVideoSurface 要求全新 surface),
        // 所以每帧新建。释放(Dispose)一律在 SendLoop 线程执行:
        //   - 回调线程做 GPU surface Dispose 会触发 GPU 同步,拖慢帧服务器交付(实测回调 60fps→10fps→停摆)
        //   - 被新帧顶替的旧帧先入 _toDispose,由发送循环统一释放
        private readonly object _pendingLock = new();
        private VideoFrame _pending;
        private readonly List<VideoFrame> _toDispose = new();

        // TCP
        private TcpClient _client;
        private NetworkStream _stream;

        // 统计
        private long _cbCount;
        private long _sentCount;
        private int _lastStatsT;

        // 发送缓冲复用(避免每帧分配 8MB heap)
        private byte[] _pixelBuf;
        private int _bufW, _bufH;

        // 共享内存直传(v2): 帧负载直接写共享内存, TCP 仅发 28B 头(含槽号)。
        // 名称与宿主 FrameClient 约定: Local\MirrorCenterFrames_<port>。
        // 双槽轮换(保留"仅最新帧"语义: 慢帧被新帧顶替)。
        // v2.1(2026-08-15): 槽尾 4B seqlock 状态值消除写读竞争——
        // 宿主 memcpy 前后各读一次 state(奇数=写入中, 偶数=完整, 单调递增),
        // 两次相同且为偶数才采信 → 杜绝读到"写了一半"的混合数据(偶现花屏根因)。
        // 写方顺序: state=奇数 → payload → state=偶数。
        private const int kMaxSlotBytes = 1920 * 1080 * 4;   // 1080p BGRA8 上限(与宿主严格一致)
        private const int kSlotCount = 2;
        private const int kSlotStateOff = kMaxSlotBytes - 4; // 槽尾 4B state(避开 payload)
        private MemoryMappedFile _mmf;
        private MemoryMappedViewAccessor _view;
        private int _slot;
        private int _slotSeq;   // seqlock 单调递增序号(奇数=写入中, 偶数=完整)

        // 自适应读回尺寸(由 Program 根据活跃连接数动态设置):
        //   0    = 按源分辨率读回(单路, 清晰度优先)
        //   >0   = 最大边上限(≥2 路时缩小到 1280x720 级别, 多路性能优先)
        public volatile int ScaleEdge;

        // 主动限帧(由 Program 按活跃连接数设置): 帧回调每帧仍必须消费
        // (CopyFrameToVideoSurface,否则 MediaPlayer 交付停摆), 但超过该帧率的帧
        // 只做 GPU 拷贝后立即丢弃, 不再读回/拷贝/发送 → 降低多路 GPU 竞争下的 CPU。
        // 实测: 2 路并发时 copy=31~78ms/帧, VFrate 30/s→12/s 且 CPU 持续增长,
        // 限帧可避免"每帧都全链路处理"的无谓开销。
        public volatile int MaxFps;
        private int _lastSendT = int.MinValue;   // 上次真正发送的时刻(节流基准)

        // 宿主控制帧率覆盖(全屏放大场景): 宿主经 TCP 帧通道发 "SETFPS n" 文本行,
        // 覆盖 Program 按连接数的默认 MaxFps。>0 强制帧率, 0 恢复默认。
        // 实现: 焦点路放大时把其余路降到 1fps(连接保持, 仅极低开销),
        // 缩回后发 SETFPS 0 恢复 → 焦点路独占 GPU/CPU, 帧率回满。
        public volatile int FpsOverride;

        // 宿主读回尺寸覆盖(混合路数分档): AirPlay 路不走本服务, 服务端按自己的
        // 连接数分档会低估总路数(如 2 Miracast + 1 AirPlay 时仍按 2 路给 1280)。
        // 宿主在 relayout 时按总活跃路数计算并推 "SETEDGE n"(n>=0), 优先于
        // Program 按连接数的 ScaleEdge。-1 = 未设置(用服务端默认)。
        public volatile int EdgeOverride = -1;

        public FrameServerSocket(int port, string name)
        {
            _port = port;
            _name = name;
            ScaleEdge = 0;   // 默认单路:不缩放
        }

        public async Task StartAsync()
        {
            _client = new TcpClient();
            await _client.ConnectAsync(IPAddress.Loopback, _port);
            _stream = _client.GetStream();

            // v2: 创建共享内存(2 槽轮换), 名称与宿主 FrameClient 约定一致
            try
            {
                string shmName = @"Local\MirrorCenterFrames_" + _port;
                _mmf = MemoryMappedFile.CreateNew(shmName,
                    (long)kSlotCount * kMaxSlotBytes, MemoryMappedFileAccess.ReadWrite);
                _view = _mmf.CreateViewAccessor(0,
                    (long)kSlotCount * kMaxSlotBytes, MemoryMappedFileAccess.ReadWrite);
                Program.Log("Shm", new Exception(
                    $"created {(long)kSlotCount * kMaxSlotBytes}B slots={kSlotCount} port={_port}"));
            }
            catch (Exception ex)
            {
                // 名称被残留占用等: 帧会因 WriteArray 失败在 SendFrame 抛出 → 进程退出由宿主重启
                Program.Log("Shm", ex);
            }

            _running = true;
            _lastStatsT = Environment.TickCount;
            _sendLoopTask = Task.Run(SendLoop);
            _ = Task.Run(ControlLoop);   // 宿主控制消息读取(全屏放大降帧)
        }

        // ========== 宿主控制消息(后台线程) ==========
        // 宿主 FrameClient 在帧 TCP 通道上发文本行 "SETFPS n"(n>0 强制帧率, 0 恢复默认)。
        // 帧通道方向只有服务端→宿主写帧头, 宿主→服务端仅此控制消息, 无数据冲突。

        private async Task ControlLoop()
        {
            try
            {
                var buffer = new List<byte>(16);
                var scratch = new byte[256];
                while (_running && _client != null)
                {
                    int n = await _stream.ReadAsync(scratch, 0, scratch.Length);
                    if (n <= 0) break;
                    for (int i = 0; i < n; i++)
                    {
                        byte b = scratch[i];
                        if (b == (byte)'\n')
                        {
                            string line = System.Text.Encoding.ASCII.GetString(buffer.ToArray()).Trim();
                            buffer.Clear();
                            if (line.StartsWith("SETFPS ", StringComparison.Ordinal))
                            {
                                if (int.TryParse(line.Substring(7), out int fps))
                                {
                                    FpsOverride = fps;
                                    Program.Log("Ctrl", new Exception($"SETFPS {fps}"));
                                }
                            }
                            else if (line.StartsWith("SETEDGE ", StringComparison.Ordinal))
                            {
                                if (int.TryParse(line.Substring(8), out int edge))
                                {
                                    EdgeOverride = edge;
                                    Program.Log("Ctrl", new Exception($"SETEDGE {edge}"));
                                }
                            }
                        }
                        else if (buffer.Count < 64)
                        {
                            buffer.Add(b);
                        }
                    }
                }
            }
            catch (Exception ex)
            {
                // 宿主断开/通道关闭: 忽略, 发送循环会检测断链退出
                Program.Log("Ctrl", ex);
            }
        }

        // ========== 帧入队(回调线程) ==========

        public void QueueFrame(MediaPlayer sender, int width, int height)
        {
            Interlocked.Increment(ref _cbCount);
            if (!_running || _client == null || !_client.Connected)
                return;
            if (width <= 0 || height <= 0)
                return;

            try
            {
                int t0 = Environment.TickCount;
                // 自适应读回:单路(ScaleEdge==0)按源分辨率, 保证清晰度;
                // ≥2 路(ScaleEdge=1280)缩小读回:宿主分屏格子约 1280x720,
                // 1080p 原帧读回 8MB/帧 GPU 同步 15-47ms 是发送 33fps→15fps 的瓶颈,
                // 小尺寸 surface 让 GPU 在拷贝时缩放, 读回数据量减半, 发送帧率可翻倍。
                // 宿主 SETEDGE 覆盖优先(混合路数分档, 见 EdgeOverride 注释)。
                int tw = width, th = height;
                int maxEdge = EdgeOverride >= 0 ? EdgeOverride : ScaleEdge;
                if (maxEdge > 0 && (tw > maxEdge || th > maxEdge))
                {
                    double s = tw > th ? (double)maxEdge / tw : (double)maxEdge / th;
                    tw = (int)(tw * s); th = (int)(th * s);
                    if (tw < 1) tw = 1;
                    if (th < 1) th = 1;
                }
                var frame = VideoFrame.CreateAsDirect3D11SurfaceBacked(
                    Windows.Graphics.DirectX.DirectXPixelFormat.B8G8R8A8UIntNormalized,
                    tw, th);
                // 目标矩形:整帧缩放到小 surface(GPU 完成缩放, 无额外开销)
                sender.CopyFrameToVideoSurface(frame.Direct3DSurface,
                    new Windows.Foundation.Rect(0, 0, tw, th));

                // 可行性验证(每进程一次): 探测 surface 能否 QI 原生 DXGI 并共享
                SharedDxgi.ProbeOnce(frame.Direct3DSurface);

                long cb = _cbCount;

                // 主动限帧: 超过有效帧率的帧只做 GPU 拷贝(必须消费,否则 MediaPlayer
                // 交付停摆)后立即丢弃, 不进入发送队列 → 省掉读回/CopyToBuffer/写共享内存/
                // TCP 头 全链路开销。仅保留最新帧语义不变。
                // 有效帧率 = 宿主控制覆盖(FpsOverride>0) 或 Program 按连接数的默认 MaxFps。
                int maxFps = FpsOverride > 0 ? FpsOverride : MaxFps;
                if (maxFps > 0)
                {
                    int interval = 1000 / maxFps;
                    int lastT = _lastSendT;
                    int nowT = Environment.TickCount;
                    if (lastT != int.MinValue && (nowT - lastT) < interval)
                    {
                        lock (_pendingLock)
                            _toDispose.Add(frame);   // 统一在 SendLoop 线程 Dispose
                        return;
                    }
                    _lastSendT = nowT;
                }

                if (cb % 30 == 0)
                    Program.Log("Copy", new Exception(
                        $"#{cb} {tw}x{th} copy={Environment.TickCount - t0}ms"));

                lock (_pendingLock)
                {
                    // 丢弃上一帧(发送循环若未取走,视为可丢弃)。
                    // 注意:不能在回调线程 Dispose——交给 SendLoop 统一释放。
                    if (_pending != null)
                        _toDispose.Add(_pending);
                    _pending = frame;
                }
            }
            catch (Exception ex)
            {
                Program.Log("QueueFrame", ex);
            }
        }

        // ========== 发送循环(后台线程) ==========

        private async Task SendLoop()
        {
            _lastStatsT = Environment.TickCount;
            try
            {
                while (_running)
                {
                    // 统一释放被顶替的旧帧(只在发送线程做 GPU Dispose,避免拖慢回调)
                    lock (_pendingLock)
                    {
                        foreach (var old in _toDispose)
                            old.Dispose();
                        _toDispose.Clear();
                    }

                    int nowT = Environment.TickCount;

                    // 3s 统计打点
                    if (nowT - _lastStatsT >= 3000)
                    {
                        int pending = _pending != null ? 1 : 0;
                        Program.Log("Stats", new Exception(
                            $"cb={_cbCount} sent={_sentCount} pending={pending}"));
                        _lastStatsT = nowT;
                    }

                    // 检测宿主进程断链(QProcess 的 stdin 管道从不写入,Peek 会阻塞,
                    // 因此不能用 stdin 检测;TCP 断开时 SendFrameAsync 抛异常 → break → 退出)
                    // (无代码)

                    // 取最新帧(如有旧帧,dispose 掉)
                    VideoFrame vf = null;
                    lock (_pendingLock)
                    {
                        vf = _pending;
                        _pending = null;
                    }
                    if (vf == null) { await Task.Delay(2); continue; }

                    try
                    {
                        await SendFrameAsync(vf);
                        Interlocked.Increment(ref _sentCount);
                    }
                    catch (Exception ex)
                    {
                        Program.Log("SendFrame", ex);
                        Environment.Exit(0);  // 网络断开 → 退出(宿主重启后会重新拉起)
                    }
                    finally { vf?.Dispose(); }
                }
            }
            catch (Exception ex)
            {
                Program.Log("SendLoop", ex);
                Environment.Exit(1);
            }
        }

        private async Task SendFrameAsync(VideoFrame vf)
        {
            int t0 = Environment.TickCount;

            var swBmp = await SoftwareBitmap.CreateCopyFromSurfaceAsync(
                vf.Direct3DSurface).AsTask();
            SoftwareBitmap px = null;
            try
            {
                int w = swBmp.PixelWidth, h = swBmp.PixelHeight;
                // 跳过冗余 Convert: surface 本就是 BGRA8(创建时指定),
                // Convert 会整帧拷贝; alpha 模式只是元数据, 我们只做原始字节搬运, 无关紧要。
                px = (swBmp.BitmapPixelFormat == BitmapPixelFormat.Bgra8)
                     ? swBmp
                     : SoftwareBitmap.Convert(swBmp, BitmapPixelFormat.Bgra8, BitmapAlphaMode.Ignore);
                int readMs = Environment.TickCount - t0;

                // 缓冲复用
                int rawSize = px.PixelWidth * px.PixelHeight * 4;
                if (_pixelBuf == null || _bufW != w || _bufH != h)
                {
                    _pixelBuf = new byte[rawSize];
                    _bufW = w; _bufH = h;
                }
                if (rawSize > kMaxSlotBytes)
                {
                    // 超出共享槽上限(如 4K 源): 丢帧, 保持链路稳定
                    Program.Log("Send", new Exception($"frame too large {w}x{h} > {kMaxSlotBytes}B, drop"));
                    return;
                }
                px.CopyToBuffer(_pixelBuf.AsBuffer());

                // v2.1 共享内存直传 + seqlock: 负载写入共享槽, TCP 仅发 28B 头(含槽号)。
                // 槽尾 4B 为单调递增状态值: 奇数=写入中, 偶数=完整。
                // 宿主 memcpy 前后各读一次, 两次相同且为偶数才采信 → 杜绝读到
                // "写了一半"的混合数据(偶现花屏根因)。
                int slot = _slot; _slot = 1 - _slot;
                long slotOff = (long)slot * kMaxSlotBytes;
                int seqOdd = ++_slotSeq | 1;   // 标记写入中(奇数)
                _view.Write(slotOff + kSlotStateOff, seqOdd);
                _view.WriteArray(slotOff, _pixelBuf, 0, rawSize);
                int seqEven = ++_slotSeq & ~1; // 写入完成(偶数)
                _view.Write(slotOff + kSlotStateOff, seqEven);

                // 帧协议 v2: [magic 8B "MCVIDEO0"][w 4B][h 4B][stride 4B][size 4B][slot 4B] = 28B
                // stride==0 → JPEG; stride==w*4 → RAW BGRA8(负载在共享内存 slot 槽)
                var header = new byte[28];
                Buffer.BlockCopy(new byte[] {
                    (byte)'M', (byte)'C', (byte)'V', (byte)'I',
                    (byte)'D', (byte)'E', (byte)'O', (byte)'0' },
                    0, header, 0, 8);
                BitConverter.GetBytes(w).CopyTo(header, 8);
                BitConverter.GetBytes(h).CopyTo(header, 12);
                BitConverter.GetBytes(w * 4).CopyTo(header, 16);   // stride=w*4 标记 RAW
                BitConverter.GetBytes(rawSize).CopyTo(header, 20);
                BitConverter.GetBytes(slot).CopyTo(header, 24);

                await _stream.WriteAsync(header, 0, header.Length);

                // 每 10 帧打点一次
                if (_sentCount == 0 || _sentCount % 10 == 0)
                    Program.Log("Send", new Exception(
                        $"#{_sentCount} {w}x{h} read={readMs}ms shm-slot={slot} raw={rawSize}B"));
            }
            finally
            {
                if (!ReferenceEquals(px, swBmp)) px?.Dispose();
                swBmp.Dispose();
            }
        }

        // ========== 清理 ==========

        public void Dispose()
        {
            _running = false;
            _sendLoopTask?.Wait(3000);
            lock (_pendingLock)
            {
                _pending?.Dispose(); _pending = null;
                foreach (var old in _toDispose) old.Dispose();
                _toDispose.Clear();
            }
            _stream?.Dispose(); _stream = null;
            _client?.Close(); _client = null;
            _view?.Dispose(); _view = null;
            _mmf?.Dispose(); _mmf = null;
        }
    }
}
