using System;
using System.Runtime.InteropServices;
using Windows.Graphics.DirectX.Direct3D11;

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

    [ComImport, Guid("30961379-4609-4a41-998e-54fe567ee26c"),
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
        private static readonly Guid IID_IDXGIResource1 = new("30961379-4609-4a41-998e-54fe567ee26c");
        private const uint DXGI_SHARED_RESOURCE_READ = 0x80000000;   // 只读共享访问
        private static volatile bool _probeDone;

        /// <summary>
        /// 对 MediaPlayer 帧服务器生成的 surface 做一次性共享性探测(每进程仅一次)。
        /// 均为廉价 CPU 调用(无 GPU 同步), 可在帧回调线程执行。
        /// 注意: CsWinRT 投影对象直接强转 COM 接口会抛 InvalidCastException(不触发 QI),
        /// 必须 GetIUnknownForObject + 显式 QueryInterface 才能拿到原生接口。
        /// </summary>
        public static void ProbeOnce(IDirect3DSurface surface)
        {
            if (_probeDone) return;
            _probeDone = true;

            IntPtr pUnk = IntPtr.Zero, pAccess = IntPtr.Zero, pRes = IntPtr.Zero;
            try
            {
                // 1) 取底层 IUnknown → QI IDirect3DDXGIInterfaceAccess(官方路径)
                pUnk = Marshal.GetIUnknownForObject(surface);
                Guid iidAccess = IID_IDirect3DDXGIInterfaceAccess;
                int hrQi = Marshal.QueryInterface(pUnk, ref iidAccess, out pAccess);
                if (hrQi != 0)
                {
                    Program.Log("Probe", new Exception(
                        $"QI IDirect3DDXGIInterfaceAccess failed hr=0x{hrQi:X8} — surface 疑似封闭(拿不到原生接口)"));
                    return;
                }
                var access = (IDirect3DDXGIInterfaceAccess)Marshal.GetTypedObjectForIUnknown(
                    pAccess, typeof(IDirect3DDXGIInterfaceAccess));

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
                if (hNt != IntPtr.Zero)
                    Marshal.Release(hNt);
            }
            catch (Exception ex)
            {
                Program.Log("Probe", new Exception($"probe 异常: {ex.GetType().Name}: {ex.Message}"));
            }
            finally
            {
                if (pRes != IntPtr.Zero) Marshal.Release(pRes);
                if (pAccess != IntPtr.Zero) Marshal.Release(pAccess);
                if (pUnk != IntPtr.Zero) Marshal.Release(pUnk);
            }
        }
    }
}
