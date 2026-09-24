using System;
using System.Runtime.InteropServices;
using Windows.Graphics.DirectX.Direct3D11;
using WinRT;

namespace MiracastReceiverService
{
    // ===== D3D11/DXGI COM 互操作(仅可行性验证) =====
    // 目标: 验证 MediaPlayer 帧服务器生成的 IDirect3DSurface 能否通过
    // IDirect3DDXGIInterfaceAccess 拿到原生 DXGI 资源, 以及该纹理是否可共享。
    // 结果决定"全 GPU 零拷贝"路径是否可行:
    //   - GetSharedHandle OK      → 纹理带 D3D11_RESOURCE_MISC_SHARED, 可跨进程传统共享
    //   - CreateSharedHandle OK   → 带 MISC_SHARED_NTHANDLE, 可用 NT 句柄跨进程
    //   - 均失败                  → 当前 surface 创建路径不可共享, 需自建可共享纹理
    //                               (自建 D3D11 纹理 + CreateDirect3D11SurfaceFromDXGISurface)

    [ComImport, Guid("A9B3D012-3DF2-4EE3-B8D1-8695F457D3C1"),
     InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    internal interface IDirect3DDXGIInterfaceAccess
    {
        [PreserveSig]
        int GetInterface(ref Guid iid, out IntPtr ppv);
    }

    // 真实 IID 来自 SDK dxgi1_2.h: 尾段 ee0c1(曾误写 ee26c → QI E_NOINTERFACE)
    [ComImport, Guid("30961379-4609-4a41-998e-54fe567ee0c1"),
     InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    internal interface IDXGIResource1
    {
        // IDXGIObject
        [PreserveSig] int SetPrivateData(ref Guid name, uint dataSize, IntPtr pData);
        [PreserveSig] int SetPrivateDataInterface(ref Guid name, IntPtr pUnknown);
        [PreserveSig] int GetPrivateData(ref Guid name, ref uint pDataSize, IntPtr pData);
        [PreserveSig] int GetParent(ref Guid riid, out IntPtr ppParent);
        // IDXGIDeviceSubObject
        [PreserveSig] int GetDevice(ref Guid riid, out IntPtr ppDevice);
        // IDXGIResource
        [PreserveSig] int GetSharedHandle(out IntPtr pSharedHandle);
        [PreserveSig] int GetUsage(out uint pUsage);
        [PreserveSig] int SetEvictionPriority(uint evictionPriority);
        [PreserveSig] int GetEvictionPriority(out uint pEvictionPriority);
        // IDXGIResource1
        [PreserveSig] int CreateSubresourceSurface(uint index, out IntPtr ppSurface);
        [PreserveSig] int CreateSharedHandle(IntPtr pAttributes, uint dwAccess, IntPtr lpName, out IntPtr pHandle);
    }

    internal static class SharedDxgi
    {
        private static readonly Guid IID_IDirect3DDXGIInterfaceAccess = new("A9B3D012-3DF2-4EE3-B8D1-8695F457D3C1");
        private static readonly Guid IID_IDXGIResource1 = new("30961379-4609-4a41-998e-54fe567ee0c1");
        // d3d11.h 核实: DEFINE_GUID(IID_ID3D11Device,0xdb6f6ddb,0xac77,0x4e88,0x82,0x53,0x81,0x9d,0xf9,0xbb,0xf1,0x40)
        private static readonly Guid IID_ID3D11Device = new("db6f6ddb-ac77-4e88-8253-819df9bbf140");
        private const uint DXGI_SHARED_RESOURCE_READ = 0x80000000;   // 只读共享访问
        private static volatile bool _probeDone;

        [DllImport("kernel32.dll", ExactSpelling = true, SetLastError = true)]
        private static extern bool CloseHandle(IntPtr handle);

        /// <summary>
        /// 从 WinRT IDirect3DSurface(如 VideoFrame.CreateAsDirect3D11SurfaceBacked 的产物)
        /// 提取底层 D3D11 设备: As<IDirect3DDxgiInterfaceAccess>(真 QI) → GetInterface
        /// (IDXGIResource1) → IDXGIDeviceSubObject::GetDevice(ID3D11Device)。
        /// 成功返回 AddRef 后的 ID3D11Device*(调用方负责 Marshal.Release), 失败 false。
        /// 注意: 投影对象上 Marshal.GetIUnknownForObject+QI 拿到的是托管包装引用,
        /// 对原生接口必 E_NOINTERFACE(2026-09-24 07:58 日志实证) → 必须走
        /// WinRT.CastExtensions.As(经 NativeObject 对原生指针做真 QueryInterface)。
        /// </summary>
        public static bool TryGetSurfaceDevice(IDirect3DSurface surface, out IntPtr device)
        {
            device = IntPtr.Zero;
            if (surface == null) return false;
            IntPtr pRes = IntPtr.Zero;
            try
            {
                var access = surface.As<IDirect3DDXGIInterfaceAccess>();
                Guid iidRes = IID_IDXGIResource1;
                int hr = access.GetInterface(ref iidRes, out pRes);
                if (hr < 0)
                {
                    Program.Log("GpuDiag", new Exception($"GetInterface(IDXGIResource1) hr=0x{hr:X8}"));
                    return false;
                }
                var res = (IDXGIResource1)Marshal.GetTypedObjectForIUnknown(pRes, typeof(IDXGIResource1));
                Guid iidDev = IID_ID3D11Device;
                hr = res.GetDevice(ref iidDev, out device);
                if (hr < 0)
                {
                    Program.Log("GpuDiag", new Exception($"GetDevice(ID3D11Device) hr=0x{hr:X8}"));
                    return false;
                }
                return true;
            }
            catch (Exception ex)
            {
                Program.Log("GpuDiag", ex);
                return false;
            }
            finally
            {
                if (pRes != IntPtr.Zero) Marshal.Release(pRes);
            }
        }

        /// <summary>
        /// 对 MediaPlayer 帧服务器生成的 surface 做一次性共享性探测(每进程仅一次)。
        /// 均为廉价 CPU 调用(无 GPU 同步), 可在帧回调线程执行。
        /// 注意: 投影对象必须经 As<T>() 做真 QI(与 TryGetSurfaceDevice 同),
        /// GetIUnknownForObject+QI 拿到的是托管包装引用, 历史上全部误报 E_NOINTERFACE。
        /// </summary>
        public static void ProbeOnce(IDirect3DSurface surface)
        {
            if (_probeDone) return;
            _probeDone = true;

            IntPtr pRes = IntPtr.Zero;
            try
            {
                // 1) As<T>() 真 QI → IDirect3DDXGIInterfaceAccess(官方路径)
                var access = surface.As<IDirect3DDXGIInterfaceAccess>();

                // 2) GetInterface → IDXGIResource1
                Guid iidRes = IID_IDXGIResource1;
                int hr = access.GetInterface(ref iidRes, out pRes);
                if (hr < 0)
                {
                    Program.Log("Probe", new Exception($"GetInterface(IDXGIResource1) failed hr=0x{hr:X8}"));
                    return;
                }
                var res = (IDXGIResource1)Marshal.GetTypedObjectForIUnknown(pRes, typeof(IDXGIResource1));

                // 3) 传统共享句柄(D3D11_RESOURCE_MISC_SHARED)
                int hr2 = res.GetSharedHandle(out IntPtr hShared);
                // 4) NT 共享句柄(D3D11_RESOURCE_MISC_SHARED_NTHANDLE)
                int hr3 = res.CreateSharedHandle(IntPtr.Zero, DXGI_SHARED_RESOURCE_READ, IntPtr.Zero, out IntPtr hNt);

                Program.Log("Probe", new Exception(
                    $"QI=OK GetSharedHandle=0x{hr2:X8}(h=0x{hShared.ToInt64():X}) " +
                    $"CreateSharedHandle(NT)=0x{hr3:X8}(h=0x{hNt.ToInt64():X})"));

                // 仅验证用, 不跨进程传递, 释放句柄
                // hNt 是 NT 内核句柄(非 COM 指针), 必须 CloseHandle;
                // 曾误用 Marshal.Release → 按错误 vtable 调用 → NullReferenceException
                if (hNt != IntPtr.Zero)
                    CloseHandle(hNt);
            }
            catch (Exception ex)
            {
                Program.Log("Probe", new Exception($"probe 异常: {ex.GetType().Name}: {ex.Message}"));
            }
            finally
            {
                if (pRes != IntPtr.Zero) Marshal.Release(pRes);
            }
        }
    }
}
