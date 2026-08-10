# Screen-Mirroring

投屏接收中心（MirrorCenter）—— 在一台 PC 上统一接收来自不同设备的投屏画面。

## 当前能力状态

| 能力 | 协议 | 后端 | 状态 |
|------|------|------|------|
| AirPlay | Apple AirPlay 2 | UxPlay (GStreamer) | ✅ 已打通 |
| Miracast | Wi-Fi Direct Miracast | UWP Miracast 接收器 | 🚧 开发中 |
| DLNA / 媒体流 | DLNA / UPnP | 规划中 | ⏳ 未开始 |

## 目录结构

- `MirrorCenter/` — 主程序（Qt）
  - `core/` — 无 UI 核心逻辑库（会话管理、帧接收）
  - `sdk/` — 对外 C ABI 共享库 `mirrorsdk.dll`
  - `app/` — 桌面窗口 + 悬浮控制台（Qt Widgets）
- `UxPlay-src/` — AirPlay 接收器（已针对本项目定制：d3d11 视频输出、窗口/全屏适配、音视频同步）
- `MiracastReceiverSample/` — 微软官方 UWP Miracast 接收示例（参考）

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

### MirrorCenter（Qt 6 + CMake）

```powershell
cmake -S MirrorCenter -B MirrorCenter/build -G Ninja
cmake --build MirrorCenter/build
```

### UxPlay

依赖 GStreamer 运行时，详见 `UxPlay-src/README.md`。
