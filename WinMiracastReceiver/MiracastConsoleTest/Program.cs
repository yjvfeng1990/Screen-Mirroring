using System;
using System.Collections.Generic;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using Windows.Media;
using Windows.Media.Miracast;
using Windows.Media.Playback;
using MiracastReceiverService;

namespace MiracastConsoleTest
{
    /// <summary>
    /// M1 GPU 零拷贝验证工具(2026-09)。同一进程扮演两个角色:
    ///   1) 接收端: 桌面进程 MiracastReceiver + MediaPlayer 帧服务器 → FrameServerSocket
    ///      (internal, 经 InternalsVisibleTo 复用服务工程代码, GPU 拷贝链与正式服务一致)
    ///   2) 宿主模拟器: 监听帧端口, 收 28B 帧头; stride=0xFFFFFFFF 视为 GPU 帧 →
    ///      按名 OpenSharedResource → keyed mutex(key 0) → staging 读回 → 像素方差判定
    ///      (全同色/黑屏 = 跨设备拷贝失败)
    /// 判定 PASS: GPU 帧 > 0 且至少 1 帧像素验证通过; 只见 SHM 帧说明 GPU 模式未生效。
    /// 运行前必须退出 MirrorCenter / MiracastReceiverService(接收器冲突), 然后投屏本机。
    /// 首个验证帧会存 BMP 到 %TEMP%\mc_gpu_first_frame.bmp 供人工目检。
    /// </summary>
    internal class Program
    {
        // ---- D3D11 常量 ----
        private const uint DXGI_FORMAT_B8G8R8A8_UNORM = 87;
        private const uint D3D11_USAGE_STAGING = 3;
        private const uint D3D11_CPU_ACCESS_READ = 0x20000;
        private const uint D3D11_MAP_READ = 1;
        private const uint DXGI_SHARED_READ_WRITE = 0x80000001;
        private const ulong MAGIC_MCVIDEO0 = 0x304F45444956434DUL;   // "MCVIDEO0"

        private static readonly Guid IID_ID3D11Texture2D =
            new Guid("6f15aaf2-d208-4e89-9ab4-489535d34f9c");

        [StructLayout(LayoutKind.Sequential)]
        private struct D3D11_TEXTURE2D_DESC
        {
            public uint Width, Height, MipLevels, ArraySize, Format;
            public uint SampleCount, SampleQuality;
            public uint Usage, BindFlags, CPUAccessFlags, MiscFlags;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct D3D11_MAPPED_SUBRESOURCE
        {
            public IntPtr pData;
            public uint RowPitch, DepthPitch;
        }

        // ---- ID3D11Device1(截断 vtable: 只声明到最后一个使用的方法, 顺序严格一致) ----
        // IUnknown(3) + ID3D11Device 40 个 + ID3D11Device1 前 3 个。
        // 占位方法从不调用, 只占槽位。
        [ComImport, Guid("a04bfb29-08ef-43d6-a49c-a9bdbdcbe686"),
         InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        private interface ID3D11Device1_T
        {
            [PreserveSig] int CreateBuffer(IntPtr a, IntPtr b, out IntPtr c);
            [PreserveSig] int CreateTexture1D(IntPtr a, IntPtr b, out IntPtr c);
            [PreserveSig] int CreateTexture2D(ref D3D11_TEXTURE2D_DESC a, IntPtr b, out IntPtr c);
            // CreateTexture3D .. GetDeviceRemovedReason(34 个占位)
            [PreserveSig] int S00(); [PreserveSig] int S01(); [PreserveSig] int S02(); [PreserveSig] int S03();
            [PreserveSig] int S04(); [PreserveSig] int S05(); [PreserveSig] int S06(); [PreserveSig] int S07();
            [PreserveSig] int S08(); [PreserveSig] int S09(); [PreserveSig] int S10(); [PreserveSig] int S11();
            [PreserveSig] int S12(); [PreserveSig] int S13(); [PreserveSig] int S14(); [PreserveSig] int S15();
            [PreserveSig] int S16(); [PreserveSig] int S17(); [PreserveSig] int S18(); [PreserveSig] int S19();
            [PreserveSig] int S20(); [PreserveSig] int S21(); [PreserveSig] int S22(); [PreserveSig] int S23();
            [PreserveSig] int S24(); [PreserveSig] int S25(); [PreserveSig] int S26(); [PreserveSig] int S27();
            [PreserveSig] int S28(); [PreserveSig] int S29(); [PreserveSig] int S30(); [PreserveSig] int S31();
            [PreserveSig] int S32(); [PreserveSig] int S33();
            [PreserveSig] void GetImmediateContext(out IntPtr ctx);          // ID3D11Device 第 40 个
            [PreserveSig] int S34(); [PreserveSig] int S35();                 // SetExceptionMode/GetExceptionMode
            // ID3D11Device1(d3d11_1.h): GetImmediateContext1, CreateDeferredContext1,
            // CreateBlendState1, CreateRasterizerState1, CreateDeviceContextState,
            // OpenSharedResource1 —— 共 6 个占位, OpenSharedResourceByName 在第 7 个(槽 49)。
            // (曾只放 2 个占位 → 错位 4 槽, 调用跳到 CreateBlendState1 → AV 0xc0000005,
            // 2026-09-24 实测崩溃)
            [PreserveSig] int S36(); [PreserveSig] int S37();
            [PreserveSig] int S38(); [PreserveSig] int S39();
            [PreserveSig] int S40(); [PreserveSig] int S41();
            [PreserveSig] int OpenSharedResourceByName(                        // ID3D11Device1 第 7 个
                [MarshalAs(UnmanagedType.LPWStr)] string name,
                uint desiredAccess, ref Guid riid, out IntPtr ppResource);
        }

        // ---- ID3D11DeviceContext(截断): IUnknown(3) + DeviceChild(4) + 前 41 个 ----
        [ComImport, Guid("c0bfa96c-e089-44fb-8eaf-26f8796190da"),
         InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        private interface ID3D11DeviceContext_T
        {
            // ID3D11DeviceChild
            [PreserveSig] int C00(); [PreserveSig] int C01(); [PreserveSig] int C02(); [PreserveSig] int C03();
            // VSSetConstantBuffers .. Draw(7 个占位)
            [PreserveSig] int C04(); [PreserveSig] int C05(); [PreserveSig] int C06(); [PreserveSig] int C07();
            [PreserveSig] int C08(); [PreserveSig] int C09(); [PreserveSig] int C10();
            [PreserveSig] int Map(IntPtr resource, uint subresource, uint mapType,
                uint mapFlags, out D3D11_MAPPED_SUBRESOURCE mapped);           // 第 8 个
            [PreserveSig] void Unmap(IntPtr resource, uint subresource);       // 第 9 个
            // PSSetConstantBuffers .. CopySubresourceRegion(31 个占位)
            [PreserveSig] int C11(); [PreserveSig] int C12(); [PreserveSig] int C13(); [PreserveSig] int C14();
            [PreserveSig] int C15(); [PreserveSig] int C16(); [PreserveSig] int C17(); [PreserveSig] int C18();
            [PreserveSig] int C19(); [PreserveSig] int C20(); [PreserveSig] int C21(); [PreserveSig] int C22();
            [PreserveSig] int C23(); [PreserveSig] int C24(); [PreserveSig] int C25(); [PreserveSig] int C26();
            [PreserveSig] int C27(); [PreserveSig] int C28(); [PreserveSig] int C29(); [PreserveSig] int C30();
            [PreserveSig] int C31(); [PreserveSig] int C32(); [PreserveSig] int C33(); [PreserveSig] int C34();
            [PreserveSig] int C35(); [PreserveSig] int C36(); [PreserveSig] int C37(); [PreserveSig] int C38();
            [PreserveSig] int C39(); [PreserveSig] int C40(); [PreserveSig] int C41();
            [PreserveSig] void CopyResource(IntPtr dst, IntPtr src);           // 第 41 个
        }

        // ---- 状态 ----
        private static ID3D11Device1_T _dev1;
        private static ID3D11DeviceContext_T _ctx;
        private static IntPtr _stagingTex;
        private static int _stagingW, _stagingH;
        private static readonly Dictionary<string, IntPtr> _openedTex = new Dictionary<string, IntPtr>();
        private static int _gpuFrames, _shmFrames, _verifiedOk, _acquireFails, _verifyTries;
        private static string _bmpPath;

        private static async Task<int> Main(string[] args)
        {
            int code;
            try
            {
                code = await RunAsync(args);
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[test] 未处理异常: {ex}");
                code = 1;
            }
            Console.WriteLine("[test] 按回车退出...");
            try { Console.ReadLine(); } catch { }
            return code;
        }

        private static async Task<int> RunAsync(string[] args)
        {
            int seconds = 60;
            for (int i = 0; i < args.Length - 1; i++)
                if (args[i] == "--seconds" && !int.TryParse(args[i + 1], out seconds)) { }

            Console.WriteLine("==============================================================");
            Console.WriteLine(" MirrorCenter M1 GPU 零拷贝验证");
            Console.WriteLine(" 运行前请退出 MirrorCenter / MiracastReceiverService(接收器冲突)");
            Console.WriteLine("==============================================================");

            FrameServerSocket.GpuEnabled = true;   // 本工具显式启用 GPU 拷贝链

            // 宿主模拟器角色: 监听帧端口(FrameServerSocket 是 TCP client)
            var listener = new TcpListener(IPAddress.Loopback, 0);
            listener.Start();
            int port = ((IPEndPoint)listener.LocalEndpoint).Port;
            Console.WriteLine($"[test] frame port={port}");

            // 测试侧 D3D11 设备(打开共享纹理 + staging 读回用)
            if (!DxgiInterop.TryCreateDevice(out IntPtr testDevice))
            {
                Console.WriteLine("[test] FAIL: 测试进程 D3D11 设备创建失败");
                return 1;
            }
            _dev1 = (ID3D11Device1_T)Marshal.GetObjectForIUnknown(testDevice);
            _dev1.GetImmediateContext(out IntPtr ctx);
            _ctx = (ID3D11DeviceContext_T)Marshal.GetObjectForIUnknown(ctx);
            _bmpPath = Path.Combine(Path.GetTempPath(), "mc_gpu_first_frame.bmp");

            // GPU 预检(无需投屏): 两种 MiscFlags 模式各试创建一次 64x64 环,
            // 提前暴露 D3D11 参数问题(注意 KeyedNt 环须先 Dispose 释放命名再建 NtOnly)
            foreach (SharedTexMode pm in new[] { SharedTexMode.KeyedNt, SharedTexMode.NtOnly })
            {
                var pr = GpuTextureRing.TryCreate(testDevice, 64, 64, 0, 1, pm);
                Console.WriteLine($"[test] GPU 预检 mode={pm}: {(pr != null ? "OK(共享纹理创建成功)" : "失败(看服务端日志 GpuTex/GpuRing)")}");
                // 二分实验 v2: OpenSharedResourceByName 语义是"打开*另一设备*的资源",
                // 同设备自开 E_INVALIDARG 是合法响应(上一版实验无效)。
                // 现用独立设备 dev2 打开, 并同时试带/不带 Local\ 前缀(名字机制二分)。
                if (pr != null && DxgiInterop.TryCreateDevice(out IntPtr dev2))
                {
                    var dev2_1 = (ID3D11Device1_T)Marshal.GetObjectForIUnknown(dev2);
                    // access 组合扫描: 创建端授 READ|WRITE, 打开端逐项试子集
                    uint[] accesses = { 0x80000001u, 0x80000000u, 0x1u };
                    for (int si = 0; si < 2; si++)
                    {
                        string nm = pr.TexName(si);
                        var sb = new StringBuilder($"[test] 跨设备开 mode={pm} slot={si}:");
                        foreach (uint acc in accesses)
                        {
                            var iid0 = IID_ID3D11Texture2D;
                            int hrA = dev2_1.OpenSharedResourceByName(nm, acc, ref iid0, out IntPtr pA);
                            if (hrA >= 0) Marshal.Release(pA);
                            sb.Append($" acc=0x{acc:X8}→0x{hrA:X8}");
                        }
                        Console.WriteLine(sb.ToString());
                    }
                    Marshal.ReleaseComObject(dev2_1);
                    Marshal.Release(dev2);
                }
                pr?.Dispose();
            }
            // NT 共享二分探针: 结果写服务端日志 GpuProbe(句柄 vs 名字)
            DxgiInterop.DebugNtShareProbe(SharedTexMode.KeyedNt);
            DxgiInterop.DebugNtShareProbe(SharedTexMode.NtOnly);
            Console.WriteLine("[test] NT 共享探针完成(结果见日志 GpuProbe)");

            var cts = new CancellationTokenSource();
            var acceptTask = Task.Run(async () =>
            {
                while (!cts.IsCancellationRequested)
                {
                    TcpClient client = await listener.AcceptTcpClientAsync();
                    Console.WriteLine("[host] frame client connected");
                    _ = HostLoopAsync(client, port, cts.Token);
                }
            });

            // 接收端角色(与 MiracastReceiverService 同链路)
            try
            {
                var receiver = new MiracastReceiver();
                var settings = receiver.GetDefaultSettings();
                settings.FriendlyName = "MirrorCenter GpuTest";
                settings.AuthorizationMethod = MiracastReceiverAuthorizationMethod.None;
                settings.RequireAuthorizationFromKnownTransmitters = false;

                Console.WriteLine("[test] ApplySettings...");
                var apply = await receiver.DisconnectAllAndApplySettingsAsync(settings);
                Console.WriteLine($"[test] Apply={apply.Status}");

                var session = await receiver.CreateSessionAsync(null);
                session.AllowConnectionTakeover = true;
                session.ConnectionCreated += (s, e) =>
                    Console.WriteLine($"[test] ConnectionCreated {e.Connection.Transmitter.Name}");
                session.Disconnected += (s, e) =>
                    Console.WriteLine($"[test] Disconnected {e.Connection.Transmitter.Name}");
                session.MediaSourceCreated += (s, e) => _ = OnMediaSourceAsync(e, port);

                Console.WriteLine("[test] Start...");
                var start = await session.StartAsync();
                Console.WriteLine($"[test] Start={start.Status}");
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[test] FAIL(接收器初始化): {ex.Message}");
                return 1;
            }

            Console.WriteLine($"[test] 投屏本机后观察输出(最长 {seconds}s)...");
            await Task.Delay(TimeSpan.FromSeconds(seconds));
            cts.Cancel();
            listener.Stop();

            Console.WriteLine("==============================================================");
            Console.WriteLine($"[test] GPU帧={_gpuFrames} 验证尝试={_verifyTries} 通过={_verifiedOk} " +
                              $"AcquireSync失败={_acquireFails} SHM回退帧={_shmFrames}");
            bool pass = _gpuFrames > 0 && _verifiedOk > 0;
            Console.WriteLine(pass
                ? "[test] PASS: GPU 零拷贝链路工作正常(服务端直拷 + 跨进程纹理读取 + 像素正确)"
                : "[test] FAIL: 未见有效 GPU 帧(查看上方日志定位: 服务端 gpu mode / OpenSharedResource / Map)");
            Console.WriteLine("==============================================================");
            return pass ? 0 : 1;
        }

        // ---- 接收端: MediaSource → MediaPlayer 帧服务器 → FrameServerSocket ----
        private static async Task OnMediaSourceAsync(MiracastReceiverMediaSourceCreatedEventArgs e, int port)
        {
            try
            {
                var fs = new FrameServerSocket(port, "gpu-test-0");
                await fs.StartAsync();
                Console.WriteLine("[test] FrameServer connected");

                var mp = new MediaPlayer();
                mp.IsVideoFrameServerEnabled = true;
                mp.RealTimePlayback = true;   // 不缓冲, 防交付停摆
                fs.MediaPlayerRef = mp;
                mp.VideoFrameAvailable += (s, o) =>
                {
                    var ps = s.PlaybackSession;
                    int w = (int)ps.NaturalVideoWidth, h = (int)ps.NaturalVideoHeight;
                    if (w > 0 && h > 0)
                        fs.QueueFrame(s, w, h);
                };
                mp.MediaFailed += (s, ev) =>
                    Console.WriteLine($"[test] MediaFailed {ev.Error} {ev.ErrorMessage}");
                mp.Source = e.MediaSource;
                mp.Play();
                Console.WriteLine("[test] MediaPlayer playing");
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[test] MediaSource error: {ex.Message}");
            }
        }

        // ---- 宿主模拟器: 帧头接收 + GPU 帧验证 ----
        private static async Task HostLoopAsync(TcpClient client, int port, CancellationToken ct)
        {
            var stream = client.GetStream();
            var head = new byte[28];
            try
            {
                while (!ct.IsCancellationRequested)
                {
                    if (!await ReadExactAsync(stream, head, ct)) break;
                    if (BitConverter.ToUInt64(head, 0) != MAGIC_MCVIDEO0)
                    {
                        Console.WriteLine("[host] bad magic → 流失步, 断开");
                        break;
                    }
                    int w = BitConverter.ToInt32(head, 8);
                    int h = BitConverter.ToInt32(head, 12);
                    int stride = BitConverter.ToInt32(head, 16);
                    int size = BitConverter.ToInt32(head, 20);   // GPU 帧=帧序号, SHM 帧=字节数
                    int slotField = BitConverter.ToInt32(head, 24);  // 低16=槽号, 高16=gen
                    int slot = slotField & 0xFFFF;
                    int gen = (slotField >> 16) & 0xFFFF;

                    if (stride != -1)
                    {
                        int n = Interlocked.Increment(ref _shmFrames);
                        if (n == 1)
                            Console.WriteLine($"[host] SHM 帧 {w}x{h} stride={stride} → GPU 模式未生效(看服务端日志)");
                        continue;
                    }

                    int gn = Interlocked.Increment(ref _gpuFrames);
                    if (gn == 1 || gn % 30 == 0)
                        Console.WriteLine($"[host] GPU 帧 #{gn} {w}x{h} slot={slot} gen={gen} seq={size}");

                    // 第 1 帧 + 每 60 帧做一次跨进程像素验证(gen 从帧头直读)
                    if (gn == 1 || gn % 60 == 0)
                    {
                        string texName = $@"Local\MirrorCenterSharedTex_{port}_g{gen}_{slot}";
                        string saveBmp = (gn == 1) ? _bmpPath : null;
                        bool ok = VerifyFrame(texName, w, h, size, saveBmp);
                        Interlocked.Increment(ref _verifyTries);
                        if (ok) Interlocked.Increment(ref _verifiedOk);
                    }
                }
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[host] loop end: {ex.Message}");
            }
        }

        private static async Task<bool> ReadExactAsync(NetworkStream s, byte[] buf, CancellationToken ct)
        {
            int off = 0;
            while (off < buf.Length)
            {
                int n = await s.ReadAsync(buf, off, buf.Length - off, ct);
                if (n <= 0) return false;
                off += n;
            }
            return true;
        }

        /// <summary>打开共享纹理 → keyed mutex → staging 读回 → 像素方差判定。</summary>
        private static bool VerifyFrame(string texName, int w, int h, long seq, string saveBmp)
        {
            try
            {
                if (!_openedTex.TryGetValue(texName, out IntPtr tex))
                {
                    var iid = IID_ID3D11Texture2D;
                    int hr = _dev1.OpenSharedResourceByName(texName, DXGI_SHARED_READ_WRITE,
                        ref iid, out tex);
                    if (hr < 0)
                    {
                        Console.WriteLine($"[verify] OpenSharedResourceByName hr=0x{hr:X8} {texName}");
                        return false;
                    }
                    _openedTex[texName] = tex;
                    Console.WriteLine($"[verify] opened {texName}");
                }

                var texRcw = Marshal.GetObjectForIUnknown(tex);
                // QI keyed mutex: NtOnly 模式纹理无此接口(cast 抛 InvalidCastException),
                // 跳过同步直接读回(服务端侧同步退化为 2 槽轮换, 验证读回允许个别撕裂帧)
                DxgiInterop.IDXGIKeyedMutex_Cs km = null;
                try { km = (DxgiInterop.IDXGIKeyedMutex_Cs)texRcw; }
                catch (InvalidCastException)
                {
                    Console.WriteLine($"[verify] {texName} 无 keyed mutex(NtOnly), 直接读回");
                }
                int ar = (km != null) ? km.AcquireSync(0, 200) : 0;
                if (ar < 0)
                {
                    Interlocked.Increment(ref _acquireFails);
                    Console.WriteLine($"[verify] AcquireSync(0,200) hr=0x{ar:X8} (keyed mutex 超时) {texName}");
                    return false;
                }
                try
                {
                    if (_stagingTex == IntPtr.Zero || _stagingW != w || _stagingH != h)
                    {
                        if (_stagingTex != IntPtr.Zero) { Marshal.Release(_stagingTex); _stagingTex = IntPtr.Zero; }
                        var desc = new D3D11_TEXTURE2D_DESC
                        {
                            Width = (uint)w, Height = (uint)h,
                            MipLevels = 1, ArraySize = 1,
                            Format = DXGI_FORMAT_B8G8R8A8_UNORM,
                            SampleCount = 1, SampleQuality = 0,
                            Usage = D3D11_USAGE_STAGING, BindFlags = 0,
                            CPUAccessFlags = D3D11_CPU_ACCESS_READ, MiscFlags = 0,
                        };
                        int hr = _dev1.CreateTexture2D(ref desc, IntPtr.Zero, out _stagingTex);
                        if (hr < 0)
                        {
                            Console.WriteLine($"[verify] staging CreateTexture2D hr=0x{hr:X8}");
                            return false;
                        }
                        _stagingW = w; _stagingH = h;
                    }

                    _ctx.CopyResource(_stagingTex, tex);
                    int mhr = _ctx.Map(_stagingTex, 0, D3D11_MAP_READ, 0, out var mapped);
                    if (mhr < 0)
                    {
                        Console.WriteLine($"[verify] Map hr=0x{mhr:X8}");
                        return false;
                    }
                    try
                    {
                        // 像素方差: 采样 16 行 × 32 点(纯色/黑屏 = 跨设备拷贝失败)
                        int min = 255, max = 0, distinct = 0;
                        var hist = new bool[256];
                        var row = new byte[w * 4];
                        for (int ry = 0; ry < 16; ry++)
                        {
                            int y = (h - 1) * ry / 15;
                            var src = new IntPtr(mapped.pData.ToInt64() + (long)y * mapped.RowPitch);
                            Marshal.Copy(src, row, 0, row.Length);
                            for (int rx = 0; rx < 32; rx++)
                            {
                                int o = ((w - 1) * rx / 31) * 4;
                                int lum = (row[o] + row[o + 1] + row[o + 2]) / 3;
                                if (lum < min) min = lum;
                                if (lum > max) max = lum;
                                hist[lum] = true;
                            }
                        }
                        for (int i = 0; i < 256; i++) if (hist[i]) distinct++;
                        bool ok = distinct >= 8;   // 阈值: 投纯色画面时可能误报, 以 BMP 目检为准
                        Console.WriteLine($"[verify] seq={seq} {w}x{h} pitch={mapped.RowPitch} " +
                                          $"lum[{min}..{max}] distinct={distinct} → {(ok ? "OK" : "SUSPECT(纯色?)")}");
                        if (saveBmp != null) SaveBmp(saveBmp, mapped.pData, mapped.RowPitch, w, h);
                        return ok;
                    }
                    finally { _ctx.Unmap(_stagingTex, 0); }
                }
                finally { if (km != null) km.ReleaseSync(0); }
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[verify] EXC {ex.Message}");
                return false;
            }
        }

        private static void SaveBmp(string path, IntPtr data, long pitch, int w, int h)
        {
            try
            {
                int rowBytes = w * 4;
                using (var bw = new BinaryWriter(new FileStream(path, FileMode.Create, FileAccess.Write)))
                {
                    bw.Write((byte)'B'); bw.Write((byte)'M');
                    bw.Write(54 + rowBytes * h);                    // bfSize
                    bw.Write((short)0); bw.Write((short)0);         // bfReserved
                    bw.Write(54);                                   // bfOffBits
                    bw.Write(40);                                   // biSize
                    bw.Write(w); bw.Write(h);
                    bw.Write((short)1); bw.Write((short)32);
                    bw.Write(0);                                    // BI_RGB
                    bw.Write(rowBytes * h);                         // biSizeImage
                    bw.Write(0); bw.Write(0); bw.Write(0); bw.Write(0);
                    var row = new byte[rowBytes];
                    for (int y = h - 1; y >= 0; y--)
                    {
                        var src = new IntPtr(data.ToInt64() + (long)y * pitch);
                        Marshal.Copy(src, row, 0, rowBytes);
                        bw.Write(row);
                    }
                }
                Console.WriteLine($"[verify] 已存帧 {path}");
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[verify] save bmp: {ex.Message}");
            }
        }
    }
}
