#include "audiocontrol.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <combaseapi.h>
#include <QDebug>

namespace mirrorui {

namespace {

struct ComInit {
    ComInit()  { m_hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); }
    ~ComInit() { if (SUCCEEDED(m_hr) || m_hr == RPC_E_CHANGED_MODE) CoUninitialize(); }
    HRESULT m_hr = E_FAIL;
    bool ok() const { return SUCCEEDED(m_hr) || m_hr == RPC_E_CHANGED_MODE; }
};

// 在单个音频设备上查找目标进程的渲染会话并设置静音
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
    for (int i = 0; i < count && !found; ++i) {
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
                vol->Release();
                found = true;
            }
        }
        ctl->Release();
    }

    sessions->Release();
    sessionMgr->Release();
    return found;
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

    // 2) 兜底: 默认控制台/多媒体设备(某些会话不挂在活动枚举上)
    if (!found) {
        for (ERole role : { eConsole, eMultimedia }) {
            IMMDevice *device = nullptr;
            if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, role,
                                                              &device)) && device) {
                found = setMuteOnDevice(device, pid, mute);
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
