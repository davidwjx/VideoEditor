#include "WasapiOutput.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <objbase.h>

#include <Windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>

#include <QThread>

namespace VideoEditor {

namespace {

// Consecutive device I/O failures tolerated before declaring the endpoint lost.
constexpr int kIoFailureThreshold = 3;

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

void floatToPcm16(const float *samples, std::int16_t *out, std::size_t count)
{
    if (!samples || !out)
        return;
    for (std::size_t i = 0; i < count; ++i) {
        const float clamped = std::clamp(samples[i], -1.0f, 1.0f);
        out[i] = std::int16_t(std::lround(clamped * 32767.0f));
    }
}

WasapiOutput::~WasapiOutput()
{
    release();
}

void WasapiOutput::noteIoFailure() const
{
    if (m_ioFailures.fetch_add(1) + 1 >= kIoFailureThreshold)
        m_deviceLost.store(true);
}

void WasapiOutput::noteIoSuccess() const
{
    m_ioFailures.store(0);
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
    const DWORD convertFlags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
        | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, convertFlags, 200'000, 0,
                            &format, nullptr);
    if (FAILED(hr)) {
        // Last resort: plain 16-bit PCM with the same automatic conversion.
        // Dropping AUTOCONVERTPCM here would request a format the shared-mode
        // engine usually cannot provide, which made the first revision of this
        // fallback effectively dead code. write() converts to int16 itself.
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.wBitsPerSample = 16;
        format.nBlockAlign = WORD(channels * 2);
        format.nAvgBytesPerSec = DWORD(sampleRate * channels * 2);
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, convertFlags, 200'000, 0,
                                &format, nullptr);
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
    output->m_converted.resize(std::size_t(bufferFrames) * std::size_t(channels));
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
    noteIoSuccess();
    m_started.store(true);
    return true;
}

void WasapiOutput::stop()
{
    if (m_client && m_started.exchange(false))
        m_client->Stop();
}

void WasapiOutput::flush()
{
    if (!m_client)
        return;
    // IAudioClient::Reset requires a stopped stream and discards every queued
    // frame, so a resumed/seeked stream never replays stale audio.
    if (m_started.exchange(false))
        m_client->Stop();
    const HRESULT hr = m_client->Reset();
    if (FAILED(hr)) {
        noteIoFailure();
        return;
    }
    noteIoSuccess();
}

int WasapiOutput::availableFrames() const
{
    if (!m_client)
        return 0;
    UINT32 padding = 0;
    if (FAILED(m_client->GetCurrentPadding(&padding))) {
        noteIoFailure();
        return 0;
    }
    return int(m_bufferFrames > padding ? m_bufferFrames - padding : 0);
}

int WasapiOutput::write(const float *samples, int frames)
{
    if (!m_client || !m_render || !samples || frames <= 0)
        return 0;

    UINT32 padding = 0;
    if (FAILED(m_client->GetCurrentPadding(&padding))) {
        noteIoFailure();
        return 0;
    }
    const UINT32 available = m_bufferFrames > padding ? m_bufferFrames - padding : 0;
    const int writable = int(std::min<UINT32>(UINT32(frames), available));
    if (writable <= 0) {
        // Device buffer full; the engine retries on the next tick. This is not
        // an I/O failure.
        QThread::msleep(1);
        return 0;
    }

    BYTE *buffer = nullptr;
    if (FAILED(m_render->GetBuffer(UINT32(writable), &buffer)) || !buffer) {
        noteIoFailure();
        return 0;
    }

    if (m_pcm16) {
        const std::size_t count = std::size_t(writable) * std::size_t(m_channels);
        if (m_converted.size() < count)
            m_converted.resize(count);
        floatToPcm16(samples, m_converted.data(), count);
        std::memcpy(buffer, m_converted.data(), count * sizeof(std::int16_t));
    } else {
        std::memcpy(buffer, samples,
                    std::size_t(writable) * std::size_t(m_channels) * sizeof(float));
    }
    m_render->ReleaseBuffer(UINT32(writable), 0);
    noteIoSuccess();
    m_writtenFrames.fetch_add(writable);
    return writable;
}

int64_t WasapiOutput::playedFrames() const
{
    UINT32 padding = 0;
    if (m_client && SUCCEEDED(m_client->GetCurrentPadding(&padding)))
        return std::max<int64_t>(0, m_writtenFrames.load() - int64_t(padding));
    if (m_client)
        noteIoFailure();
    return m_writtenFrames.load();
}

} // namespace VideoEditor
