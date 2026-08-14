#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif

#include "micedecoder.h"

#include <QThread>
#include <QDebug>

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mftransform.h>
#include <mferror.h>

namespace mirror {

namespace {

// {62BE5D44-55A9-4CA2-B2B5-095F4D2D2B6E} Microsoft H.264 Video Decoder MFT
const GUID kMsH264DecoderClsid = { 0x62be5d44, 0x55a9, 0x4ca2,
                                   { 0xb2, 0xb5, 0x09, 0x5f, 0x4d, 0x2d, 0x2b, 0x6e } };

constexpr int kFrameDuration = 333333;  // ~30fps, 100ns 单位

inline int clamp255(int v)
{
    return v < 0 ? 0 : (v > 255 ? 255 : v);
}

/* NV12 → BGRA8 */
void nv12ToBgra(const BYTE *data, LONG pitch, int w, int h,
                quint8 *bgra, int bgraPitch)
{
    if (pitch < 0) {
        data += pitch * (h - 1);
        pitch = -pitch;
    }
    const BYTE *yPlane = data;
    const BYTE *uvPlane = data + pitch * h;
    for (int j = 0; j < h; ++j) {
        const BYTE *yRow = yPlane + j * pitch;
        const BYTE *uvRow = uvPlane + (j / 2) * pitch;
        quint8 *out = bgra + j * bgraPitch;
        for (int i = 0; i < w; ++i) {
            const int Y = yRow[i];
            const int U = uvRow[(i >> 1) << 1] - 128;
            const int V = uvRow[((i >> 1) << 1) + 1] - 128;
            const int R = Y + ((1436 * V) >> 10);
            const int G = Y - ((352 * U + 731 * V) >> 10);
            const int B = Y + ((1815 * U) >> 10);
            out[0] = quint8(clamp255(B));
            out[1] = quint8(clamp255(G));
            out[2] = quint8(clamp255(R));
            out[3] = 255;
            out += 4;
        }
    }
}

/**
 * MFT 封装:创建解码器、设置类型、输入/输出,处理 STREAM_CHANGE。
 * 生命周期限定在解码线程内。
 */
struct MfH264 {
    IMFTransform *mft = nullptr;
    IMFMediaBuffer *out2d = nullptr;   // 当前 NV12 2D 输出 buffer
    int outW = 0;
    int outH = 0;
    bool streamStarted = false;

    ~MfH264() { destroy(); }

    bool createDecoder()
    {
        HRESULT hr = CoCreateInstance(kMsH264DecoderClsid, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&mft));
        if (FAILED(hr)) {
            // 兜底:枚举系统注册的同步 H.264 解码器(软件/硬件)
            MFT_REGISTER_TYPE_INFO in = { MFMediaType_Video, MFVideoFormat_H264 };
            IMFActivate **acts = nullptr;
            UINT32 n = 0;
            if (SUCCEEDED(MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER,
                                    MFT_ENUM_FLAG_SYNCMFT,
                                    &in, nullptr, &acts, &n))) {
                for (UINT32 i = 0; i < n; ++i) {
                    if (SUCCEEDED(acts[i]->ActivateObject(IID_PPV_ARGS(&mft))))
                        break;
                    acts[i]->Release();
                }
                for (UINT32 i = 0; i < n; ++i)
                    acts[i]->Release();
                CoTaskMemFree(acts);
            }
        }
        return mft != nullptr;
    }

    bool initTypes()
    {
        // 输入类型: H.264 字节流(Annex B)
        IMFMediaType *in = nullptr;
        if (FAILED(MFCreateMediaType(&in)))
            return false;
        in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        HRESULT hr = mft->SetInputType(0, in, 0);
        in->Release();
        if (FAILED(hr))
            return false;

        if (FAILED(mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0)))
            return false;
        if (FAILED(mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0)))
            return false;
        streamStarted = true;
        return true;
    }

    /* 输出类型变化(收到 SPS 后)时按可用类型配置 NV12 2D buffer */
    bool rebuildOutput()
    {
        for (DWORD i = 0;; ++i) {
            IMFMediaType *t = nullptr;
            if (FAILED(mft->GetOutputAvailableType(0, i, &t)))
                break;
            GUID sub = GUID_NULL;
            t->GetGUID(MF_MT_SUBTYPE, &sub);
            UINT32 w = 0, h = 0;
            MFGetAttributeSize(t, MF_MT_FRAME_SIZE, &w, &h);
            if (sub == MFVideoFormat_NV12 && w > 0 && h > 0) {
                HRESULT hr = mft->SetOutputType(0, t, 0);
                t->Release();
                if (FAILED(hr))
                    return false;
                IMFMediaBuffer *buf = nullptr;
                if (FAILED(MFCreate2DMediaBuffer(w, h, 0x3231564E /* 'NV12' */, FALSE, &buf)))
                    return false;
                if (out2d)
                    out2d->Release();
                out2d = buf;
                outW = int(w);
                outH = int(h);
                qInfo() << "[mice-dec] output stream" << outW << "x" << outH;
                return true;
            }
            t->Release();
        }
        return false;
    }

    void destroy()
    {
        if (mft) {
            if (streamStarted)
                mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
            mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        }
        if (out2d) {
            out2d->Release();
            out2d = nullptr;
        }
        if (mft) {
            mft->Release();
            mft = nullptr;
        }
        streamStarted = false;
    }
};

} // namespace

class MiceDecoderThread : public QThread
{
public:
    explicit MiceDecoderThread(MiceDecoder *d)
        : m_d(d)
    {
    }

protected:
    void run() override { m_d->decodeLoop(); }

private:
    MiceDecoder *m_d = nullptr;
};

MiceDecoder::MiceDecoder(QObject *parent)
    : QObject(parent)
{
}

MiceDecoder::~MiceDecoder()
{
    stop();
}

bool MiceDecoder::start()
{
    if (m_thread)
        return true;
    m_stopping = false;
    m_thread = new MiceDecoderThread(this);
    m_thread->start();
    return true;
}

void MiceDecoder::stop()
{
    if (!m_thread)
        return;
    {
        QMutexLocker l(&m_mutex);
        m_stopping = true;
        m_cond.wakeAll();
    }
    m_thread->wait(3000);
    delete m_thread;
    m_thread = nullptr;
    m_stopping = false;
    m_queue.clear();
}

void MiceDecoder::pushFrame(const QByteArray &annexB)
{
    if (!m_thread || annexB.isEmpty())
        return;
    QMutexLocker l(&m_mutex);
    if (m_queue.size() > 8) {
        // 解码落后时丢旧帧, 保持实时
        m_queue.clear();
    }
    m_queue.enqueue(annexB);
    m_cond.wakeOne();
}

void MiceDecoder::decodeLoop()
{
    HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool mfOk = SUCCEEDED(MFStartup(MF_VERSION));

    MfH264 dec;
    bool decoderOk = false;
    if (SUCCEEDED(com) && mfOk) {
        decoderOk = dec.createDecoder() && dec.initTypes();
        if (!decoderOk) {
            emit error(QStringLiteral("No H.264 decoder MFT available"));
            qWarning() << "[mice-dec] failed to create H.264 decoder MFT";
        }
    }

    LONGLONG ts = 0;
    while (true) {
        QByteArray frame;
        {
            QMutexLocker l(&m_mutex);
            while (m_queue.isEmpty() && !m_stopping)
                m_cond.wait(&m_mutex);
            if (m_queue.isEmpty())
                break;
            frame = m_queue.dequeue();
        }

        if (!decoderOk)
            continue;

        // 输入
        IMFMediaBuffer *inBuf = nullptr;
        if (FAILED(MFCreateAlignedMemoryBuffer(UINT32(frame.size()), 32, &inBuf)))
            continue;
        BYTE *p = nullptr;
        inBuf->Lock(&p, nullptr, nullptr);
        memcpy(p, frame.constData(), size_t(frame.size()));
        inBuf->Unlock();
        inBuf->SetCurrentLength(UINT32(frame.size()));

        IMFSample *inSample = nullptr;
        MFCreateSample(&inSample);
        inSample->AddBuffer(inBuf);
        inSample->SetSampleTime(ts);
        inSample->SetSampleDuration(kFrameDuration);
        ts += kFrameDuration;
        HRESULT hr = dec.mft->ProcessInput(0, inSample, 0);
        inSample->Release();
        inBuf->Release();
        if (FAILED(hr)) {
            // MF_E_NOTACCEPTING 等: 丢弃本帧
            continue;
        }

        // 输出
        for (int attempt = 0; attempt < 8; ++attempt) {
            IMFSample *outSample = nullptr;
            MFCreateSample(&outSample);
            if (dec.out2d) {
                // 注意: AddBuffer 只增加引用, buffer 内容由 MFT 写入
                outSample->AddBuffer(dec.out2d);
            } else {
                // 首次尚未确定尺寸: 用大内存 buffer 触发 STREAM_CHANGE
                IMFMediaBuffer *tmp = nullptr;
                MFCreateMemoryBuffer(16 * 1024 * 1024, &tmp);
                outSample->AddBuffer(tmp);
                tmp->Release();
            }

            MFT_OUTPUT_DATA_BUFFER out = {};
            out.dwStreamID = 0;
            out.pSample = outSample;
            DWORD status = 0;
            hr = dec.mft->ProcessOutput(0, 1, &out, &status);
            outSample->Release();

            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT)
                break;
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                if (!dec.rebuildOutput())
                    break;
                continue;   // 重新调用 ProcessOutput
            }
            if (FAILED(hr))
                break;

            if (out.pSample && dec.out2d) {
                // 读 NV12 → BGRA
                IMFMediaBuffer *mb = nullptr;
                if (SUCCEEDED(out.pSample->GetBufferByIndex(0, &mb))) {
                    IMF2DBuffer *d2 = nullptr;
                    if (SUCCEEDED(mb->QueryInterface(IID_PPV_ARGS(&d2)))) {
                        BYTE *data = nullptr;
                        LONG pitch = 0;
                        if (SUCCEEDED(d2->Lock2D(&data, &pitch))) {
                            QImage img(dec.outW, dec.outH, QImage::Format_RGB32);
                            nv12ToBgra(data, pitch, dec.outW, dec.outH,
                                       img.bits(), img.bytesPerLine());
                            d2->Unlock2D();
                            emit frameReady(img);
                        }
                        d2->Release();
                    }
                    mb->Release();
                }
            }
        }
    }

    dec.destroy();
    if (mfOk)
        MFShutdown();
    if (SUCCEEDED(com))
        CoUninitialize();
}

} // namespace mirror
