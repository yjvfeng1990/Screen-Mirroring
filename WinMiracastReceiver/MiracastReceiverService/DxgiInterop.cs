using System;
using System.Runtime.InteropServices;
using Windows.Graphics.DirectX.Direct3D11;
using WinRT;

namespace MiracastReceiverService
{
    // ===== D3D11 共享纹理互操作(v3 GPU 零拷贝, 2026-09) =====
    //
    // 背景: VideoFrame.CreateAsDirect3D11SurfaceBacked 产生的 surface 是封闭的
    // (QI IDirect3DDxgiInterfaceAccess = E_NOINTERFACE, 2026-08-15 实测), 无法共享。
    // 绕过方案: 自建 D3D11 设备 + 带 MISC_SHARED_NTHANDLE|KEYEDMUTEX 的纹理,
    // 用 d3d11.dll 原生导出 CreateDirect3D11SurfaceFromDXGISurface 包装成
    // IDirect3DSurface, 交给 MediaPlayer.CopyFrameToVideoSurface 做 GPU 缩放直拷,
    // 再经命名 NT 句柄跨进程共享给宿主(OpenGL WGL_NV_DX_interop2 采样渲染)。
    //
    // 同步: keyed mutex 全程 key 0(与 WGL_NV_DX_interop2 的 lock/unlock 一致):
    //   服务端 AcquireSync(0, 200ms) → CopyFrameToVideoSurface → ReleaseSync(0)
    //   宿主 wglDXLockObjectsNV(内部 acquire key 0) → 采样 → unlock
    // 互斥由 keyed mutex 保证, 帧就绪信号走 TCP 帧头, 不依赖 key 值交替。
    // 超时 200ms 失败一律丢帧走临时 surface 消费, 不无限等待。

    /// <summary>
    /// 共享纹理 MiscFlags 模式(自诊断矩阵)。
    /// CopyFrameToVideoSurface 对带 KEYEDMUTEX 的纹理抛 ArgumentException 时
    /// (2026-09-24 真机实测), 自动切换 NtOnly 重试; 纹理名两种模式一致,
    /// 宿主按名打开不受影响。
    /// </summary>
    internal enum SharedTexMode
    {
        /// <summary>KEYEDMUTEX|NTHANDLE(首选: 服务端写/宿主读双向同步)</summary>
        KeyedNt = 0,
        /// <summary>仅 NTHANDLE(拷贝器不处理 keyed mutex 时的回退, 同步退化为 2 槽轮换)</summary>
        NtOnly = 1,
        /// <summary>非共享普通纹理(仅诊断: 判定拷贝器拒绝的是"共享标志"还是"设备", 不可跨进程)</summary>
        Plain = 2,
    }

    internal static class DxgiInterop
    {
        // ---- 常量(d3d11.h / dxgi.h) ----
        private const uint D3D11_CREATE_DEVICE_BGRA_SUPPORT = 0x20;
        // VIDEO_SUPPORT=0x800(d3d11.h): MediaPlayer 帧服务器拷贝器走 VideoProcessor,
        // 要求目标设备带此标志 —— 设备缺它是 CopyFrameToVideoSurface 抛
        // ArgumentException(参数错误) 的候选根因之一(2026-09-24 排查)。
        private const uint D3D11_CREATE_DEVICE_VIDEO_SUPPORT = 0x800;
        private const uint D3D_DRIVER_TYPE_HARDWARE = 1;
        private const uint D3D11_SDK_VERSION = 7;
        private const uint D3D_FEATURE_LEVEL_11_1 = 0xb100;
        private const uint D3D_FEATURE_LEVEL_11_0 = 0xb000;
        private const uint DXGI_FORMAT_B8G8R8A8_UNORM = 87;
        private const uint D3D11_USAGE_DEFAULT = 0;
        private const uint D3D11_BIND_SHADER_RESOURCE = 0x8;
        private const uint D3D11_BIND_RENDER_TARGET = 0x20;
        // 注意: D3D11_RESOURCE_MISC_* 是 D3D11 自己的枚举, 值以 SDK 头文件 d3d11.h 为准——
        // SHARED=0x2, SHARED_KEYEDMUTEX=0x100, SHARED_NTHANDLE=0x800(不是 0x80000000!)
        // (曾误写 0x2/0x10 → SHARED|DRAWINDIRECT_ARGS; 又误写 0x80000000 → 无效位, 均 E_INVALIDARG)
        private const uint D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX = 0x100;
        private const uint D3D11_RESOURCE_MISC_SHARED_NTHANDLE = 0x800;
        // SHARED=0x2: NTHANDLE 不能单独使用(d3d11.h 文档: 须与 SHARED 或 KEYEDMUTEX 组合,
        // 否则 CreateTexture2D E_INVALIDARG, 2026-09-24 真机实测 hr=0x80070057)
        private const uint D3D11_RESOURCE_MISC_SHARED = 0x2;
        internal const uint DXGI_SHARED_RESOURCE_READ = 0x80000000;
        internal const uint DXGI_SHARED_RESOURCE_WRITE = 0x1;

        // ---- P/Invoke ----

        [DllImport("d3d11.dll", ExactSpelling = true)]
        private static extern int D3D11CreateDevice(
            IntPtr pAdapter,             // null → 默认适配器
            uint driverType,             // D3D_DRIVER_TYPE_HARDWARE
            IntPtr software,             // null
            uint flags,                  // BGRA_SUPPORT
            IntPtr pFeatureLevels,       // uint[](降序)
            uint featureLevelCount,
            uint sdkVersion,             // D3D11_SDK_VERSION
            out IntPtr ppDevice,         // ID3D11Device**
            IntPtr pFeatureLevel,        // 可 null(不取 FL)
            IntPtr ppImmediateContext);  // 可 null(不需要 context)

        // d3d11.dll 长期导出(Win8.1+): 把原生 DXGI surface 包成 WinRT
        // IDirect3DSurface(IInspectable, 同时实现 IDirect3DDxgiInterfaceAccess)
        [DllImport("d3d11.dll", ExactSpelling = true)]
        private static extern int CreateDirect3D11SurfaceFromDXGISurface(
            IntPtr pIUnknownofDXGISurface, out IntPtr ppGraphicsSurface);

        [DllImport("kernel32.dll", ExactSpelling = true, SetLastError = true)]
        private static extern bool CloseHandle(IntPtr handle);

        /// <summary>关闭命名 NT 共享句柄(GpuTextureRing.Dispose 用)。</summary>
        internal static void CloseSharedHandle(IntPtr h) => CloseHandle(h);

        // ---- COM 接口(截断 vtable: 只声明到需要的最后一个方法, 顺序必须严格一致) ----

        // ID3D11Device: IUnknown(3) + CreateBuffer + CreateTexture1D + CreateTexture2D
        [ComImport, Guid("db6f6ddb-ac77-4e88-8253-819df9bbf140"),
         InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        private interface ID3D11Device_Cs
        {
            [PreserveSig] int CreateBuffer(IntPtr pDesc, IntPtr pInitialData, out IntPtr ppBuffer);
            [PreserveSig] int CreateTexture1D(IntPtr pDesc, IntPtr pInitialData, out IntPtr ppTexture1D);
            [PreserveSig] int CreateTexture2D(ref D3D11_TEXTURE2D_DESC pDesc, IntPtr pInitialData, out IntPtr ppTexture2D);
        }

        // IDXGIKeyedMutex_Cs
        // 继承链: IUnknown → IDXGIObject(4) → IDXGIDeviceSubObject(GetDevice) → IDXGIKeyedMutex
        // 真实 IID 来自 SDK dxgi.h: {9d8e1289-d7b3-465f-8126-250e349af85d}
        [ComImport, Guid("9d8e1289-d7b3-465f-8126-250e349af85d"),
         InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        internal interface IDXGIKeyedMutex_Cs
        {
            // IDXGIObject
            [PreserveSig] int SetPrivateData(ref Guid name, uint dataSize, IntPtr pData);
            [PreserveSig] int SetPrivateDataInterface(ref Guid name, IntPtr pUnknown);
            [PreserveSig] int GetPrivateData(ref Guid name, ref uint pDataSize, IntPtr pData);
            [PreserveSig] int GetParent(ref Guid riid, out IntPtr ppParent);
            // IDXGIDeviceSubObject
            [PreserveSig] int GetDevice(ref Guid riid, out IntPtr ppDevice);
            // IDXGIKeyedMutex
            [PreserveSig] int AcquireSync(uint key, uint timeoutMs);
            [PreserveSig] int ReleaseSync(uint key);
        }

        // ID3D11Device1(截断到 OpenSharedResourceByName, 仅诊断用)
        // vtable: IUnknown(3) + ID3D11Device(40) + ID3D11Device1(7)
        // IID_ID3D11Device1 = {a04bfb29-08ef-43d6-a49c-a9bdbdcbe686} (d3d11_1.h)
        [ComImport, Guid("a04bfb29-08ef-43d6-a49c-a9bdbdcbe686"),
         InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        private interface ID3D11Device1_Cs
        {
            // ID3D11Device 40 个方法占位
            [PreserveSig] int _D00(); [PreserveSig] int _D01(); [PreserveSig] int _D02();
            [PreserveSig] int _D03(); [PreserveSig] int _D04(); [PreserveSig] int _D05();
            [PreserveSig] int _D06(); [PreserveSig] int _D07(); [PreserveSig] int _D08();
            [PreserveSig] int _D09(); [PreserveSig] int _D10(); [PreserveSig] int _D11();
            [PreserveSig] int _D12(); [PreserveSig] int _D13(); [PreserveSig] int _D14();
            [PreserveSig] int _D15(); [PreserveSig] int _D16(); [PreserveSig] int _D17();
            [PreserveSig] int _D18(); [PreserveSig] int _D19(); [PreserveSig] int _D20();
            [PreserveSig] int _D21(); [PreserveSig] int _D22(); [PreserveSig] int _D23();
            [PreserveSig] int _D24(); [PreserveSig] int _D25(); [PreserveSig] int _D26();
            [PreserveSig] int _D27(); [PreserveSig] int _D28(); [PreserveSig] int _D29();
            [PreserveSig] int _D30(); [PreserveSig] int _D31(); [PreserveSig] int _D32();
            [PreserveSig] int _D33(); [PreserveSig] int _D34(); [PreserveSig] int _D35();
            [PreserveSig] int _D36(); [PreserveSig] int _D37(); [PreserveSig] int _D38();
            [PreserveSig] int _D39();
            // ID3D11Device1: GetImmediateContext1, CreateDeferredContext1,
            // CreateBlendState1, CreateRasterizerState1, CreateDeviceContextState,
            // OpenSharedResource1, OpenSharedResourceByName
            [PreserveSig] int _E00(); [PreserveSig] int _E01(); [PreserveSig] int _E02();
            [PreserveSig] int _E03(); [PreserveSig] int _E04();
            [PreserveSig] int OpenSharedResource1(IntPtr hResource, ref Guid riid, out IntPtr ppResource);
            [PreserveSig] int OpenSharedResourceByName(
                [MarshalAs(UnmanagedType.LPWStr)] string lpName,
                uint dwDesiredAccess, ref Guid riid, out IntPtr ppResource);
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct D3D11_TEXTURE2D_DESC
        {
            public uint Width;
            public uint Height;
            public uint MipLevels;
            public uint ArraySize;
            public uint Format;
            public DXGI_SAMPLE_DESC SampleDesc;
            public uint Usage;
            public uint BindFlags;
            public uint CPUAccessFlags;
            public uint MiscFlags;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct DXGI_SAMPLE_DESC
        {
            public uint Count;
            public uint Quality;
        }

        /// <summary>
        /// 创建硬件 D3D11 设备(FL 11.1 → 11.0, BGRA + VIDEO 支持)。
        /// 成功返回 true, device 为 ID3D11Device*(调用方负责 Marshal.Release)。
        /// </summary>
        public static bool TryCreateDevice(out IntPtr device)
        {
            device = IntPtr.Zero;
            // 声明在非托管内存(固定 8B×2), 避免 GC 搬动
            // (Marshal.Copy 无 uint[] 重载, 用 int[] — 二进制布局与 UINT 数组一致)
            var fls = new[] { (int)D3D_FEATURE_LEVEL_11_1, (int)D3D_FEATURE_LEVEL_11_0 };
            var buffer = Marshal.AllocHGlobal(sizeof(uint) * fls.Length);
            try
            {
                Marshal.Copy(fls, 0, buffer, fls.Length);
                // 首选 BGRA|VIDEO(帧服务器拷贝需要 VIDEO_SUPPORT), 失败退回仅 BGRA
                uint[] flagSets =
                {
                    D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                    D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                };
                foreach (uint flags in flagSets)
                {
                    int hr = D3D11CreateDevice(IntPtr.Zero, D3D_DRIVER_TYPE_HARDWARE, IntPtr.Zero,
                        flags, buffer, (uint)fls.Length,
                        D3D11_SDK_VERSION, out IntPtr dev, IntPtr.Zero, IntPtr.Zero);
                    if (hr >= 0)
                    {
                        device = dev;
                        return true;
                    }
                    Program.Log("GpuDev", new Exception(
                        $"D3D11CreateDevice flags=0x{flags:X} hr=0x{hr:X8}"));
                }
                return false;
            }
            finally
            {
                Marshal.FreeHGlobal(buffer);
            }
        }

        /// <summary>
        /// 在指定设备上创建一个带命名 NT 共享句柄的 BGRA8 纹理(KeyedNt 模式额外带
        /// keyed mutex), 并包装成 WinRT IDirect3DSurface(供 CopyFrameToVideoSurface 使用)。
        /// 失败返回 false / null。
        /// </summary>
        public static bool TryCreateSharedTexture(IntPtr device, int w, int h, string shareName,
            SharedTexMode mode, out IntPtr tex, out IntPtr keyedMutex, out IDirect3DSurface surface,
            out IntPtr sharedHandle)
        {
            tex = IntPtr.Zero; keyedMutex = IntPtr.Zero; surface = null; sharedHandle = IntPtr.Zero;
            var desc = new D3D11_TEXTURE2D_DESC
            {
                Width = (uint)w,
                Height = (uint)h,
                MipLevels = 1,
                ArraySize = 1,
                Format = DXGI_FORMAT_B8G8R8A8_UNORM,
                SampleDesc = new DXGI_SAMPLE_DESC { Count = 1, Quality = 0 },
                Usage = D3D11_USAGE_DEFAULT,
                BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
                CPUAccessFlags = 0,
                MiscFlags = (mode == SharedTexMode.KeyedNt)
                    ? D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX | D3D11_RESOURCE_MISC_SHARED_NTHANDLE
                    : (mode == SharedTexMode.NtOnly)
                        ? D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE
                        : 0,   // Plain(诊断): 非共享
            };

            var devObj = (ID3D11Device_Cs)Marshal.GetObjectForIUnknown(device);
            int hr = devObj.CreateTexture2D(ref desc, IntPtr.Zero, out tex);
            if (hr < 0)
            {
                Program.Log("GpuTex", new Exception($"CreateTexture2D {w}x{h} mode={mode} hr=0x{hr:X8}"));
                return false;
            }

            var texUnk = Marshal.GetObjectForIUnknown(tex);
            try
            {
                // QI keyed mutex(仅 KeyedNt 模式; NtOnly 纹理无此接口, keyedMutex 保持 0)
                if (mode == SharedTexMode.KeyedNt)
                {
                    var km = (IDXGIKeyedMutex_Cs)texUnk;   // cast 触发 QI
                    keyedMutex = Marshal.GetIUnknownForObject(km);
                }

                // 命名共享句柄(NTHANDLE): 宿主用 ID3D11Device1::OpenSharedResourceByName 打开
                // Plain(诊断)纹理不可共享, 跳过
                if (mode != SharedTexMode.Plain)
                {
                    var res1 = (IDXGIResource1)texUnk;
                    IntPtr namePtr = Marshal.StringToHGlobalUni(shareName);
                    try
                    {
                        hr = res1.CreateSharedHandle(IntPtr.Zero,
                            DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                            namePtr, out sharedHandle);
                        if (hr < 0)
                        {
                            Program.Log("GpuTex", new Exception(
                                $"CreateSharedHandle hr=0x{hr:X8} name={shareName}"));
                            return false;
                        }
                        // 关键(2026-09-24): 命名 NT 对象的生命周期由*句柄*引用计数维持,
                        // 句柄关闭即对象销毁 → OpenSharedResourceByName 找不到名字
                        // (E_INVALIDARG)。必须保留句柄至纹理销毁, 由调用方负责 CloseHandle。
                    }
                    finally
                    {
                        Marshal.FreeHGlobal(namePtr);
                    }
                }

                QiMatrixProbe(tex);

                // 原生包装 → WinRT IDirect3DSurface 投影
                // 关键修正(2026-09-24 第 3 轮): IID_IDXGISurface 的正确值是
                // dxgi.h 的 {CAFCB56C-6AC3-4889-BF47-9E23BBD260EC}。
                // 之前误用 34275A43-...(错误 GUID), QI 拿到错接口指针 →
                // vtable 读 Desc 全乱(0x0), 拷贝器报 bounds 错误;
                // "共享纹理不支持 QI surface"的结论也系错误 IID 所致
                // (temp 纹理 QI 同样失败证实这点)。
                Guid iidSurf = new Guid("CAFCB56C-6AC3-4889-BF47-9E23BBD260EC");
                int qhr = Marshal.QueryInterface(tex, ref iidSurf, out IntPtr pSurf);
                if (qhr < 0)
                {
                    Program.Log("GpuTex", new Exception(
                        $"QI IDXGISurface hr=0x{qhr:X8} mode={mode}"));
                    return false;
                }
                try
                {
                    hr = CreateDirect3D11SurfaceFromDXGISurface(pSurf, out IntPtr insp);
                    if (hr < 0)
                    {
                        Program.Log("GpuTex", new Exception(
                            $"CreateDirect3D11SurfaceFromDXGISurface hr=0x{hr:X8}"));
                        return false;
                    }
                    try
                    {
                    // 两步包装(2026-09-24):
                    // 1) IInspectable RCW; 2) 显式 QI 到 IDirect3DSurface。
                    // 直接 MarshalInterface<T>.FromAbi(insp) 假定指针即 T 的 ABI 接口,
                    // 若 insp 实为 IInspectable* 则 vtable 错位——拷贝器对该对象
                    // 的内部 QI(取 D3D 设备/纹理)一律 E_NOINTERFACE。显式 QI 保证
                    // 拿到正确的接口指针, 与 temp surface(VideoFrame 内部包装)同构。
                    var inspObj = WinRT.MarshalInspectable<object>.FromAbi(insp);
                    surface = inspObj.As<IDirect3DSurface>();
                }
                catch (Exception ex)
                {
                    Program.Log("GpuTex", new Exception(
                        $"surface 投影构造失败: {ex.GetType().Name}: {ex.Message}"));
                    Marshal.Release(insp);
                    return false;
                }
                // 自校验: 投影 Description 必须与请求尺寸一致, 否则拷贝器必拒
                var d = surface.Description;
                if (d.Width != w || d.Height != h)
                {
                    Program.Log("GpuTex", new Exception(
                        $"surface Desc 错误: {d.Width}x{d.Height}, 期望 {w}x{h}"));
                    surface = null;
                    return false;
                }
                // 往返自检: 拷贝器需要从 surface QI IDirect3DDxgiInterfaceAccess
                // 取原生接口; 此 QI 失败 = 之前 E_NOINTERFACE 被拒的直接原因。
                try
                {
                    var access = surface.As<IDirect3DDXGIInterfaceAccess>();
                    Guid iidTex = new Guid("6F15AAF2-D208-4E89-9AB4-489535D34F9C"); // ID3D11Texture2D
                    int ahr = access.GetInterface(ref iidTex, out IntPtr pBack);
                    if (ahr < 0)
                    {
                        Program.Log("GpuTex", new Exception(
                            $"round-trip GetInterface(ID3D11Texture2D) hr=0x{ahr:X8}"));
                    }
                    else
                    {
                        Marshal.Release(pBack);
                        Program.Log("GpuTex", new Exception(
                            $"round-trip OK: surface QI → ID3D11Texture2D 成功 {w}x{h}"));
                    }
                }
                catch (Exception ex)
                {
                    Program.Log("GpuTex", new Exception(
                        $"round-trip QI 失败: {ex.GetType().Name}: {ex.Message}"));
                }
                return true;
                }
                finally
                {
                    Marshal.Release(pSurf);
                }
            }
            finally
            {
                // GetObjectForIUnknown 创建了 RCW, 立即释放(引用计数由 keyedMutex/surface 持有)
                if (texUnk != null) Marshal.FinalReleaseComObject(texUnk);
            }
        }

        // ===== QI 矩阵探测(每进程一次, 2026-09-24) =====
        // 目的: 搞清"能拷成功的 temp surface 纹理"与"我们的纹理"各自支持哪些
        // 接口, 定位 CreateDirect3D11SurfaceFromDXGISurface 该传什么指针。
        private static volatile bool _qiProbed;

        private static void QiMatrixProbe(IntPtr tex)
        {
            if (_qiProbed) return;
            _qiProbed = true;
            try
            {
                var sb = new System.Text.StringBuilder();
                sb.Append("QI矩阵[我们的纹理]: ");
                foreach (var kv in new (string Name, string Guid)[]
                {
                    ("Texture2D", "6F15AAF2-D208-4E89-9AB4-489535D34F9C"),
                    ("IDXGISurface", "CAFCB56C-6AC3-4889-BF47-9E23BBD260EC"),
                    ("IDXGIResource", "035F3AB4-482E-4E50-B41F-8A7F8BD8960B"),
                    ("IDXGIResource1", "30961379-4609-4A41-998E-54FE567EE0C1"),
                })
                {
                    var g = new Guid(kv.Guid);
                    int h = Marshal.QueryInterface(tex, ref g, out IntPtr p);
                    if (p != IntPtr.Zero) Marshal.Release(p);
                    sb.Append($"{kv.Name}=0x{h:X8} ");
                }

                // 对照组: temp surface(VideoFrame 创建, 拷贝器认可)
                var tmp = Windows.Media.VideoFrame.CreateAsDirect3D11SurfaceBacked(
                    Windows.Graphics.DirectX.DirectXPixelFormat.B8G8R8A8UIntNormalized, 64, 64);
                try
                {
                    var acc = tmp.Direct3DSurface.As<IDirect3DDXGIInterfaceAccess>();
                    var gTex = new Guid("6F15AAF2-D208-4E89-9AB4-489535D34F9C");
                    int h2 = acc.GetInterface(ref gTex, out IntPtr pTmpTex);
                    sb.Append($"| temp: GetInterface(Texture2D)=0x{h2:X8}");
                    if (h2 >= 0)
                    {
                        try
                        {
                            var gS = new Guid("CAFCB56C-6AC3-4889-BF47-9E23BBD260EC");
                            int h3 = Marshal.QueryInterface(pTmpTex, ref gS, out IntPtr p3);
                            if (p3 != IntPtr.Zero) Marshal.Release(p3);
                            sb.Append($" temp纹理QI(IDXGISurface)=0x{h3:X8}");
                            var gR1 = new Guid("30961379-4609-4A41-998E-54FE567EE0C1");
                            int h4 = Marshal.QueryInterface(pTmpTex, ref gR1, out IntPtr p4);
                            if (p4 != IntPtr.Zero) Marshal.Release(p4);
                            sb.Append($" temp纹理QI(IDXGIResource1)=0x{h4:X8}");
                        }
                        finally { Marshal.Release(pTmpTex); }
                    }
                }
                finally { tmp.Dispose(); }
                Program.Log("GpuQi", new Exception(sb.ToString()));
            }
            catch (Exception ex) { Program.Log("GpuQi", ex); }
        }

        /// <summary>
        /// NT 共享打开机制二分诊断: dev1 创建命名共享纹理, dev2 分别试
        /// OpenSharedResource1(句柄) 与 OpenSharedResourceByName(名字)。
        /// 句柄成功+名字失败 → 名字机制问题; 都失败 → 纹理/设备层问题。
        /// </summary>
        public static void DebugNtShareProbe(SharedTexMode mode)
        {
            IntPtr dev1 = IntPtr.Zero, dev2 = IntPtr.Zero, tex = IntPtr.Zero;
            IntPtr hNt = IntPtr.Zero, hKeep = IntPtr.Zero;
            try
            {
                if (!TryCreateDevice(out dev1)) { Program.Log("GpuProbe", new Exception("dev1 创建失败")); return; }
                if (!TryCreateSharedTexture(dev1, 64, 64, "Local\\McNtProbe", mode,
                        out tex, out _, out _, out hKeep))
                { Program.Log("GpuProbe", new Exception($"tex 创建失败 mode={mode}")); return; }

                // 无名句柄(避免与 TryCreateSharedTexture 内部已建命名对象冲突)
                var res1 = (IDXGIResource1)Marshal.GetObjectForIUnknown(tex);
                int hrH = res1.CreateSharedHandle(IntPtr.Zero,
                    DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, IntPtr.Zero, out hNt);

                if (!TryCreateDevice(out dev2)) { Program.Log("GpuProbe", new Exception("dev2 创建失败")); return; }
                var dev2_1 = (ID3D11Device1_Cs)Marshal.GetTypedObjectForIUnknown(dev2, typeof(ID3D11Device1_Cs));

                var iidTex = new Guid("6F15AAF2-D208-4E89-9AB4-489535D34F9C");
                IntPtr pH = IntPtr.Zero;
                int hrByHandle = hNt != IntPtr.Zero
                    ? dev2_1.OpenSharedResource1(hNt, ref iidTex, out pH)
                    : -1;
                if (hrByHandle >= 0) Marshal.Release(pH);

                // 名字解析矩阵: 创建端用 Local\McNtProbe, 打开端试 3 种变体
                var sb = new System.Text.StringBuilder();
                foreach (string nm in new[] { "Local\\McNtProbe", "McNtProbe", "Global\\McNtProbe" })
                {
                    var iid2 = iidTex;
                    int hrN = dev2_1.OpenSharedResourceByName(nm,
                        DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, ref iid2, out IntPtr pN);
                    if (hrN >= 0) Marshal.Release(pN);
                    sb.Append($" [{nm}]=0x{hrN:X8}");
                }

                Program.Log("GpuProbe", new Exception(
                    $"mode={mode} CreateHandle(无名)=0x{hrH:X8} " +
                    $"ByHandle=0x{hrByHandle:X8} ByName:{sb}"));
            }
            catch (Exception ex) { Program.Log("GpuProbe", ex); }
            finally
            {
                if (hNt != IntPtr.Zero) CloseHandle(hNt);
                if (hKeep != IntPtr.Zero) CloseHandle(hKeep);
                if (tex != IntPtr.Zero) Marshal.Release(tex);
                if (dev2 != IntPtr.Zero) Marshal.Release(dev2);
                if (dev1 != IntPtr.Zero) Marshal.Release(dev1);
            }
        }
    }

    /// <summary>
    /// 单路连接的共享纹理环(2 槽轮换, 仅最新帧语义)。
    /// 线程约定: QueueFrame(帧回调线程)与 SendLoop(发送线程)都访问,
    /// 替换/销毁由 SendLoop 统一执行(与 VideoFrame._toDispose 同规)。
    /// </summary>
    internal sealed class GpuTextureRing : IDisposable
    {
        public const int SlotCount = 2;
        private const uint AcquireTimeoutMs = 200;

        private readonly IntPtr _device;                       // ID3D11Device*
        private readonly IntPtr[] _tex = new IntPtr[SlotCount];      // ID3D11Texture2D*
        private readonly IntPtr[] _mutex = new IntPtr[SlotCount];    // IDXGIKeyedMutex*(COM 指针)
        private readonly IntPtr[] _shrHandle = new IntPtr[SlotCount]; // 命名 NT 共享句柄(持有至 Dispose)
        private readonly object[] _mutexObj = new object[SlotCount]; // 强类型包装(RCW 保活)
        private readonly IDirect3DSurface[] _surface = new IDirect3DSurface[SlotCount];

        public readonly string[] Names = new string[SlotCount];
        public int Width { get; }
        public int Height { get; }
        public int Gen { get; }
        public SharedTexMode Mode { get; }     // KeyedNt 带 mutex; NtOnly 无(读写同步退化为槽轮换)
        public string TexName(int slot) => Names[slot];

        private GpuTextureRing(IntPtr device, int w, int h, int port, int gen, SharedTexMode mode,
            string[] names, IntPtr[] tex, IntPtr[] mutex, IntPtr[] shrHandle, object[] mutexObj, IDirect3DSurface[] surfaces)
        {
            _device = device;
            Width = w; Height = h; Gen = gen; Mode = mode;
            Names = names; _tex = tex; _mutex = mutex; _shrHandle = shrHandle; _mutexObj = mutexObj; _surface = surfaces;
        }

        /// <summary>创建纹理环; 任一槽失败即整体失败(返回 null, 已建部分内部清理)。</summary>
        public static GpuTextureRing TryCreate(IntPtr device, int w, int h, int port, int gen,
            SharedTexMode mode)
        {
            IntPtr[] tex = new IntPtr[SlotCount];
            IntPtr[] mutex = new IntPtr[SlotCount];
            IntPtr[] shrHandle = new IntPtr[SlotCount];
            object[] mutexObj = new object[SlotCount];
            IDirect3DSurface[] surfaces = new IDirect3DSurface[SlotCount];
            string[] names = new string[SlotCount];
            try
            {
                for (int i = 0; i < SlotCount; i++)
                {
                    names[i] = $@"Local\MirrorCenterSharedTex_{port}_g{gen}_{i}";
                    if (!DxgiInterop.TryCreateSharedTexture(device, w, h, names[i], mode,
                            out tex[i], out mutex[i], out surfaces[i], out shrHandle[i]))
                        break;
                    if (mutex[i] != IntPtr.Zero)
                        mutexObj[i] = Marshal.GetObjectForIUnknown(mutex[i]);
                    if (i == SlotCount - 1)
                        return new GpuTextureRing(device, w, h, port, gen, mode,
                            names, tex, mutex, shrHandle, mutexObj, surfaces);
                }
                // 半成品清理: 已建纹理立即释放 —— 命名共享对象跟句柄走,
                // 不清会残留名字, 阻塞调用方换模式后按同名重建。
                var part = new GpuTextureRing(device, w, h, port, gen, mode,
                    names, tex, mutex, shrHandle, mutexObj, surfaces);
                part.Dispose();
                return null;
            }
            catch (Exception ex)
            {
                Program.Log("GpuRing", ex);
                // 半成品清理
                var tmp = new GpuTextureRing(device, w, h, port, gen, mode, names, tex, mutex, shrHandle, mutexObj, surfaces);
                tmp.Dispose();
                return null;
            }
        }

        /// <summary>获取写入权(key 0, 200ms 超时)。NtOnly 模式无 mutex 直接放行。
        /// 失败调用方走临时 surface 消费。</summary>
        public bool TryAcquire(int slot)
        {
            if (_mutexObj[slot] == null) return true;   // NtOnly: 无 keyed mutex
            var km = (DxgiInterop.IDXGIKeyedMutex_Cs)_mutexObj[slot];
            int hr = km.AcquireSync(0, AcquireTimeoutMs);
            if (hr < 0)
                Program.Log("GpuAcq", new Exception(
                    $"slot={slot} AcquireSync(0,{AcquireTimeoutMs}) hr=0x{hr:X8}"));
            return hr >= 0;
        }

        /// <summary>释放写入权(key 0)。NtOnly 模式无操作。</summary>
        public void Release(int slot)
        {
            if (_mutexObj[slot] == null) return;
            try
            {
                var km = (DxgiInterop.IDXGIKeyedMutex_Cs)_mutexObj[slot];
                int hr = km.ReleaseSync(0);
                if (hr < 0)
                    Program.Log("GpuRel", new Exception($"slot={slot} ReleaseSync hr=0x{hr:X8}"));
            }
            catch (Exception ex)
            {
                Program.Log("GpuRel", ex);
            }
        }

        public IDirect3DSurface Surface(int slot) => _surface[slot];

        public void Dispose()
        {
            for (int i = 0; i < SlotCount; i++)
            {
                if (_surface[i] != null)
                {
                    try { Marshal.FinalReleaseComObject(_surface[i]); } catch { }
                    _surface[i] = null;
                }
                if (_mutexObj[i] != null)
                {
                    try { Marshal.FinalReleaseComObject(_mutexObj[i]); } catch { }
                    _mutexObj[i] = null;
                }
                if (_mutex[i] != IntPtr.Zero)
                {
                    try { Marshal.Release(_mutex[i]); } catch { }
                    _mutex[i] = IntPtr.Zero;
                }
                if (_tex[i] != IntPtr.Zero)
                {
                    try { Marshal.Release(_tex[i]); } catch { }
                    _tex[i] = IntPtr.Zero;
                }
                // 命名 NT 共享句柄: 最后关闭(关闭即销毁命名对象)
                if (_shrHandle[i] != IntPtr.Zero)
                {
                    try { DxgiInterop.CloseSharedHandle(_shrHandle[i]); } catch { }
                    _shrHandle[i] = IntPtr.Zero;
                }
            }
            // 注意: 设备引用归 FrameServerSocket 所有(TryCreateDevice 仅 AddRef 一次),
            // 环只借用指针, 绝不能在此释放 —— 环重建多次会过度 Release 导致设备提前销毁。
        }
    }
}
