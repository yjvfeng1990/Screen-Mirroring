# Screen-Mirroring

投屏接收中心（MirrorCenter）—— 在一台 PC 上统一接收来自不同设备的投屏画面。

## 当前能力状态

| 能力 | 协议 | 后端 | 状态 |
|------|------|------|------|
| AirPlay | Apple AirPlay 2 | UxPlay (GStreamer, d3d11videosink) | ✅ 已打通 |
| Miracast | Wi-Fi Direct Miracast | 桌面进程版 MiracastReceiverService（原生 Windows.Media.Miracast） | ✅ 已打通 |
| MS-MICE | Miracast over Infrastructure（LAN 有线投屏） | 自研（mDNS + TCP 7250 + RTSP/RTP + MF H.264 解码） | ✅ 已实现，待真机联调 |
| DLNA / 媒体流 | DLNA / UPnP | 规划中 | ⏳ 未开始 |

## 目录结构

- `MirrorCenter/` — 主程序（Qt 6，无 UI 核心 + SDK + 桌面窗口）
  - `core/` — 无 UI 核心逻辑库（会话管理、FrameClient 帧接收、MS-MICE 引擎）
  - `sdk/` — 对外 C ABI 共享库 `mirrorsdk.dll`
  - `app/` — 桌面窗口 + 悬浮控制台（Qt Widgets，GL 渲染）
- `UxPlay-src/` — AirPlay 接收器（已针对本项目定制：d3d11 视频输出、窗口/全屏适配、音视频同步）
- `WinMiracastReceiver/` — Miracast 接收端（dotnet）
  - `MiracastReceiverService/` — **当前主力**桌面进程版（无窗口、无 UWP 挂起限制，Qt 以 QProcess 拉起）
  - `MiracastConsoleTest/` — 服务控制台测试台
- `MiracastReceiverSample/` — 微软官方 UWP Miracast 接收示例（参考）

## 帧链路（Miracast）

- 服务端：MediaPlayer(VideoFrameAvailable) → CopyFrameToVideoSurface → GPU 缩放读回 BGRA8
- **共享内存直传**：帧负载写入共享内存双槽（`Local\MirrorCenterFrames_<port>`），TCP 仅传 28B 头 —— 消除 8MB/帧 的回环拷贝
- **seqlock 校验**：槽尾写状态奇偶校验，宿主 memcpy 前后各读一次，杜绝写读竞争导致的花屏
- 宿主端：FrameClient → 双缓冲 QImage → OpenGL 纹理，缩放/裁切/铺满全部 GPU 完成

## 自适应分辨率分档（按总活跃路数，含 AirPlay）

| 总路数 | 读回最大边 |
|--------|-----------|
| 1 | 原始分辨率（不缩放） |
| 2 | 1280 |
| 3~4 | 960 |
| 5~9 | 640 |
| >9 | 480 |

- 服务端按连接数限帧：1 路 30fps / 2 路 20fps / 3~4 路 15fps / 5~9 路 12fps / >9 路 10fps
- 全屏放大时：焦点路恢复满帧率，其余被遮住的路降到 1fps（连接保持、开销可忽略），缩回后全部恢复

## AirPlay 能力说明

已打通能力：

- iPhone / iPad / Mac 屏幕镜像搜索到本机（mDNS 广播）
- 视频 + 音频同步接收（GStreamer 管线，d3d11videosink 硬件输出）
- 窗口模式 / 全屏切换（Alt+Enter），保持视频宽高比
- 窗口方向锁定初始投屏方向，不随设备旋转变化
- 多路会话统一管理（core → sdk → app 分层）

AirPlay 启动方式（MirrorCenter 内自动拉起，也可独立运行）：

```text
uxplay.exe -n MirrorCenter
```

## 构建

### MirrorCenter（Qt 6 + CMake，一键脚本）

```powershell
build.bat release    # 默认 release；debug 用 build.bat debug
```

脚本会：编译 Qt 主程序 → 部署 Qt DLL → `dotnet publish` MiracastReceiverService 到 `app\miracast-service\`。

手工构建：

```powershell
cmake -S MirrorCenter -B MirrorCenter/build -G Ninja -DCMAKE_PREFIX_PATH=C:/Qt/6.5.3/msvc2019_64
cmake --build MirrorCenter/build
```

### UxPlay

依赖 GStreamer 运行时，详见 `UxPlay-src/README.md`。

### Miracast 接收服务（dotnet）

```powershell
dotnet publish WinMiracastReceiver/MiracastReceiverService/MiracastReceiverService.csproj -c Release -r win-x64 --self-contained true
```

## Windows 投屏前置条件

- 投影设置 `UserPreference=1`、`AllowP2P=1`
- P2P 服务 `p2psvc` / `p2pimsvc` Running + Automatic
- USB 网卡同一时间只能一个角色：AP 热点与 Wi-Fi Direct 互斥，测投屏前先 `netsh wlan stop hostednetwork`
- 已知坑：反复强杀接收进程会导致 Wi-Fi Direct GO 栈卡死（需重启电脑），结束接收请用正常 stop()
