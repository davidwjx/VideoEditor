#include "WasapiOutput.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include <objbase.h>

#include <Windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>

#include <QThread>

namespace VideoEditor {

namespace {

void setError(QString *error, const QString &message)
{
    if (error)
        *error = message;
}

QString hexHresult(HRESULT hr)
{
    return QStringLiteral("0x%1").arg(quint32(hr), 0, 16);
}

} // namespace

WasapiOutput::~WasapiOutput()
{
    release();
}

void WasapiOutput::release()
{
    if (m_client) {
        m_client->Stop();
        m_client->Release();
        m_client = nullptr;
    }
    if (m_render) {
        m_render->Release();
        m_render = nullptr;
    }
    if (m_device) {
        m_device->Release();
        m_device = nullptr;
    }
    m_started.store(false);
    // COM is apartment/thread scoped: the sink is created and destroyed on the
    // engine's audio thread so this balances the successful CoInitializeEx.
    if (m_comInitialized) {
        CoUninitialize();
        m_comInitialized = false;
    }
}

std::unique_ptr<WasapiOutput> WasapiOutput::create(int sampleRate, int channels,
                                                   QString *error)
{
    if (sampleRate <= 0 || channels <= 0) {
        setError(error, QStringLiteral("音频输出格式无效。"));
        return nullptr;
    }

    auto output = std::unique_ptr<WasapiOutput>(new WasapiOutput);
    output->m_sampleRate = sampleRate;
    output->m_channels = channels;

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        setError(error, QStringLiteral("音频初始化失败（%1）。").arg(hexHresult(hr)));
        return nullptr;
    }
    output->m_comInitialized = SUCCEEDED(hr);

    IMMDeviceEnumerator *enumerator = nullptr;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          IID_PPV_ARGS(&enumerator));
    if (FAILED(hr) || !enumerator) {
        setError(error,
                 QStringLiteral("无法访问音频设备管理器（%1）。").arg(hexHresult(hr)));
        return nullptr;
    }

    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &output->m_device);
    enumerator->Release();
    if (FAILED(hr) || !output->m_device) {
        setError(error,
                 QStringLiteral("没有可用的默认音频输出设备（%1）。")
                     .arg(hexHresult(hr)));
        return nullptr;
    }

    IAudioClient *client = nullptr;
    hr = output->m_device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                    reinterpret_cast<void **>(&client));
    if (FAILED(hr) || !client) {
        setError(error, QStringLiteral("无法打开音频输出（%1）。").arg(hexHresult(hr)));
        return nullptr;
    }

    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    format.nChannels = WORD(channels);
    format.nSamplesPerSec = DWORD(sampleRate);
    format.wBitsPerSample = 32;
    format.nBlockAlign = WORD(channels * 4);
    format.nAvgBytesPerSec = DWORD(sampleRate * channels * 4);
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                            AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
                                | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                            200'000, 0, &format, nullptr);
    if (FAILED(hr)) {
        // Last resort: plain 16-bit PCM shared mode. write() converts.
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.wBitsPerSample = 16;
        format.nBlockAlign = WORD(channels * 2);
        format.nAvgBytesPerSec = DWORD(sampleRate * channels * 2);
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 200'000, 0, &format,
                                nullptr);
        if (SUCCEEDED(hr))
            output->m_pcm16 = true;
    }
    if (FAILED(hr)) {
        client->Release();
        setError(error,
                 QStringLiteral("音频设备不支持 48 kHz 浮点输出（%1）。")
                     .arg(hexHresult(hr)));
        return nullptr;
    }

    UINT32 bufferFrames = 0;
    hr = client->GetBufferSize(&bufferFrames);
    if (FAILED(hr) || bufferFrames == 0) {
        client->Release();
        setError(error,
                 QStringLiteral("无法获取音频缓冲区大小（%1）。").arg(hexHresult(hr)));
        return nullptr;
    }

    IAudioRenderClient *render = nullptr;
    hr = client->GetService(IID_PPV_ARGS(&render));
    if (FAILED(hr) || !render) {
        client->Release();
        setError(error,
                 QStringLiteral("无法获取音频渲染接口（%1）。").arg(hexHresult(hr)));
        return nullptr;
    }

    output->m_client = client;
    output->m_render = render;
    output->m_bufferFrames = bufferFrames;
    return output;
}

bool WasapiOutput::start(QString *error)
{
    if (!m_client) {
        setError(error, QStringLiteral("音频输出尚未打开。"));
        return false;
    }
    const HRESULT hr = m_client->Start();
    if (FAILED(hr)) {
        setError(error, QStringLiteral("无法启动音频输出（%1）。").arg(hexHresult(hr)));
        return false;
    }
    m_started.store(true);
    return true;
}

void WasapiOutput::stop()
{
    if (m_client && m_started.exchange(false))
        m_client->Stop();
}

int WasapiOutput::write(const float *samples, int frames)
{
    if (!m_client || !m_render || !samples || frames <= 0)
        return 0;

    UINT32 padding = 0;
    if (FAILED(m_client->GetCurrentPadding(&padding)))
        return 0;
    const UINT32 available = m_bufferFrames > padding ? m_bufferFrames - padding : 0;
    const int writable = int(std::min<UINT32>(UINT32(frames), available));
    if (writable <= 0) {
        // Device buffer full; let the engine retry on the next 10 ms tick.
        QThread::msleep(1);
        return 0;
    }

    BYTE *buffer = nullptr;
    if (FAILED(m_render->GetBuffer(UINT32(writable), &buffer)) || !buffer)
        return 0;

    if (m_pcm16) {
        std::vector<std::int16_t> converted(size_t(writable) * size_t(m_channels));
        for (size_t i = 0; i < converted.size(); ++i) {
            const float clamped = std::clamp(samples[i], -1.0f, 1.0f);
            converted[i] = std::int16_t(std::lround(clamped * 32767.0f));
        }
        std::memcpy(buffer, converted.data(), converted.size() * sizeof(std::int16_t));
    } else {
        std::memcpy(buffer, samples,
                    size_t(writable) * size_t(m_channels) * sizeof(float));
    }
    m_render->ReleaseBuffer(UINT32(writable), 0);
    m_writtenFrames.fetch_add(writable);
    return writable;
}

int64_t WasapiOutput::playedFrames() const
{
    UINT32 padding = 0;
    if (m_client && SUCCEEDED(m_client->GetCurrentPadding(&padding)))
        return std::max<int64_t>(0, m_writtenFrames.load() - int64_t(padding));
    return m_writtenFrames.load();
}

} // namespace VideoEditor
