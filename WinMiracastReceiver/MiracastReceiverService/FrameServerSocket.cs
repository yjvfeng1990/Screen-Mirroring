using System;
using System.Collections.Generic;
using System.IO.MemoryMappedFiles;
using System.Net;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Runtime.InteropServices.WindowsRuntime;
using System.Threading;
using System.Threading.Tasks;
using Windows.Graphics.Imaging;
using Windows.Media;
using Windows.Media.Playback;

namespace MiracastReceiverService
{
    /// <summary>
    /// 帧通道:MediaPlayer 帧回调 → VideoFrame 轮换 → 帧输出。
    /// v3(2026-09) GPU 零拷贝: 自建 D3D11 设备 + 命名共享纹理(SHARED_NTHANDLE
    /// |KEYEDMUTEX), CopyFrameToVideoSurface 直接 GPU 缩放拷入共享纹理, TCP 仅发
    /// 28B 头(stride=0xFFFFFFFF 标记 GPU 帧), 宿主 WGL_NV_DX_interop2 采样渲染。
    /// v2(2026-08-15) SHM 路径完整保留为回退(GPU 模式关闭/纹理创建失败):
    /// SoftwareBitmap CPU 读回 → Local\MirrorCenterFrames_&lt;port&gt; 双槽共享内存直传。
    /// (2026-08-15 曾因 CreateAsDirect3D11SurfaceBacked 产物封闭不可共享而判定
    /// GPU 链路不可行, v3 改为自建共享纹理绕开, 该限制不再适用)
    ///
    /// 关键设计(已验证):
    /// - 每次回调都执行 CopyFrameToVideoSurface,保持 MediaPlayer 帧服务器持续交付
    ///   (GPU 模式下拷入共享纹理即完成消费, 不再建临时 surface)
    /// - 仅保留最新帧(发送跟不上时实时丢弃旧帧,避免延迟累积)
    /// - 发送循环空闲时做统计打点与宿主断链检测
    /// - surface 释放(Dispose)一律在 SendLoop 线程执行(回调线程 Dispose 触发
    ///   GPU 同步会拖慢帧服务器交付)
    /// </summary>
    internal sealed class FrameServerSocket : IDisposable
    {
        private readonly int _port;
        private readonly string _name;
        private volatile bool _running;
        private Task _sendLoopTask;

        /// 本路帧端口(Program 空闲端口分配用, 连接断开后该端口可被复用)
        public int Port => _port;
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
        private readonly object _writeLock = new();   // 保护控制消息与帧头并发写

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

        // 本路对应的 MediaPlayer(Program 在连接创建后赋值, 断开时清空)。
        // 宿主 SETMUTE 命令按"连接"静音: Miracast 组共享同一接收进程,
        // 用 WASAPI 按进程静音会把整组(含焦点路)都静音, 必须走 MediaPlayer.Volume。
        public MediaPlayer MediaPlayerRef { get; set; }

        // 宿主 SETDISC 命令 → 请求断开该连接(由 Program 挂接 MiracastReceiverConnection)。
        public Action RequestDisconnect;

        // ========== v3 GPU 零拷贝(2026-09) ==========
        // 帧驻留 GPU: 自建 D3D11 设备 + 命名共享纹理(NT 句柄 + keyed mutex),
        // CopyFrameToVideoSurface 直接缩放拷入, TCP 仅发 28B 头(stride=0xFFFFFFFF)。
        // SHM 路径完整保留: _gpuMode=false 自动回退(设备创建/纹理创建失败)。
        // 线程模型: 回调线程写纹理环, SendLoop 发头并统一做环替换/销毁。
        // GpuEnabled 进程级开关(默认关): GPU 帧头要求宿主 FrameClient 支持
        // stride=0xFFFFFFFF 解析(M2), 宿主未升级前强制关, 保持 SHM 兼容。
        // MiracastReceiverService 启动参数 --gpu 1 / ConsoleTest 显式置 true 打开。
        public static bool GpuEnabled = false;
        private bool _gpuMode;
        // 共享纹理 MiscFlags 模式(自诊断矩阵): 拷贝器/创建拒绝时同 gen 切换
        // (纹理名不变, 宿主按名打开不受影响), 两种模式都败才回退 SHM。
        // 初始共享纹理模式: 本机真机实测(M1/M2) CopyFrameToVideoSurface 对
        // KeyedNt 纹理必抛 ArgumentException(0x887A0001), 每次新连接都白试
        // 一轮 KeyedNt→重建→重试(~1s 首帧延迟), 故直接默认 NtOnly。
        // 若某机器 KeyedNt 可用, 走 L525 的降级兜底仍可自动切回。
        private SharedTexMode _gpuTexMode = SharedTexMode.NtOnly;
        private IntPtr _d3dDevice;
        private GpuTextureRing _ring;                       // 当前纹理环(尺寸=缩放后 tw×th)
        private readonly List<GpuTextureRing> _ringToDispose = new();  // 旧环由 SendLoop 销毁
        private int _gpuGen;                                // 环世代(纹理名含 gen, 防宿主拿旧对象)
        private int _gpuSlot;                               // 下一帧写入槽(每次拷贝交替)
        private long _gpuSeq;                               // GPU 帧序号(帧头 size 字段复用)
        // GPU pending(与 SHM pending 复用 _pendingLock): SendLoop 取走后发头
        private bool _gpuHasPending;
        private int _gpuPendingSlot, _gpuPendingW, _gpuPendingH;
        private int _gpuPendingGen;                          // 环世代随 pending 一起交给发送线程
        private long _gpuPendingSeq;

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

            // v3 GPU 零拷贝初始化: 设备建好即启用 GPU 模式(纹理环在首帧按实际尺寸创建)。
            // 任一步失败仅置 _gpuMode=false 记日志, SHM 路径照常工作。
            if (!GpuEnabled)
            {
                Program.Log("Gpu", new Exception($"gpu disabled (SHM mode) port={_port}"));
            }
            else try
            {
                if (DxgiInterop.TryCreateDevice(out _d3dDevice))
                {
                    _gpuMode = true;
                    Program.Log("Gpu", new Exception($"device created, gpu mode ON port={_port}"));
                }
                else
                {
                    Program.Log("Gpu", new Exception($"device create failed, gpu mode OFF port={_port}"));
                }
            }
            catch (Exception ex)
            {
                _gpuMode = false;
                Program.Log("Gpu", ex);
            }

            _running = true;
            _lastStatsT = Environment.TickCount;
            _sendLoopTask = Task.Run(SendLoop);
            _ = Task.Run(ControlLoop);   // 宿主控制消息读取(全屏放大降帧)
        }

        // ========== 宿主控制消息(后台线程) ==========
        // 宿主 FrameClient 在帧 TCP 通道上发文本行 "SETFPS n"(n>0 强制帧率, 0 恢复默认)。
        // 帧通道方向只有服务端→宿主写帧头, 宿主→服务端仅此控制消息, 无数据冲突。

        /// 向宿主发控制消息(带 MCCTRL1 前缀以区分帧头协议)。仅在连接建立初期
        /// (帧数据开始前)调用, 与发送循环无竞争; 写锁防止与帧头字节交错。
        public void SendControl(string message)
        {
            try
            {
                if (_stream != null && _client != null && _client.Connected)
                {
                    byte[] bytes = System.Text.Encoding.ASCII.GetBytes("MCCTRL1" + message + "\n");
                    lock (_writeLock)
                    {
                        _stream.Write(bytes, 0, bytes.Length);
                        _stream.Flush();
                    }
                }
            }
            catch (Exception ex) { Program.Log("SendCtrl", ex); }
        }

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
                            else if (line.StartsWith("SETMUTE ", StringComparison.Ordinal))
                            {
                                // 宿主全屏放大: 焦点路 SETMUTE 0(取消静音), 其余路 SETMUTE 1(静音)。
                                // 按连接静音(MediaPlayer.Volume), 不能用 WASAPI 进程级(组内会误伤)。
                                if (int.TryParse(line.Substring(8), out int m) && MediaPlayerRef != null)
                                {
                                    try
                                    {
                                        // IsMuted 与 Volume 双保险: 实测部分机型仅设 Volume=0
                                        // 后音频仍输出(渲染引擎未即时生效), IsMuted 是独立静音开关
                                        MediaPlayerRef.IsMuted = (m != 0);
                                        MediaPlayerRef.Volume = (m != 0) ? 0.0 : 1.0;
                                        Program.Log("Ctrl", new Exception($"SETMUTE {m} vol={MediaPlayerRef.Volume} muted={MediaPlayerRef.IsMuted}"));
                                    }
                                    catch (Exception ex)
                                    {
                                        Program.Log("Ctrl", ex);
                                    }
                                }
                            }
                            else if (line.StartsWith("SETDISC", StringComparison.Ordinal))
                            {
                                // 宿主移除该投屏源: 仅断开本连接(不动其它连接)。
                                // 由 Program 挂接的 RequestDisconnect → conn.Disconnect() →
                                // OnConnectionDisconnected 清理本路状态, 服务进程与其余路保留。
                                try { RequestDisconnect?.Invoke(); }
                                catch (Exception ex) { Program.Log("Ctrl", ex); }
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

                long cb = _cbCount;

                // v3 GPU 模式: 帧直接缩放拷入共享纹理(见 QueueFrameGpu), 该拷贝即
                // 完成 MediaPlayer 交付消费 → 无临时 surface, 无 SoftwareBitmap 读回。
                if (_gpuMode)
                {
                    QueueFrameGpu(sender, tw, th, cb);
                    return;
                }

                int t0 = Environment.TickCount;
                var frame = VideoFrame.CreateAsDirect3D11SurfaceBacked(
                    Windows.Graphics.DirectX.DirectXPixelFormat.B8G8R8A8UIntNormalized,
                    tw, th);
                // 目标矩形:整帧缩放到小 surface(GPU 完成缩放, 无额外开销)
                sender.CopyFrameToVideoSurface(frame.Direct3DSurface,
                    new Windows.Foundation.Rect(0, 0, tw, th));

                // 可行性验证(每进程一次): 探测 surface 能否 QI 原生 DXGI 并共享
                SharedDxgi.ProbeOnce(frame.Direct3DSurface);

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

        // ========== v3 GPU 帧入队(回调线程, 共享纹理直拷) ==========
        // 每次回调都必须消费交付(否则 MediaPlayer 停摆):
        // - 拿到槽写权 → CopyFrameToVideoSurface 直拷(缩放 GPU 完成) → 释放
        // - 限帧节流(拷贝前判定): 发送帧写交替槽并翻转指针; 超频帧固定写备用槽
        //   (不翻转, 永不覆盖宿主正在采样的显示槽), 不更新 pending
        // - 纹理环尺寸/SETEDGE 变化: 重建(gen+1), 旧环交 SendLoop 销毁
        // - 拿锁超时(宿主长时间持锁, 极罕见): 临时 surface 消费本次交付, 丢帧保稳

        private void QueueFrameGpu(MediaPlayer sender, int tw, int th, long cb)
        {
            // 纹理环保障(尺寸变化时重建; 锁内串行, 防回调线程竞争)
            lock (_pendingLock)
            {
                if (_ring == null || _ring.Width != tw || _ring.Height != th)
                {
                    var old = _ring;
                    if (old != null)
                        _ringToDispose.Add(old);
                    _gpuGen++;
                    _ring = GpuTextureRing.TryCreate(_d3dDevice, tw, th, _port, _gpuGen, _gpuTexMode);
                    if (_ring == null && _gpuTexMode == SharedTexMode.KeyedNt)
                    {
                        // 创建矩阵: KeyedNt 失败换 NtOnly 再试(纹理名不含 mode, 同 gen 同名)
                        _gpuTexMode = SharedTexMode.NtOnly;
                        _ring = GpuTextureRing.TryCreate(_d3dDevice, tw, th, _port, _gpuGen, _gpuTexMode);
                    }
                    if (_ring == null)
                    {
                        // 纹理创建失败(两种模式) → 本路永久回退 SHM 模式。
                        // 本次交付尚未消费(直拷没做成) → 临时 surface 补一次拷贝,
                        // 防止 MediaPlayer 帧服务器停摆。
                        _gpuMode = false;
                        Program.Log("Gpu", new Exception(
                            $"ring create failed {tw}x{th}, fall back to SHM port={_port}"));
                        ConsumeWithTempSurface(sender, tw, th);
                        return;
                    }
                    _gpuHasPending = false;
                    Program.Log("Gpu", new Exception(
                        $"ring created {_ring.Width}x{_ring.Height} gen={_gpuGen} mode={_gpuTexMode} port={_port}"));
                }
            }

            // 主动限帧判定提前到拷贝之前: 决定本帧是"发送帧"(写交替槽并翻转指针)
            // 还是"超频帧"(写同一备用槽, 不翻转)。NtOnly 模式无 keyed mutex,
            // 写入端与宿主采样的同步完全靠 2 槽轮换 —— 发送帧提交 pending 后,
            // _gpuSlot 恒指向"非显示槽"; 超频帧固定写 _gpuSlot 即永不覆盖宿主
            // 正在采样的显示槽。旧实现超频帧也翻转交替, 会覆盖显示槽读到撕裂帧
            // (单路 MaxFps=30 vs 源 30fps 临界抖动、全屏聚焦其余路 SETFPS 1 时高频触发)。
            int maxFps = FpsOverride > 0 ? FpsOverride : MaxFps;
            bool send = maxFps <= 0;
            int nowT = 0;
            if (!send)
            {
                nowT = Environment.TickCount;
                int lastT = _lastSendT;
                send = lastT == int.MinValue || (nowT - lastT) >= 1000 / maxFps;
            }

            var ring = _ring;
            int slot = _gpuSlot;
            if (send)
                _gpuSlot = 1 - _gpuSlot;   // 仅发送帧推进交替指针

            if (!ring.TryAcquire(slot))
            {
                // 宿主持锁超时: 临时 surface 消费本次交付(保持帧服务器不停摆), 丢帧
                ConsumeWithTempSurface(sender, tw, th);
                return;
            }

            // 拷贝(自诊断矩阵): 首次 ArgumentException 时同 gen 换模式重建环重试一次;
            // 两种共享模式都被拒 → WinRT 默认设备自恢复(换设备重建环+拷贝), 仍败才回退 SHM
            if (!CopyToRingWithModeMatrix(sender, ring, slot, tw, th))
            {
                if (!TryRecoverWithWinRtDevice(sender, tw, th, out slot))
                {
                    _gpuMode = false;
                    Program.Log("Gpu", new Exception(
                        $"all tex modes/devices rejected {tw}x{th}, gpu mode OFF port={_port}"));
                    ConsumeWithTempSurface(sender, tw, th);
                    return;
                }
                send = true;   // 设备恢复路径强制送出(拷贝已进 slot 0, 恢复开销大不丢)
            }

            long seq = Interlocked.Increment(ref _gpuSeq);
            if (cb % 30 == 0)
                Program.Log("GpuCopy", new Exception(
                    $"#{cb} {tw}x{th} slot={slot} seq={seq} send={send}"));

            // 超频帧: 拷贝即完成消费(内容写备用槽, 不覆盖显示槽), 不更新 pending/不发头
            if (!send)
                return;

            if (maxFps > 0)
                _lastSendT = nowT;

            lock (_pendingLock)
            {
                _gpuPendingSlot = slot;
                _gpuPendingW = tw;
                _gpuPendingH = th;
                _gpuPendingSeq = seq;
                _gpuPendingGen = _ring != null ? _ring.Gen : 0;
                _gpuHasPending = true;
            }
        }

        /// <summary>临时 surface 消费本次 MediaPlayer 交付(防止帧服务器停摆), 失败仅记日志。</summary>
        private void ConsumeWithTempSurface(MediaPlayer sender, int tw, int th)
        {
            try
            {
                var tmp = VideoFrame.CreateAsDirect3D11SurfaceBacked(
                    Windows.Graphics.DirectX.DirectXPixelFormat.B8G8R8A8UIntNormalized, tw, th);
                sender.CopyFrameToVideoSurface(tmp.Direct3DSurface,
                    new Windows.Foundation.Rect(0, 0, tw, th));
                lock (_pendingLock)
                    _toDispose.Add(tmp);
            }
            catch (Exception ex) { Program.Log("GpuSkip", ex); }
        }

        /// <summary>
        /// 拷贝一次到环槽; 拷贝被拒(2026-09-24 真机实测: ArgumentException 0x80070057
        /// 与 InvalidCastException E_NOINTERFACE 两种形态)时同 gen 换模式重建环并重试一次。
        /// 两种模式都失败返回 false(调用方走 WinRT 设备恢复/回退 SHM)。
        /// </summary>
        private bool CopyToRingWithModeMatrix(MediaPlayer sender, GpuTextureRing ring,
            int slot, int tw, int th)
        {
            try
            {
                try
                {
                    // GPU 缩放直拷进共享纹理(keyed mutex 持有中)
                    sender.CopyFrameToVideoSurface(ring.Surface(slot),
                        new Windows.Foundation.Rect(0, 0, tw, th));
                    return true;
                }
                finally
                {
                    ring.Release(slot);   // 任何结果都先归还写锁再考虑切模式
                }
            }
            catch (Exception ex)
            {
                // 2026-09-24 实测: 拒绝形态不止 ArgumentException(0x80070057 bounds),
                // surface 包装修复后还有 InvalidCastException(E_NOINTERFACE)。
                // 任何拷贝失败都按"本模式被拒"处理 → 走矩阵/设备恢复/SHM 回退链,
                // 绝不让异常穿透(否则逐帧吞掉, GPU/SHM 全停)。
                Program.Log("Gpu", new Exception(
                    $"copy rejected mode={ring.Mode} {tw}x{th} slot={slot} hr=0x{ex.HResult:X8} ({ex.GetType().Name}): {ex.Message}"));
            }

            // 切模式: KeyedNt→NtOnly(单向, 不回切, 防逐帧震荡)
            var alt = (ring.Mode == SharedTexMode.KeyedNt)
                ? SharedTexMode.NtOnly : SharedTexMode.KeyedNt;
            var nr = RebuildRingSameGen(tw, th, alt);
            if (nr == null)
                return false;
            if (!nr.TryAcquire(slot))
                return false;
            try
            {
                sender.CopyFrameToVideoSurface(nr.Surface(slot),
                    new Windows.Foundation.Rect(0, 0, tw, th));
            }
            catch (Exception ex2)
            {
                Program.Log("Gpu", new Exception(
                    $"mode={alt} retry also rejected: {ex2.Message}"));
                return false;
            }
            finally
            {
                nr.Release(slot);
            }
            Program.Log("Gpu", new Exception(
                $"mode switch to {alt} retry copy OK gen={nr.Gen} port={_port}"));
            return true;
        }

        /// <summary>
        /// 同 gen 换模式重建纹理环(gen 不变 → 纹理名不变 → 宿主不受影响)。
        /// 命名共享对象的生命周期跟纹理本体走(句柄创建后即关), 旧环不先销毁
        /// 会同名冲突 → 此处就地 Dispose 旧环(罕见一次性路径, 例外于
        /// "回调线程不 Dispose"约定; 此刻宿主必然还没打开过本 gen 的纹理——
        /// 切换发生在首个成功拷贝之前)。
        /// </summary>
        private GpuTextureRing RebuildRingSameGen(int tw, int th, SharedTexMode newMode)
        {
            lock (_pendingLock)
            {
                var old = _ring;
                _ring = null;
                try { old?.Dispose(); }
                catch (Exception ex) { Program.Log("GpuRing", ex); }

                var nr = GpuTextureRing.TryCreate(_d3dDevice, tw, th, _port, _gpuGen, newMode);
                if (nr == null)
                {
                    Program.Log("Gpu", new Exception(
                        $"ring rebuild gen={_gpuGen} mode={newMode} failed"));
                    return null;
                }
                _ring = nr;
                _gpuTexMode = newMode;
                _gpuHasPending = false;
                Program.Log("Gpu", new Exception(
                    $"ring rebuilt gen={_gpuGen} mode={newMode} (same names) port={_port}"));
                return nr;
            }
        }

        /// <summary>
        /// 两种共享模式均被拷贝器拒绝后的诊断 + 自恢复(2026-09-24)。
        /// SHM 路径证明 CopyFrameToVideoSurface 接受 CreateAsDirect3D11SurfaceBacked
        /// 的 temp surface → 假设: 拷贝器只认与自己同设备(WinRT 默认设备)的目标纹理。
        /// A) 自设备非共享(Plain)纹理拷贝: OK → 阻断点是共享 MiscFlags; 拒 → 阻断点是设备;
        /// B) 从 temp surface 提取 WinRT 默认 D3D 设备指针(AddRef);
        /// C) 换该设备同 gen 同名重建共享环(KeyedNt→NtOnly 矩阵)并拷贝,
        ///    成功则切换 _d3dDevice 并经 outSlot(=0)送出本帧, 继续全 GPU 链路。
        /// </summary>
        private bool TryRecoverWithWinRtDevice(MediaPlayer sender, int tw, int th, out int outSlot)
        {
            outSlot = 0;

            // A: 诊断 —— 自设备非共享纹理(一次性, 不共享不可复用)
            try
            {
                if (DxgiInterop.TryCreateSharedTexture(_d3dDevice, tw, th, "", SharedTexMode.Plain,
                        out var ptex, out _, out var psurf, out _))
                {
                    try
                    {
                        sender.CopyFrameToVideoSurface(psurf,
                            new Windows.Foundation.Rect(0, 0, tw, th));
                        Program.Log("GpuDiag", new Exception(
                            "A: 自设备非共享纹理拷贝 OK → 拷贝器拒绝的是共享 MiscFlags"));
                    }
                    catch (Exception ex)
                    {
                        Program.Log("GpuDiag", new Exception(
                            $"A: 自设备非共享纹理拷贝也被拒({ex.Message}) → 拷贝器拒绝的是异设备纹理"));
                    }
                    finally
                    {
                        try { Marshal.FinalReleaseComObject(psurf); } catch { }
                        if (ptex != IntPtr.Zero) Marshal.Release(ptex);
                    }
                }
            }
            catch (Exception ex) { Program.Log("GpuDiag", ex); }

            // B: temp surface → WinRT 默认设备指针(GetDevice 已 AddRef, temp 可即弃)
            IntPtr dev = IntPtr.Zero;
            try
            {
                var tmp = VideoFrame.CreateAsDirect3D11SurfaceBacked(
                    Windows.Graphics.DirectX.DirectXPixelFormat.B8G8R8A8UIntNormalized, 64, 64);
                try { SharedDxgi.TryGetSurfaceDevice(tmp.Direct3DSurface, out dev); }
                finally { tmp.Dispose(); }
            }
            catch (Exception ex)
            {
                Program.Log("GpuDiag", ex);
                return false;
            }
            if (dev == IntPtr.Zero)
                return false;
            Program.Log("GpuDiag", new Exception(
                $"B: WinRT 默认设备=0x{dev.ToInt64():X} 自建设备=0x{_d3dDevice.ToInt64():X}"));

            // C: 切设备(自建引用交还给调用方 Stop 统一释放的新引用), 同 gen 同名重建环矩阵重拷
            Marshal.Release(_d3dDevice);
            _d3dDevice = dev;
            foreach (var m in new[] { SharedTexMode.KeyedNt, SharedTexMode.NtOnly })
            {
                var nr = RebuildRingSameGen(tw, th, m);
                if (nr == null)
                    continue;                    // 本模式重建失败 → 试下一模式
                bool copied = false;
                if (nr.TryAcquire(0))
                {
                    try
                    {
                        sender.CopyFrameToVideoSurface(nr.Surface(0),
                            new Windows.Foundation.Rect(0, 0, tw, th));
                        copied = true;
                    }
                    catch (Exception ex)
                    {
                        Program.Log("GpuDiag", new Exception(
                            $"C: WinRT 设备 mode={m} 拷贝被拒: {ex.Message}"));
                    }
                    finally
                    {
                        nr.Release(0);
                    }
                }
                if (!copied)
                    continue;                    // 本模式被拒 → 试下一模式
                Program.Log("GpuDiag", new Exception(
                    $"C: WinRT 设备 mode={m} 共享纹理拷贝 OK! 切换设备继续 GPU 链路"));
                outSlot = 0;
                _gpuSlot = 1;                    // 下一帧写 slot 1
                _lastSendT = int.MinValue;       // 恢复帧绕过节流立即送出
                return true;
            }
            Program.Log("GpuDiag", new Exception(
                "C: WinRT 默认设备上两种模式拷贝均被拒, 回退 SHM"));
            return false;
        }

        // ========== 发送循环(后台线程) ==========

        private async Task SendLoop()
        {
            _lastStatsT = Environment.TickCount;
            try
            {
                while (_running)
                {
                    // 统一释放被顶替的旧帧/旧纹理环(只在发送线程做 GPU Dispose,避免拖慢回调)
                    lock (_pendingLock)
                    {
                        foreach (var old in _toDispose)
                            old.Dispose();
                        _toDispose.Clear();
                        if (_ringToDispose.Count > 0)
                        {
                            foreach (var ring in _ringToDispose)
                                ring.Dispose();
                            _ringToDispose.Clear();
                        }
                    }

                    int nowT = Environment.TickCount;

                    // 3s 统计打点
                    if (nowT - _lastStatsT >= 3000)
                    {
                        int pending = (_pending != null || _gpuHasPending) ? 1 : 0;
                        Program.Log("Stats", new Exception(
                            $"cb={_cbCount} sent={_sentCount} pending={pending} gpu={_gpuMode}"));
                        _lastStatsT = nowT;
                    }

                    // 检测宿主进程断链(QProcess 的 stdin 管道从不写入,Peek 会阻塞,
                    // 因此不能用 stdin 检测;TCP 断开时 SendFrameAsync 抛异常 → break → 退出)
                    // (无代码)

                    // 取最新帧: GPU pending 优先(负载已在共享纹理, 仅发 28B 头),
                    // 否则 SHM pending(负载在共享内存槽)
                    VideoFrame vf = null;
                    int gSlot = -1, gW = 0, gH = 0, gGen = 0;
                    long gSeq = 0;
                    lock (_pendingLock)
                    {
                        if (_gpuHasPending)
                        {
                            gSlot = _gpuPendingSlot; gW = _gpuPendingW; gH = _gpuPendingH;
                            gSeq = _gpuPendingSeq; gGen = _gpuPendingGen;
                            _gpuHasPending = false;
                        }
                        else
                        {
                            vf = _pending;
                            _pending = null;
                        }
                    }
                    if (vf == null && gSlot < 0) { await Task.Delay(2); continue; }

                    try
                    {
                        if (gSlot >= 0)
                            await SendGpuHeaderAsync(gSlot, gW, gH, gSeq, gGen);
                        else
                            await SendFrameAsync(vf);
                        Interlocked.Increment(ref _sentCount);
                    }
                    catch (Exception ex)
                    {
                        Program.Log("SendFrame", ex);
                        // 该路帧通道断开: 若仍有其它存活连接(多路共享进程), 只结束本路
                        // 发送循环, 不能把整组服务进程带崩 —— 2026-08-15 实测根因:
                        // 宿主移除单路时只关本路通道, 旧代码 Environment.Exit 会杀整个
                        // Miracast 服务进程, 其余在投连接全部断开。仅当这是最后一路
                        // (宿主离开/进程被外部杀死)时才退出进程。
                        bool anyOther;
                        lock (Program.ConnectionsLock)
                        {
                            anyOther = Program.ActiveConnectionCount > 1;
                        }
                        if (anyOther)
                            return;
                        Environment.Exit(0);  // 最后一路断开 → 宿主已离开 → 退出
                    }
                    finally { vf?.Dispose(); }
                }
            }
            catch (Exception ex)
            {
                Program.Log("SendLoop", ex);
                return;   // 单路循环异常只结束本路, 不杀共享服务进程
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

        // ========== v3 GPU 帧发送(仅 28B 头, 负载在共享纹理) ==========
        // 帧头格式与 SHM 路径一致(MCVIDEO0 28B), 差异:
        //   stride=0xFFFFFFFF → 标记 GPU 帧; size 字段复用为帧序号;
        //   slot 字段低 16 位=纹理槽号, 高 16 位=环世代 gen(2026-09-24 起随头直发,
        //   接收端不再按 (w,h) 变化推导 —— 服务端换设备恢复等重建同名纹理时
        //   宿主也能感知并重开纹理)。
        // 纹理名 Local\MirrorCenterSharedTex_<port>_g<gen>_<slot>。
        private async Task SendGpuHeaderAsync(int slot, int w, int h, long seq, int gen)
        {
            var header = new byte[28];
            Buffer.BlockCopy(new byte[] {
                (byte)'M', (byte)'C', (byte)'V', (byte)'I',
                (byte)'D', (byte)'E', (byte)'O', (byte)'0' },
                0, header, 0, 8);
            BitConverter.GetBytes(w).CopyTo(header, 8);
            BitConverter.GetBytes(h).CopyTo(header, 12);
            BitConverter.GetBytes(-1).CopyTo(header, 16);        // stride=0xFFFFFFFF → GPU 帧
            BitConverter.GetBytes((int)seq).CopyTo(header, 20);  // size 复用为帧序号
            BitConverter.GetBytes((slot & 0xFFFF) | (gen << 16)).CopyTo(header, 24);

            await _stream.WriteAsync(header, 0, header.Length);

            // 每 10 帧打点一次(确认 GPU 链路在发, 无 SoftwareBitmap 读回)
            if (_sentCount == 0 || _sentCount % 10 == 0)
                Program.Log("GpuSend", new Exception(
                    $"#{_sentCount} {w}x{h} slot={slot} gen={gen} seq={seq}"));
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
                // v3: 释放共享纹理环(当前环 + 待销毁旧环)
                _gpuHasPending = false;
                _ring?.Dispose(); _ring = null;
                foreach (var ring in _ringToDispose) ring.Dispose();
                _ringToDispose.Clear();
            }
            _stream?.Dispose(); _stream = null;
            _client?.Close(); _client = null;
            _view?.Dispose(); _view = null;
            _mmf?.Dispose(); _mmf = null;
            // v3: 释放 D3D11 设备(本路独享设备, 环只借用不释放, 此处统一释放)
            if (_d3dDevice != IntPtr.Zero)
            {
                try { Marshal.Release(_d3dDevice); } catch { }
                _d3dDevice = IntPtr.Zero;
            }
        }
    }
}
