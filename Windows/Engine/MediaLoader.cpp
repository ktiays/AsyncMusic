//
//  Created by ktiays on 2026/10/16.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#include "MediaLoader.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <bcrypt.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <vector>

#include "Text.h"

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

using Microsoft::WRL::ComPtr;

namespace asyncmusic::windows {

namespace {

constexpr DWORD kAudioStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM);
/// Media Foundation's seeks land on a packet boundary at or before the target; seeking this
/// much earlier and decoding forward makes them exact.
constexpr int64_t kSeekPreroll = 5'000'000;  // 100 ns units

/// Media Foundation readers are free-threaded, but whichever thread uses one needs COM.
void ensureCOM() {
    thread_local const bool initialized = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    (void)initialized;
}

std::string describe(HRESULT result) {
    char code[16];
    snprintf(code, sizeof(code), "0x%08lX", static_cast<unsigned long>(result));
    return code;
}

/// The SHA-256 of a file's contents.
bool hashFile(const std::wstring& path, uint8_t digest[32]) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 && BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) == 0;
    std::vector<UCHAR> chunk(1 << 20);
    DWORD read = 0;
    while (ok && ReadFile(file, chunk.data(), static_cast<DWORD>(chunk.size()), &read, nullptr) && read > 0) {
        ok = BCryptHashData(hash, chunk.data(), read, 0) == 0;
    }
    ok = ok && BCryptFinishHash(hash, digest, 32, 0) == 0;
    if (hash != nullptr) BCryptDestroyHash(hash);
    if (algorithm != nullptr) BCryptCloseAlgorithmProvider(algorithm, 0);
    CloseHandle(file);
    return ok;
}

class MediaFoundationDecoder final : public AudioDecoder {
public:
    /// Opens `path` and reads its format; null on failure.
    static std::unique_ptr<MediaFoundationDecoder> open(const std::wstring& path, std::string& error) {
        ensureCOM();
        ComPtr<IMFSourceReader> reader;
        HRESULT result = MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader);
        if (FAILED(result)) {
            error = "Not a playable audio file (" + describe(result) + ")";
            return nullptr;
        }
        reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
        reader->SetStreamSelection(kAudioStream, TRUE);
        // Float at the file's own rate and channel count; the renderer resamples.
        ComPtr<IMFMediaType> type;
        MFCreateMediaType(&type);
        type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
        result = reader->SetCurrentMediaType(kAudioStream, nullptr, type.Get());
        ComPtr<IMFMediaType> current;
        if (FAILED(result) || FAILED(result = reader->GetCurrentMediaType(kAudioStream, &current))) {
            error = "The file has no audio Windows can decode (" + describe(result) + ")";
            return nullptr;
        }
        auto decoder = std::unique_ptr<MediaFoundationDecoder>(new MediaFoundationDecoder);
        decoder->reader_ = reader;
        decoder->channels_ = static_cast<int>(MFGetAttributeUINT32(current.Get(), MF_MT_AUDIO_NUM_CHANNELS, 0));
        decoder->rate_ = static_cast<double>(MFGetAttributeUINT32(current.Get(), MF_MT_AUDIO_SAMPLES_PER_SECOND, 0));
        if (decoder->channels_ <= 0 || !(decoder->rate_ > 0)) {
            error = "Unsupported audio format";
            return nullptr;
        }
        PROPVARIANT duration;
        PropVariantInit(&duration);
        if (SUCCEEDED(reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &duration)) && duration.vt == VT_UI8) {
            decoder->frameCount_ = static_cast<int64_t>(std::llround(static_cast<double>(duration.uhVal.QuadPart) * decoder->rate_ / 1e7));
        }
        PropVariantClear(&duration);
        return decoder;
    }

    double sampleRate() const { return rate_; }
    int64_t frameCount() const { return frameCount_; }

    bool seek(int64_t frame) override {
        ensureCOM();
        PROPVARIANT position;
        PropVariantInit(&position);
        position.vt = VT_I8;
        position.hVal.QuadPart = std::max<int64_t>(0, static_cast<int64_t>(static_cast<double>(frame) * 1e7 / rate_) - kSeekPreroll);
        if (FAILED(reader_->SetCurrentPosition(GUID_NULL, position))) return false;
        pending_.clear();
        pendingOffset_ = 0;
        skipTo_ = frame;
        ended_ = false;
        return true;
    }

    int read(float* left, float* right, int capacity) override {
        ensureCOM();
        int produced = 0;
        while (produced < capacity) {
            const size_t available = (pending_.size() - pendingOffset_) / 2;
            if (available > 0) {
                const int count = static_cast<int>(std::min<size_t>(available, static_cast<size_t>(capacity - produced)));
                for (int i = 0; i < count; ++i) {
                    left[produced + i] = pending_[pendingOffset_ + 2 * static_cast<size_t>(i)];
                    right[produced + i] = pending_[pendingOffset_ + 2 * static_cast<size_t>(i) + 1];
                }
                pendingOffset_ += 2 * static_cast<size_t>(count);
                produced += count;
                continue;
            }
            if (ended_) break;
            if (!decodeNext()) return produced > 0 ? produced : -1;
        }
        return produced;
    }

private:
    MediaFoundationDecoder() = default;

    /// Decodes the next packet into `pending_` (stereo, interleaved); false on error.
    bool decodeNext() {
        pending_.clear();
        pendingOffset_ = 0;
        DWORD flags = 0;
        LONGLONG timestamp = 0;
        ComPtr<IMFSample> sample;
        if (FAILED(reader_->ReadSample(kAudioStream, 0, nullptr, &flags, &timestamp, &sample))) return false;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) ended_ = true;
        if (!sample) return true;
        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) return false;
        BYTE* data = nullptr;
        DWORD length = 0;
        if (FAILED(buffer->Lock(&data, nullptr, &length))) return false;
        const auto* samples = reinterpret_cast<const float*>(data);
        int64_t frames = static_cast<int64_t>(length / sizeof(float)) / channels_;
        int64_t first = 0;
        if (skipTo_ >= 0) {
            // Drop what the seek decoded before the target; pad if it landed after it.
            const int64_t start = std::llround(static_cast<double>(timestamp) * rate_ / 1e7);
            const int64_t padding = std::clamp<int64_t>(start - skipTo_, 0, static_cast<int64_t>(rate_));
            first = std::max<int64_t>(0, skipTo_ - start);
            if (first < frames) {
                pending_.assign(static_cast<size_t>(2 * padding), 0.0f);
                skipTo_ = -1;
            }
        }
        for (int64_t i = first; i < frames && skipTo_ < 0; ++i) {
            const float l = samples[i * channels_];
            pending_.push_back(l);
            pending_.push_back(channels_ > 1 ? samples[i * channels_ + 1] : l);
        }
        buffer->Unlock();
        return true;
    }

    ComPtr<IMFSourceReader> reader_;
    double rate_ = 0;
    int channels_ = 0;
    int64_t frameCount_ = 0;
    std::vector<float> pending_;
    size_t pendingOffset_ = 0;
    /// After a seek, the frame the next output starts at (-1: none pending).
    int64_t skipTo_ = 0;
    bool ended_ = false;
};

}  // namespace

MediaFoundationScope::MediaFoundationScope() {
    started_ = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
}

MediaFoundationScope::~MediaFoundationScope() {
    if (started_) MFShutdown();
}

std::shared_ptr<MediaFile> loadMediaFile(const std::wstring& path, std::string& error) {
    auto decoder = MediaFoundationDecoder::open(path, error);
    if (!decoder) return nullptr;
    uint8_t digest[32];
    if (!hashFile(path, digest)) {
        error = "Could not read the file";
        return nullptr;
    }
    auto file = std::make_shared<MediaFile>();
    file->id = mediaFileID(digest);
    file->name = toUTF8(std::filesystem::path(path).stem().wstring());
    file->sampleRate = decoder->sampleRate();
    file->frameCount = decoder->frameCount();
    // Each stream gets its own decoder; the feeder seeks it to where playback is.
    file->openDecoder = [path]() -> std::unique_ptr<AudioDecoder> {
        std::string ignored;
        return MediaFoundationDecoder::open(path, ignored);
    };
    return file;
}

}  // namespace asyncmusic::windows
