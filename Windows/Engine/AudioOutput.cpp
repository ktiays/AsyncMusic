//
//  Created by ktiays on 2026/10/16.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#include "AudioOutput.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>
// Defines (rather than declares) the property keys below in this file.
#include <initguid.h>
#include <propkeydef.h>
#include <functiondiscoverykeys_devpkey.h>

#include <cmath>
#include <vector>

#include "Text.h"

#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "ole32.lib")

using Microsoft::WRL::ComPtr;

namespace asyncmusic::windows {

namespace {

constexpr int kChannels = 2;

std::string describe(HRESULT result) {
    char* message = nullptr;
    const DWORD length = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
        static_cast<DWORD>(result), MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US), reinterpret_cast<char*>(&message), 0, nullptr);
    std::string text = length > 0 && message != nullptr ? std::string(message, length) : std::string();
    if (message != nullptr) LocalFree(message);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == '.')) text.pop_back();
    char code[16];
    snprintf(code, sizeof(code), "0x%08lX", static_cast<unsigned long>(result));
    return text.empty() ? std::string(code) : text + " (" + code + ")";
}

std::string friendlyName(IMMDevice* device) {
    ComPtr<IPropertyStore> properties;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &properties))) return "Audio device";
    PROPVARIANT value;
    PropVariantInit(&value);
    std::string name = "Audio device";
    if (SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &value)) && value.vt == VT_LPWSTR) {
        name = toUTF8(value.pwszVal);
    }
    PropVariantClear(&value);
    return name;
}

/// Fits `time = intercept + slope * frame` through the most recent readings.
class TimelineFit {
public:
    void reset() {
        count_ = 0;
        next_ = 0;
    }

    /// Adds a raw reading and returns the fitted time for `frame`. `jitter` is the RMS of the
    /// raw readings around the fit.
    double add(double frame, double time, double& jitter) {
        if (count_ > 0) {
            // A reading far off the line means the device skipped (a glitch or a reconfigured
            // device): the old readings no longer describe it.
            const double predicted = fittedTime(frame);
            if (count_ >= kMinimumCount && std::abs(time - predicted) > kDiscontinuity) reset();
        }
        frames_[next_] = frame;
        times_[next_] = time;
        next_ = (next_ + 1) % kCapacity;
        if (count_ < kCapacity) ++count_;
        refit(frame, time);
        jitter = jitter_;
        return count_ >= kMinimumCount ? fittedTime(frame) : time;
    }

private:
    static constexpr int kCapacity = 400;  // about 4 s of 10 ms callbacks
    static constexpr int kMinimumCount = 20;
    static constexpr double kDiscontinuity = 3e6;  // ns

    double fittedTime(double frame) const { return baseTime_ + intercept_ + slope_ * (frame - baseFrame_); }

    void refit(double baseFrame, double baseTime) {
        // Relative to the newest reading, so the sums stay small and precise.
        baseFrame_ = baseFrame;
        baseTime_ = baseTime;
        double sumX = 0, sumY = 0, sumXX = 0, sumXY = 0;
        for (int i = 0; i < count_; ++i) {
            const double x = frames_[i] - baseFrame;
            const double y = times_[i] - baseTime;
            sumX += x;
            sumY += y;
            sumXX += x * x;
            sumXY += x * y;
        }
        const double n = count_;
        const double denominator = n * sumXX - sumX * sumX;
        if (count_ < 2 || denominator <= 0) {
            slope_ = 0;
            intercept_ = 0;
            jitter_ = 0;
            return;
        }
        slope_ = (n * sumXY - sumX * sumY) / denominator;
        intercept_ = (sumY - slope_ * sumX) / n;
        double squares = 0;
        for (int i = 0; i < count_; ++i) {
            const double residual = (times_[i] - baseTime) - (intercept_ + slope_ * (frames_[i] - baseFrame));
            squares += residual * residual;
        }
        jitter_ = std::sqrt(squares / n) / 1e3;
    }

    double frames_[kCapacity] = {};
    double times_[kCapacity] = {};
    int count_ = 0;
    int next_ = 0;
    double baseFrame_ = 0;
    double baseTime_ = 0;
    double slope_ = 0;
    double intercept_ = 0;
    double jitter_ = 0;
};

}  // namespace

/// Tells the render thread when the default output device changes.
class AudioOutput::DeviceNotifications final : public IMMNotificationClient {
public:
    explicit DeviceNotifications(AudioOutput& output) : output_(output) {}

    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&references_); }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG count = InterlockedDecrement(&references_);
        if (count == 0) delete this;
        return count;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** object) override {
        if (id == __uuidof(IUnknown) || id == __uuidof(IMMNotificationClient)) {
            *object = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        if (flow == eRender && role == eConsole) {
            output_.deviceChanged_ = true;
            SetEvent(output_.wake_);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

private:
    AudioOutput& output_;
    LONG references_ = 1;
};

AudioOutput::AudioOutput(SyncedRenderer& renderer) : renderer_(renderer) {
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
}

AudioOutput::~AudioOutput() {
    stop();
    CloseHandle(wake_);
}

void AudioOutput::start() {
    if (thread_.joinable()) return;
    stopping_ = false;
    thread_ = std::thread([this] { run(); });
}

void AudioOutput::stop() {
    stopping_ = true;
    SetEvent(wake_);
    if (thread_.joinable()) thread_.join();
    std::lock_guard<std::mutex> lock(mutex_);
    status_.isRunning = false;
}

void AudioOutput::setManualOffset(double milliseconds) {
    manualOffset_ = milliseconds;
    renderer_.setPresentationDelay(static_cast<int64_t>(milliseconds * 1e6));
}

AudioOutput::Status AudioOutput::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

void AudioOutput::setStatus(const Status& status) {
    std::lock_guard<std::mutex> lock(mutex_);
    status_ = status;
}

void AudioOutput::run() {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    DWORD taskIndex = 0;
    HANDLE task = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

    ComPtr<IMMDeviceEnumerator> enumerator;
    DeviceNotifications* notifications = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)))) {
        notifications = new DeviceNotifications(*this);
        enumerator->RegisterEndpointNotificationCallback(notifications);
    }
    while (!stopping_) {
        deviceChanged_ = false;
        const bool reopenRightAway = play();
        if (stopping_) break;
        // After a failure (no device, device busy) try again a little later.
        if (!reopenRightAway) WaitForSingleObject(wake_, 1000);
    }
    if (notifications != nullptr) {
        enumerator->UnregisterEndpointNotificationCallback(notifications);
        notifications->Release();
    }
    enumerator.Reset();
    if (task != nullptr) AvRevertMmThreadCharacteristics(task);
    if (SUCCEEDED(initialized)) CoUninitialize();
}

bool AudioOutput::play() {
    Status status;
    const auto fail = [&](const std::string& message) {
        status.isRunning = false;
        status.error = message;
        setStatus(status);
        return false;
    };

    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> device;
    HRESULT result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(result)) return fail(describe(result));
    result = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (FAILED(result)) return fail("No audio output device");
    status.deviceName = friendlyName(device.Get());

    ComPtr<IAudioClient> client;
    result = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client.GetAddressOf()));
    if (FAILED(result)) return fail(describe(result));

    WAVEFORMATEX* mixFormat = nullptr;
    result = client->GetMixFormat(&mixFormat);
    if (FAILED(result)) return fail(describe(result));
    const DWORD sampleRate = mixFormat->nSamplesPerSec;
    CoTaskMemFree(mixFormat);

    // Float stereo at the device's own rate: the audio engine maps it onto the device's
    // channels without resampling.
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    format.nChannels = kChannels;
    format.nSamplesPerSec = sampleRate;
    format.wBitsPerSample = 32;
    format.nBlockAlign = static_cast<WORD>(kChannels * sizeof(float));
    format.nAvgBytesPerSec = sampleRate * format.nBlockAlign;

    const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
        | AUDCLNT_STREAMFLAGS_NOPERSIST;
    result = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 0, 0, &format, nullptr);
    if (FAILED(result)) return fail(describe(result));

    HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    struct EventCloser {
        HANDLE event;
        ~EventCloser() { CloseHandle(event); }
    } closer{ready};
    client->SetEventHandle(ready);

    ComPtr<IAudioRenderClient> renderClient;
    ComPtr<IAudioClock> clock;
    UINT32 bufferFrames = 0;
    UINT64 frequency = 0;
    REFERENCE_TIME latency = 0;
    if (FAILED(result = client->GetService(IID_PPV_ARGS(&renderClient))) || FAILED(result = client->GetService(IID_PPV_ARGS(&clock)))
        || FAILED(result = client->GetBufferSize(&bufferFrames)) || FAILED(result = clock->GetFrequency(&frequency))) {
        return fail(describe(result));
    }
    client->GetStreamLatency(&latency);

    const double rate = static_cast<double>(sampleRate);
    status.sampleRate = rate;
    status.channels = kChannels;
    status.bufferDuration = bufferFrames / rate;
    status.streamLatency = static_cast<double>(latency) / 1e7;

    std::vector<float> left(bufferFrames), right(bufferFrames);
    // Start from a buffer of silence, so the first callback already has a clock position.
    BYTE* data = nullptr;
    if (SUCCEEDED(renderClient->GetBuffer(bufferFrames, &data))) {
        renderClient->ReleaseBuffer(bufferFrames, AUDCLNT_BUFFERFLAGS_SILENT);
    }
    uint64_t written = bufferFrames;
    result = client->Start();
    if (FAILED(result)) return fail(describe(result));
    status.isRunning = true;
    status.error.clear();
    setStatus(status);

    bool reopen = true;
    TimelineFit timeline;
    int callbacks = 0;
    double jitter = 0;
    const HANDLE events[] = {ready, static_cast<HANDLE>(wake_)};
    while (!stopping_ && !deviceChanged_) {
        const DWORD signaled = WaitForMultipleObjects(2, events, FALSE, 500);
        if (signaled == WAIT_OBJECT_0 + 1) continue;  // stop or device change; checked above

        UINT32 padding = 0;
        result = client->GetCurrentPadding(&padding);
        if (FAILED(result)) {
            // AUDCLNT_E_DEVICE_INVALIDATED: unplugged or reconfigured; reopen on the new default.
            reopen = result == AUDCLNT_E_DEVICE_INVALIDATED;
            if (!reopen) fail(describe(result));
            break;
        }
        const UINT32 count = bufferFrames - padding;
        if (count == 0) continue;

        // The device position (in `frequency` units) and the performance counter (in 100 ns
        // units) at which it was read. The frames written before this buffer that have not
        // played yet are still ahead of it.
        UINT64 position = 0;
        UINT64 counter = 0;
        const bool hasTime = SUCCEEDED(clock->GetPosition(&position, &counter)) && frequency > 0;
        int64_t hostTime = 0;
        if (hasTime) {
            const double played = static_cast<double>(position) * rate / static_cast<double>(frequency);
            const double measured = static_cast<double>(counter) * 100 + (static_cast<double>(written) - played) / rate * 1e9;
            hostTime = static_cast<int64_t>(timeline.add(static_cast<double>(written), measured, jitter));
            if (++callbacks % 100 == 0) {
                std::lock_guard<std::mutex> lock(mutex_);
                status_.timestampJitter = jitter;
            }
        }
        renderer_.render(hasTime, hostTime, static_cast<int>(count), rate, left.data(), right.data());

        result = renderClient->GetBuffer(count, &data);
        if (FAILED(result)) {
            reopen = result == AUDCLNT_E_DEVICE_INVALIDATED;
            if (!reopen) fail(describe(result));
            break;
        }
        auto* samples = reinterpret_cast<float*>(data);
        for (UINT32 i = 0; i < count; ++i) {
            samples[2 * i] = left[i];
            samples[2 * i + 1] = right[i];
        }
        renderClient->ReleaseBuffer(count, 0);
        written += count;
    }
    client->Stop();
    if (reopen) {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.isRunning = false;
    }
    return reopen;
}

}  // namespace asyncmusic::windows
