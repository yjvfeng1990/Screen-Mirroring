#include "audiocontrol.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <combaseapi.h>
#include <QDebug>
#include <QDateTime>

namespace mirrorui {

namespace {

struct ComInit {
    ComInit()  { m_hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); }
    ~ComInit() { if (SUCCEEDED(m_hr) || m_hr == RPC_E_CHANGED_MODE) CoUninitialize(); }
    HRESULT m_hr = E_FAIL;
    bool ok() const { return SUCCEEDED(m_hr) || m_hr == RPC_E_CHANGED_MODE; }
};

// 在单个音频设备上查找目标进程的渲染会话并设置静音。
// 重要(2026-09-25): 同一进程可能有多个音频会话(音频流重建会新建会话),
// 必须遍历**全部**匹配会话逐一设置 —— 只静第一个命中的可能是已废弃的
// 非活跃旧会话(实测假静音: UI 显示已静、声音仍在, 且无任何失败日志)。
// SetMute 无 HRESULT 状态可查, 设置后用 GetMute 读回验证并打日志。
bool setMuteOnDevice(IMMDevice *device, DWORD processId, bool mute)
{
    IAudioSessionManager2 *sessionMgr = nullptr;
    HRESULT hr = device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL,
                                  nullptr, reinterpret_cast<void **>(&sessionMgr));
    if (FAILED(hr) || !sessionMgr)
        return false;

    IAudioSessionEnumerator *sessions = nullptr;
    hr = sessionMgr->GetSessionEnumerator(&sessions);
    if (FAILED(hr) || !sessions) {
        sessionMgr->Release();
        return false;
    }

    bool found = false;
    int count = 0;
    sessions->GetCount(&count);
    for (int i = 0; i < count; ++i) {
        IAudioSessionControl *ctl = nullptr;
        if (FAILED(sessions->GetSession(i, &ctl)) || !ctl)
            continue;

        DWORD pid = 0;
        IAudioSessionControl2 *ctl2 = nullptr;
        if (SUCCEEDED(ctl->QueryInterface(IID_PPV_ARGS(&ctl2)))) {
            ctl2->GetProcessId(&pid);
            ctl2->Release();
        }
        if (pid == processId) {
            ISimpleAudioVolume *vol = nullptr;
            if (SUCCEEDED(ctl->QueryInterface(IID_PPV_ARGS(&vol))) && vol) {
                vol->SetMute(mute ? TRUE : FALSE, nullptr);
                BOOL applied = FALSE;
                const bool ok = SUCCEEDED(vol->GetMute(&applied))
                                && ((applied != FALSE) == mute);
                qInfo() << "[audiocontrol] SetMute pid" << pid << "会话" << i
                        << "mute=" << mute << "读回muted=" << (applied != FALSE)
                        << (ok ? "OK" : "不符");
                if (ok)
                    found = true;   // 任一会话读回一致即视为找到(全部会话都已设置)
                vol->Release();
            }
        }
        ctl->Release();
    }

    sessions->Release();
    sessionMgr->Release();
    return found;
}

// 诊断: 打印指定设备上全部音频会话的 pid, 定位"目标会话缺失/挂在其他进程"
// (限流: 5s 内只打印一次, 避免重试路径刷屏)
void dumpSessions(IMMDevice *device)
{
    static qint64 lastDumpMs = 0;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - lastDumpMs < 5000)
        return;
    lastDumpMs = now;

    IAudioSessionManager2 *sessionMgr = nullptr;
    HRESULT hr = device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL,
                                  nullptr, reinterpret_cast<void **>(&sessionMgr));
    if (FAILED(hr) || !sessionMgr)
        return;

    IAudioSessionEnumerator *sessions = nullptr;
    if (SUCCEEDED(sessionMgr->GetSessionEnumerator(&sessions)) && sessions) {
        int count = 0;
        sessions->GetCount(&count);
        QString list;
        for (int i = 0; i < count; ++i) {
            IAudioSessionControl *ctl = nullptr;
            if (FAILED(sessions->GetSession(i, &ctl)) || !ctl)
                continue;
            DWORD pid = 0;
            IAudioSessionControl2 *ctl2 = nullptr;
            if (SUCCEEDED(ctl->QueryInterface(IID_PPV_ARGS(&ctl2)))) {
                ctl2->GetProcessId(&pid);
                ctl2->Release();
            }
            // 会话状态: 0=Inactive 1=Active 2=Expired(区分活跃出声会话与残留旧会话)
            int state = -1;
            AudioSessionState st;
            if (SUCCEEDED(ctl->GetState(&st)))
                state = static_cast<int>(st);
            list += QStringLiteral(" pid=%1(state=%2)").arg(pid).arg(state);
            ctl->Release();
        }
        qWarning() << "[audiocontrol] 当前音频会话:" << list;
        sessions->Release();
    }
    sessionMgr->Release();
}

} // namespace

bool setProcessAudioMute(uint64_t processId, bool mute)
{
    if (processId == 0)
        return false;

    ComInit com;
    if (!com.ok())
        return false;

    const DWORD pid = static_cast<DWORD>(processId);

    // 遍历所有活动渲染设备(含默认设备), 逐一查找该进程的音频会话
    IMMDeviceEnumerator *enumerator = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                  CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr) || !enumerator)
        return false;

    bool found = false;

    // 1) 全部活动渲染设备
    IMMDeviceCollection *collection = nullptr;
    if (SUCCEEDED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE,
                                                 &collection)) && collection) {
        UINT devCount = 0;
        collection->GetCount(&devCount);
        for (UINT i = 0; i < devCount && !found; ++i) {
            IMMDevice *device = nullptr;
            if (SUCCEEDED(collection->Item(i, &device)) && device) {
                found = setMuteOnDevice(device, pid, mute);
                device->Release();
            }
        }
        collection->Release();
    }

    // 兜底: 默认控制台/多媒体设备(某些会话不挂在活动枚举上)
    if (!found) {
        for (ERole role : { eConsole, eMultimedia }) {
            IMMDevice *device = nullptr;
            if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, role,
                                                              &device)) && device) {
                found = setMuteOnDevice(device, pid, mute);
                if (!found)
                    dumpSessions(device);   // 诊断: 打印该设备实际存在的会话 pid(5s 限流)
                device->Release();
            }
            if (found)
                break;
        }
    }

    enumerator->Release();

    if (!found)
        qWarning() << "[audiocontrol] 未找到进程" << processId << "的音频会话";
    return found;
}

} // namespace mirrorui
