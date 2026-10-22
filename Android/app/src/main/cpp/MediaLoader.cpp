//
//  Created by ktiays on 2026/10/22.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#include "MediaLoader.h"

#include <fcntl.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace asyncmusic::android {

namespace {

/// Extractor seeks land on a sync sample at or before the target; seeking this much earlier and
/// decoding forward makes them exact (and gives decoders with priming, like AAC, room).
constexpr int64_t kSeekPreroll = 500'000;  // µs
/// How long one `dequeue` call may wait for the codec.
constexpr int64_t kDequeueTimeout = 10'000;  // µs
/// The codec's PCM encodings (`AudioFormat.ENCODING_*`).
constexpr int32_t kEncoding16Bit = 2;
constexpr int32_t kEncoding8Bit = 3;
constexpr int32_t kEncodingFloat = 4;

class MediaCodecDecoder final : public AudioDecoder {
public:
    /// Opens `path` and decodes up to its first output, which settles the format; null on
    /// failure.
    static std::unique_ptr<MediaCodecDecoder> open(const std::string& path, std::string& error) {
        auto decoder = std::unique_ptr<MediaCodecDecoder>(new MediaCodecDecoder);
        if (!decoder->start(path, error)) return nullptr;
        // The extractor's rate can differ from what the codec puts out (HE-AAC doubles it), so
        // decode until the codec says.
        while (!decoder->hasOutputFormat_ && !decoder->ended_) {
            if (!decoder->decodeNext()) {
                error = "The file's audio could not be decoded";
                return nullptr;
            }
        }
        if (!(decoder->rate_ > 0) || decoder->channels_ <= 0) {
            error = "Unsupported audio format";
            return nullptr;
        }
        decoder->frameCount_ = static_cast<int64_t>(std::llround(static_cast<double>(decoder->durationMicroseconds_) * decoder->rate_ / 1e6));
        return decoder;
    }

    ~MediaCodecDecoder() override {
        if (codec_ != nullptr) {
            AMediaCodec_stop(codec_);
            AMediaCodec_delete(codec_);
        }
        if (extractor_ != nullptr) AMediaExtractor_delete(extractor_);
        if (fd_ >= 0) ::close(fd_);
    }

    double sampleRate() const { return rate_; }
    int64_t frameCount() const { return frameCount_; }

    bool seek(int64_t frame) override {
        const auto target = static_cast<int64_t>(static_cast<double>(frame) * 1e6 / rate_);
        if (AMediaExtractor_seekTo(extractor_, std::max<int64_t>(0, target - kSeekPreroll), AMEDIAEXTRACTOR_SEEK_PREVIOUS_SYNC) != AMEDIA_OK) return false;
        if (AMediaCodec_flush(codec_) != AMEDIA_OK) return false;
        pending_.clear();
        pendingOffset_ = 0;
        skipTo_ = frame;
        inputEnded_ = false;
        ended_ = false;
        return true;
    }

    int read(float* left, float* right, int capacity) override {
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
    MediaCodecDecoder() = default;

    bool start(const std::string& path, std::string& error) {
        fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        struct stat info {};
        if (fd_ < 0 || fstat(fd_, &info) != 0) {
            error = "Could not read the file";
            return false;
        }
        extractor_ = AMediaExtractor_new();
        if (AMediaExtractor_setDataSourceFd(extractor_, fd_, 0, info.st_size) != AMEDIA_OK) {
            error = "Not a playable audio file";
            return false;
        }
        const size_t tracks = AMediaExtractor_getTrackCount(extractor_);
        for (size_t track = 0; track < tracks; ++track) {
            AMediaFormat* format = AMediaExtractor_getTrackFormat(extractor_, track);
            const char* mime = nullptr;
            if (AMediaFormat_getString(format, AMEDIAFORMAT_KEY_MIME, &mime) && mime != nullptr && std::strncmp(mime, "audio/", 6) == 0) {
                const std::string type = mime;
                AMediaFormat_getInt64(format, AMEDIAFORMAT_KEY_DURATION, &durationMicroseconds_);
                // Float output where the codec supports it; others fall back to 16 bits.
                AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_PCM_ENCODING, kEncodingFloat);
                codec_ = AMediaCodec_createDecoderByType(type.c_str());
                const bool configured = codec_ != nullptr && AMediaCodec_configure(codec_, format, nullptr, nullptr, 0) == AMEDIA_OK
                    && AMediaCodec_start(codec_) == AMEDIA_OK;
                AMediaFormat_delete(format);
                if (!configured) {
                    error = "This device has no decoder for the file's audio (" + type + ")";
                    return false;
                }
                AMediaExtractor_selectTrack(extractor_, track);
                return true;
            }
            AMediaFormat_delete(format);
        }
        error = "The file has no audio";
        return false;
    }

    void readOutputFormat() {
        AMediaFormat* format = AMediaCodec_getOutputFormat(codec_);
        if (format == nullptr) return;
        int32_t value = 0;
        if (AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &value)) channels_ = value;
        // The rate is the file's for good once known (a later change would only happen mid-file
        // in odd streams, and the stream's rate cannot change).
        if (!hasOutputFormat_ && AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_SAMPLE_RATE, &value)) rate_ = value;
        encoding_ = AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_PCM_ENCODING, &value) ? value : kEncoding16Bit;
        AMediaFormat_delete(format);
        hasOutputFormat_ = true;
    }

    /// Feeds the codec what it takes and collects one output buffer into `pending_` (stereo,
    /// interleaved); false on error.
    bool decodeNext() {
        pending_.clear();
        pendingOffset_ = 0;
        while (!inputEnded_) {
            const ssize_t index = AMediaCodec_dequeueInputBuffer(codec_, 0);
            if (index < 0) break;
            size_t capacity = 0;
            uint8_t* buffer = AMediaCodec_getInputBuffer(codec_, static_cast<size_t>(index), &capacity);
            const ssize_t size = buffer != nullptr ? AMediaExtractor_readSampleData(extractor_, buffer, capacity) : -1;
            if (size < 0) {
                AMediaCodec_queueInputBuffer(codec_, static_cast<size_t>(index), 0, 0, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
                inputEnded_ = true;
                break;
            }
            const int64_t time = AMediaExtractor_getSampleTime(extractor_);
            AMediaCodec_queueInputBuffer(codec_, static_cast<size_t>(index), 0, static_cast<size_t>(size), static_cast<uint64_t>(std::max<int64_t>(time, 0)), 0);
            AMediaExtractor_advance(extractor_);
        }

        AMediaCodecBufferInfo info{};
        const ssize_t index = AMediaCodec_dequeueOutputBuffer(codec_, &info, kDequeueTimeout);
        if (index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            readOutputFormat();
            return true;
        }
        if (index == AMEDIACODEC_INFO_TRY_AGAIN_LATER || index == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) return true;
        if (index < 0) return false;
        if (!hasOutputFormat_) readOutputFormat();
        if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) ended_ = true;
        size_t capacity = 0;
        const uint8_t* data = AMediaCodec_getOutputBuffer(codec_, static_cast<size_t>(index), &capacity);
        if (data != nullptr && info.size > 0 && channels_ > 0) append(data + info.offset, static_cast<size_t>(info.size), info.presentationTimeUs);
        AMediaCodec_releaseOutputBuffer(codec_, static_cast<size_t>(index), false);
        return true;
    }

    float sample(const uint8_t* data, int64_t index) const {
        switch (encoding_) {
        case kEncodingFloat: {
            float value;
            std::memcpy(&value, data + index * 4, 4);
            return value;
        }
        case kEncoding8Bit: return (static_cast<float>(data[index]) - 128.0f) / 128.0f;
        default: {
            int16_t value;
            std::memcpy(&value, data + index * 2, 2);
            return static_cast<float>(value) / 32768.0f;
        }
        }
    }

    void append(const uint8_t* data, size_t size, int64_t timestamp) {
        const int bytes = encoding_ == kEncodingFloat ? 4 : encoding_ == kEncoding8Bit ? 1 : 2;
        const int64_t frames = static_cast<int64_t>(size) / bytes / channels_;
        int64_t first = 0;
        if (skipTo_ >= 0) {
            // Drop what the seek decoded before the target; pad if it landed after it.
            const int64_t start = std::llround(static_cast<double>(timestamp) * rate_ / 1e6);
            const int64_t padding = std::clamp<int64_t>(start - skipTo_, 0, static_cast<int64_t>(rate_));
            first = std::max<int64_t>(0, skipTo_ - start);
            if (first >= frames) return;
            pending_.assign(static_cast<size_t>(2 * padding), 0.0f);
            skipTo_ = -1;
        }
        pending_.reserve(pending_.size() + static_cast<size_t>(2 * (frames - first)));
        for (int64_t i = first; i < frames; ++i) {
            const float l = sample(data, i * channels_);
            pending_.push_back(l);
            pending_.push_back(channels_ > 1 ? sample(data, i * channels_ + 1) : l);
        }
    }

    int fd_ = -1;
    AMediaExtractor* extractor_ = nullptr;
    AMediaCodec* codec_ = nullptr;
    int64_t durationMicroseconds_ = 0;
    bool hasOutputFormat_ = false;
    double rate_ = 0;
    int channels_ = 0;
    int32_t encoding_ = kEncoding16Bit;
    int64_t frameCount_ = 0;
    std::vector<float> pending_;
    size_t pendingOffset_ = 0;
    /// After a seek, the frame the next output starts at (-1: none pending).
    int64_t skipTo_ = 0;
    bool inputEnded_ = false;
    bool ended_ = false;
};

}  // namespace

std::shared_ptr<MediaFile> loadMediaFile(const std::string& path, const std::string& name, const uint8_t digest[32], std::string& error) {
    auto decoder = MediaCodecDecoder::open(path, error);
    if (!decoder) return nullptr;
    auto file = std::make_shared<MediaFile>();
    file->id = mediaFileID(digest);
    file->name = name;
    file->sampleRate = decoder->sampleRate();
    file->frameCount = decoder->frameCount();
    // Each stream gets its own decoder; the feeder seeks it to where playback is.
    file->openDecoder = [path]() -> std::unique_ptr<AudioDecoder> {
        std::string ignored;
        return MediaCodecDecoder::open(path, ignored);
    };
    return file;
}

}  // namespace asyncmusic::android
