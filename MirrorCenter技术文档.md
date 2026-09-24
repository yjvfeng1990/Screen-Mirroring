# MirrorCenter 技术文档：视频处理 / 显示布局 / 音频策略

> 更新日期：2026-09-25（二次更新：同步早静音链路、fill 窗口稳定死区、GPU 未判定态 0.5s 快速重检、60s 空闲超时等最新调整）。本文档整理 Miracast/AirPlay 双后端投屏接收端的三大核心机制，所有数字与逻辑均已对照当前源码核实。

---

## 一、视频处理

### 1.1 链路总览

```
[UWP 服务端] MiracastReceiverSession 收流 → 解码 → 拷贝到共享纹理/共享内存
     │ TCP 帧端口 12508（28B 头）
     ▼
[桌面宿主] frameclient 收帧 → SessionView 渲染（QOpenGLWidget）
```

- AirPlay 路由 uxplay（GStreamer）进程直接渲染，不经此帧协议；但 AirPlay 路数计入总路数分档。

### 1.2 帧协议格式

- TCP 帧头 28 字节：`[MCVIDEO0][w][h][stride][size][payload]`
- `stride == w*4` 为 RAW BGRA8；`stride == 0xFFFFFFFF` 标记 GPU 共享纹理帧（仅头，无 payload）
- SHM 路径：双槽 ping/pong 共享内存，槽尾 4B seqlock（奇数=写入中，偶数=完整帧）
- 协议定义见 [frameclient.cpp](file:///d:/develop/Screen%20Mirroring/MirrorCenter/core/frameclient.cpp#L16-L24)

### 1.3 GPU 零拷贝方案（M1~M4 已收口，2026-09-24）

**收益**：CPU 全程不触碰像素。SHM 基线单路 1080p 占 44% 单核；GPU 模式 2 路 1280x720 服务端 10.9~15.3% + 宿主 8.4~14.6% 单核（含并行 AirPlay）。

**链路架构**：

```
[UWP 服务端]                                     [桌面宿主]
MediaPlayer 解码帧(GPU 显存)
  → CopyFrameToVideoSurface 直拷入
    D3D11 共享纹理 slot 0/1
  → 完整帧翻转显示槽指针
  → TCP 28B 头 stride=0xFFFFFFFF  ────────────→  frameclient 收头(无像素数据)
                                                  → ensureGpuTextures: OpenSharedResourceByName
                                                    按名打开两槽共享纹理
                                                  → blit(): GPU 内 CopyResource 共享→影子纹理
                                                  → 注册的 GL 纹理着色器采样绘制(缩放/裁切全 GPU)
```

**共享纹理模式**：

| 模式 | 同步方式 | 状态 |
|---|---|---|
| KeyedNt | keyed mutex | 拷贝器侧被拒 `0x887A0001` → 自动切换 |
| **NtOnly（默认生效）** | 无 mutex，**2 槽轮换**：写满一帧翻转显示槽指针，宿主直接读回 | 正常工作 |

- 命名规则：`Local\MirrorCenterSharedTex_<port>_g<gen>_<slot>`（gen 世代防陈旧纹理、port 隔离会话）
- **超频帧**（限帧跳过的帧）**固定写备用槽、不翻转指针**，防止覆盖宿主正在采样的显示槽（撕裂）
- 实现位置：服务端 [FrameServerSocket.cs L114-130](file:///d:/develop/Screen%20Mirroring/WinMiracastReceiver/MiracastReceiverService/FrameServerSocket.cs#L114-L130)

**宿主渲染——影子纹理方案**（[d3dinterop.cpp L174-260](file:///d:/develop/Screen%20Mirroring/MirrorCenter/core/d3dinterop.cpp#L174-L260)）：
- **为什么需要影子纹理**：NVIDIA 拒绝直接注册按名打开的跨进程共享纹理
- 流程：`OpenSharedResourceByName` 打开共享 D3D 纹理 → 创建同尺寸本地影子纹理（MiscFlags=0）→ 将**影子纹理**注册为 GL 纹理（`wglDXRegisterObjectNV`）
- 每帧序列（[drawGpu L289-330](file:///d:/develop/Screen%20Mirroring/MirrorCenter/app/sessionview.cpp#L289-L330)）：`blit()` GPU 内 `CopyResource`（共享→影子）→ `lock`/acquire → `glDrawArrays` 采样 → `unlock`/release；blit/lock 失败保留上一帧并节流告警
- `readTexture`：staging 纹理 Map 读回（黑边检测用，见 §1.5）

**回退链**：GPU 打开失败或渲染失败 → SHM 共享内存路径；frameclient 收到 SHM 头时必须置 `m_gpu.valid = 0`，否则 GPU→SHM 回退后继续采样旧纹理导致画面冻结（[frameclient.cpp L266-293](file:///d:/develop/Screen%20Mirroring/MirrorCenter/core/frameclient.cpp#L266-L293)）

关键语义（`OpenSharedResourceByName`）：
- 只能打开**另一设备**创建的资源，同设备自开返回 `E_INVALIDARG` 是合法响应
- `Local\` 前缀与无前缀等价；`Global\` 同会话不可见
- 打开端 `dwDesiredAccess` 必须覆盖创建端授予范围（仅 READ → `0x8876086A` ACCESS_DENIED）
- 创建端必须持有句柄至 Dispose，提前 CloseHandle 导致命名对象销毁（`0x80070057`）

**验证结论**（[test.md GPU 探针日志](file:///d:/develop/Screen%20Mirroring/test.md) + 真机回归）：
- 跨设备打开权限探针：READ|QUERY 成功、仅 READ `0x8876086A`（预期拒绝），两模式 × 两槽全过
- 连续 1376 GPU 帧，23 次跨进程读回验证全部通过，AcquireSync 失败 0，SHM 回退帧 0，像素 lum distinct 91~94 正常
- 断连回归：强杀服务/宿主双方不崩 PASS

### 1.4 空闲超时与会话保活

- 帧空闲超时 **60s**（`kIdleTimeoutMs = 60000`），服务端与宿主两侧对齐
- 为什么是 60s：3s/5s/15s 实测均误杀——Windows 笔记本源画面完全静止时编码器合法停发帧，可静默数分钟
- 真断开由 `session.Disconnected` 事件即时清理；60s 空闲拆除仅兜底**事件丢失**场景
- 宿主侧仅 `WindowReady` 状态才计时，超时回到 `Starting` 重启链路（[mirrorsession.cpp L310-325](file:///d:/develop/Screen%20Mirroring/MirrorCenter/core/mirrorsession.cpp#L310-L325)、[Program.cs L416-469](file:///d:/develop/Screen%20Mirroring/WinMiracastReceiver/MiracastReceiverService/Program.cs#L416-L469)）
- `AllowConnectionTakeover = true`（硬编码）：新连接可抢占已有连接

### 1.5 黑边检测与竖屏铺满（五轮迭代定稿）

**动机**：安卓竖屏源常是"横屏帧内嵌竖屏内容 + 左右黑边"，分屏格子较窄时需裁掉黑边铺满。

**检测规格**（[sessionview.cpp L35-87](file:///d:/develop/Screen%20Mirroring/MirrorCenter/app/sessionview.cpp#L35-L87) `hasSideBars`）：
- 双侧各 ≥ **22%** 帧宽的纯黑边（竖屏源理论 ≥29%，阈值收紧防横屏内容窄黑边误判）
- 中央 **30%** 区域非黑校验（排除转场/闪黑全黑帧）
- 命中后取**实际内容区** `[lb, fw-rb]` 裁切（第四轮修复：固定裁中央 9:16 对 16:10/4:3 屏不成立，会裁掉真内容）

**两条检测路径**：

| 路径 | 节奏 | 消抖 | 缓存 |
|---|---|---|---|
| GPU | staging Map 读回一帧（~3.5MB）：**未判定(-1)态 0.5s 快速重检**（新会话首帧可能是桌面/启动画面，尽快定型，否则整帧缩显最长 2s 后才拉升——"2→3 路新增 1 路缩小再拉升"根因），已判定后 2s | 首次检出立即铺满（接入画面即源真实方向）；播放中 0→1 翻转需连续 2 次检出；失效立即还原；纹理(重)开后 **400ms 宽限**（新纹理未就绪读回全黑会误判） | `m_gpuBarsX/W` 内容区比例，**跨纹理重建保留**（分档等比缩放黑边相对宽度不变，避免已铺满源闪缩回落）；唯一重置点 `clearFrame()` |
| SHM | 逐帧检测 | 连续 **12 帧**（`m_shmBarsHits`，resetToWaiting 重置） | 直接用当帧 lb/rb |

**生效范围与几何**（[sessionview.cpp L994-1123](file:///d:/develop/Screen%20Mirroring/MirrorCenter/app/sessionview.cpp#L994-L1123) `renderFrame`）：
- 仅 **2/3 分屏**（fillMode）生效；4 路及以上整帧原样
- `contentPortrait` 按内容区宽高判定：真横屏内容即使误检出黑边也回退整帧等比
- fill 防裁真内容（第五轮修复）：3 路一行三屏竖格（宽高比 ~0.59）比 Pad 竖屏内容（10:16=0.625）更窄，"高度优先铺满"必宽度溢出——**src 已是内容区，再 cover 裁的就是真内容**。修复：高度铺满宽度溢出时**退化为宽度优先铺满**（宽 100%、高度不足垂直居中留边）；2 路横格场景数值不变
- **inset 30px 已删除**（2026-09-25 用户定稿）：原 30px 是"固定 9:16 盲裁"时代防内容边缘残黑的安全垫，src 改为像素级内容区后无存在意义，布局格子间自带 4px 间距——视频完全铺满格子
- **窗口稳定死区（第六轮，用户要求窗口不抖）**：黑边每 2s 重测有像素级抖动，dst 直接跟随会一直微调。新增 `m_lastFillDst`：目标矩形与上次差 **<10px** 时沿用上次窗口纹丝不动，差值由 src 居中 cover 等比微调（边缘 ≤10px）吸收；真实变化（旋转/布局切换/格子 resize）超死区才更新窗口。退出铺满即重置，`resetToWaiting` 一并重置

**AirPlay 侧**：`video_renderer.c` 的 fill 逻辑仅对 `vh>vw` 竖屏视频裁切，无需与上述同步。

**迭代教训**：① 横屏帧黑边检测不可删（安卓竖屏源靠它铺满）；② 全黑帧会被双侧黑边检测误判（需中央非黑校验+消抖）；③ 消抖计数与判定是**源属性**，不得随分档切换/纹理重建重置，否则已铺满画面回落再拉升，观感极差。

---

## 二、显示布局

### 2.1 自动布局分档

[desktopwindow.cpp L276-288](file:///d:/develop/Screen%20Mirroring/MirrorCenter/app/desktopwindow.cpp#L276-L288) `relayout()`（case 0，`m_layoutMode` 始终为 0=自动，无手动入口）：

| 路数 | 布局 |
|---|---|
| 1 | 全屏 |
| 2 | 左右 |
| 3 | **一行三屏**（2026-09-25 用户定制，取消上2下1） |
| 4 | 四宫格 |
| 5~6 | 3x2 |
| 7~9 | 3x3 |
| 10~12 | 4x3 |
| >12 | 4x4 |

- 连接数变化实时触发 `relayout`；带防闪烁快速路径，仅布局参数变化才重建网格（[desktopwindow.cpp L362-417](file:///d:/develop/Screen%20Mirroring/MirrorCenter/app/desktopwindow.cpp#L362-L417)）
- `TopBar`/`BottomControlBar` 为**死代码**（无实例化），布局切换 UI 实际不存在

### 2.2 分辨率分档（SETEDGE）

[desktopwindow.cpp L334-359](file:///d:/develop/Screen%20Mirroring/MirrorCenter/app/desktopwindow.cpp#L334-L359)，按**宿主总活跃路数**（含 AirPlay 路）实时计算：

| 总路数 | Miracast 最大边 |
|---|---|
| 1 | 原始分辨率 |
| 2 | 1280 |
| 3~4 | 960 |
| 5~9 | 640 |
| ≥10 | 480 |

- 连接数变化触发 relayout 时对每个 Miracast 会话下发 `SETEDGE` 切换分辨率
- **全屏独占**：焦点路恢复原始分辨率，其余路降至 480 节省资源
- AirPlay 计入总数但不参与 Miracast 分档
- 反向断开时逐级回升

### 2.3 铺满模式（fillMode）

- 仅 2 路/3 路分屏 `setFillMode(true)`（[desktopwindow.cpp L303-319](file:///d:/develop/Screen%20Mirroring/MirrorCenter/app/desktopwindow.cpp#L303-L319)），单路/全屏天然 fullBleed，4 路及以上保持原比例
- 竖屏内容裁黑边铺满的几何规则见 §1.5

---

## 三、音频策略

### 3.1 策略总则（2026-09-25 第三轮重构定稿）

[desktopwindow.cpp L428-439](file:///d:/develop/Screen%20Mirroring/MirrorCenter/app/desktopwindow.cpp#L428-L439) `applyAudioPolicy()`：
- **仅全屏时生效**：焦点路强制出声，其余静音
- **无全屏不动任何路**：尊重手动操作，允许全部静音（旧版无条件选举 winner 强制出声，导致手动静音最后一路后被立即撤销——已修复删除 `m_audioView` 成员）

### 3.2 新连接默认值

[desktopwindow.cpp L441-465](file:///d:/develop/Screen%20Mirroring/MirrorCenter/app/desktopwindow.cpp#L441-L465) `applyNewConnectionAudioDefault(view)`：
- 第一路活跃会话默认出声；已有其它活跃会话时新路默认静音
- 全屏期间新路静音
- 接入点（按时间从早到晚）：
  1. **`sessionConnected`**（帧链路建立，服务端连入、早于首帧/出声）——早静音决策点，Miracast 两路视图均连接（[createMiracastPlaceholders L560](file:///d:/develop/Screen%20Mirroring/MirrorCenter/app/desktopwindow.cpp#L560)、[addSession L669](file:///d:/develop/Screen%20Mirroring/MirrorCenter/app/desktopwindow.cpp#L669)）
  2. `firstFrameReceived` / `windowAttached`（兜底重断言）
  3. AirPlay：`onGatewayClientConnected` 中**视图创建即应用**（[desktopwindow.cpp L948](file:///d:/develop/Screen%20Mirroring/MirrorCenter/app/desktopwindow.cpp#L948)），WASAPI 会话激活后经重试生效

### 3.3 手动互斥

- 信号链：静音按钮 → `SessionView::toggleMute` → 意图先行（`setMuted` 记录后无条件 `emit audioToggled`，下发失败不影响主窗口选举）→ 主窗口 `audioToggled` lambda
- **仅"手动开启"时互斥**：开某路声 → 其它活跃路 `setMuted(true)`；手动静音 → 不自动开其它路
- 连接点必须覆盖全部视图创建路径（鉴权教训：**新增信号必查三路径**）：
  1. `addSession`
  2. `onGatewayClientConnected`（AirPlay 网关）
  3. `createMiracastPlaceholders`（占位视图——第五轮根因：此处漏连导致两路 Miracast 互斥完全不生效）
- 早静音的 `sessionConnected` 连接同样遵守该规则（已在 `createMiracastPlaceholders`/`addSession` 连接，AirPlay 在网关连入处直接调用）

### 3.4 全屏快照与还原

[desktopwindow.cpp L474-490](file:///d:/develop/Screen%20Mirroring/MirrorCenter/app/desktopwindow.cpp#L474-L490) `m_preFullscreenMuted`（QHash view→isMuted）：
- 进入全屏时快照各活跃路静音状态；退出全屏时按快照恢复后 `clear`
- 全屏间切换焦点路**不重复快照**（`clear` 后首次进入才快照）
- **断开/关闭会话严禁 `setMuted(false)` 全开**（历史 BUG 根因）

### 3.5 静音下发可靠性（真机实测加固）

**SessionView 侧**（[applyMute L1472](file:///d:/develop/Screen%20Mirroring/MirrorCenter/app/sessionview.cpp#L1472)、[scheduleMuteRetry L1516](file:///d:/develop/Screen%20Mirroring/MirrorCenter/app/sessionview.cpp#L1516)）：
- `setMuted` 无句柄时也记录 `m_muted`（`m_muteApplied` 去重下发），首帧/`attachWindow` 时重断言
- `applyMute` **不再要求窗口已嵌入**（2026-09-25 早静音）：音频会话可能早于视频窗口激活，有 pid 即尝试 WASAPI 静音，压缩"新连接先出声再被静音"的窗口。旧实现"空闲实例返回 true 视为已应用"已删除——会伪标记 `m_muteApplied`，导致 attachWindow/首帧重断言被去重跳过 → 该路永不静音
- 下发失败 → `scheduleMuteRetry` 重试：**窗口嵌入前 500ms 快速档**（上限 20 次 ≈10s，音频会话激活即生效的早静音关键窗口），之后回退 2s（防异常会话刷日志）；仅会话结束（`!m_running`）停止
- `resetToWaiting` 重置 `m_muteApplied`

**早静音链路（2026-09-25 新增，消除"新投的设备先出声再被静音"）**：

Miracast 命令链（SETMUTE 先存后发）：
1. 宿主 `FrameClient::setTargetMute`：socket 未连上时**不再丢弃**，缓存 `m_pendingMute`（[frameclient.cpp L197](file:///d:/develop/Screen%20Mirroring/MirrorCenter/core/frameclient.cpp#L197)）；断开时重置 -1，重连后由新连接策略重新决策
2. 服务端连入瞬间（`onNewConnection`）**补发缓存的 SETMUTE**（[frameclient.cpp L134](file:///d:/develop/Screen%20Mirroring/MirrorCenter/core/frameclient.cpp#L134)）——此刻服务端 MediaPlayer 尚未创建
3. 服务端 `FrameServer.TargetMuted` **先存后发**（[FrameServerSocket.cs L115](file:///d:/develop/Screen%20Mirroring/WinMiracastReceiver/MiracastReceiverService/FrameServerSocket.cs#L115)）：MediaPlayerRef 未就绪时只记录意图
4. MediaPlayer 创建/重建时 `ApplyPendingMute()`（[Program.cs L333/L355](file:///d:/develop/Screen%20Mirroring/WinMiracastReceiver/MiracastReceiverService/Program.cs#L333)）在 **Play() 前补应用**（IsMuted+Volume 双保险）→ 首声前即按宿主意图静音；顺带修复 MediaPlayer 重建后静音状态丢失

宿主信号链（决策点前移）：
`MirrorSession::onFrameClientReady` → `frameConnected` → `SessionManager::sessionFrameConnected` → SDK `on_frame_link` 回调（[mirror_api.cpp](file:///d:/develop/Screen%20Mirroring/MirrorCenter/sdk/mirror_api.cpp)）→ `SessionView::sessionConnected` 信号 → 主窗口 `applyNewConnectionAudioDefault`

**WASAPI 会话控制**（[audiocontrol.cpp L20-75](file:///d:/develop/Screen%20Mirroring/MirrorCenter/app/audiocontrol.cpp#L20-L75)）：
- **假静音根因**：uxplay 进程可能有多个 WASAPI 音频会话（音频流重建会新建），命中第一个即 break → 静的是非活跃旧会话（UI 已静、声音仍在、无失败日志）
- 修复：**遍历全部匹配会话逐一 `SetMute` + `GetMute` 读回验证**，读回不符返回 false 触发重试
- 找不到会话时 `dumpSessions` 带 state（0=Inactive/1=Active/2=Expired）+ 5s 限流 dump 默认设备全部会话 pid 诊断（[audiocontrol.cpp L77-120](file:///d:/develop/Screen%20Mirroring/MirrorCenter/app/audiocontrol.cpp#L77-L120)）

---

## 附：相关文件索引

| 文件 | 职责 |
|---|---|
| [desktopwindow.cpp](file:///d:/develop/Screen%20Mirroring/MirrorCenter/app/desktopwindow.cpp) | 主窗口：relayout/分档/fillMode/音频策略 |
| [sessionview.cpp](file:///d:/develop/Screen%20Mirroring/MirrorCenter/app/sessionview.cpp) | 单路视图：渲染/黑边检测裁切/静音下发 |
| [frameclient.cpp](file:///d:/develop/Screen%20Mirroring/MirrorCenter/core/frameclient.cpp) | 帧协议客户端（SHM/GPU 双路径、SETMUTE 先存后发） |
| [mirrorsession.cpp](file:///d:/develop/Screen%20Mirroring/MirrorCenter/core/mirrorsession.cpp) | Miracast 会话宿主侧（GL 渲染/空闲超时/frameConnected） |
| [d3dinterop.cpp](file:///d:/develop/Screen%20Mirroring/MirrorCenter/core/d3dinterop.cpp) | D3D-GL 互操作：共享纹理打开/影子纹理注册/blit/读回 |
| [sessionmanager.cpp](file:///d:/develop/Screen%20Mirroring/MirrorCenter/core/sessionmanager.cpp) / [mirror_api.cpp](file:///d:/develop/Screen%20Mirroring/MirrorCenter/sdk/mirror_api.cpp) | 会话信号转发 / SDK 回调分发（on_frame_link） |
| [FrameServerSocket.cs](file:///d:/develop/Screen%20Mirroring/WinMiracastReceiver/MiracastReceiverService/FrameServerSocket.cs) | 服务端：GPU 拷贝/共享纹理/SETEDGE/SETMUTE |
| [Program.cs](file:///d:/develop/Screen%20Mirroring/WinMiracastReceiver/MiracastReceiverService/Program.cs) | 服务端主程序：会话生命周期/空闲拆除 |
| [audiocontrol.cpp](file:///d:/develop/Screen%20Mirroring/MirrorCenter/app/audiocontrol.cpp) | WASAPI 音频会话控制 |
