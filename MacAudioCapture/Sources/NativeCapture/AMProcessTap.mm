#import "AMProcessTap.h"
#if TARGET_OS_OSX
#import <CoreAudio/CoreAudio.h>
#import <CoreAudio/CATapDescription.h>
#import <CoreAudio/AudioHardwareTapping.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <libproc.h>
#include <mach/mach_time.h>
#include <memory>
#include <vector>

namespace {
AudioObjectPropertyAddress address(AudioObjectPropertySelector selector) {
    return {selector, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
}
NSError* audioError(OSStatus status, NSString* operation) {
    return [NSError errorWithDomain:NSOSStatusErrorDomain code:status userInfo:@{
        NSLocalizedDescriptionKey: [NSString stringWithFormat:@"%@: Core Audio error %d.", operation, int(status)]
    }];
}
template<typename T> OSStatus get(AudioObjectID object, AudioObjectPropertySelector selector, T& value) {
    UInt32 size = sizeof(value);
    auto a = address(selector);
    return AudioObjectGetPropertyData(object, &a, 0, nullptr, &size, &value);
}
NSString* stringProperty(AudioObjectID object, AudioObjectPropertySelector selector) {
    CFStringRef value = nullptr;
    if (get(object, selector, value) != noErr) return @"";
    return CFBridgingRelease(value) ?: @"";
}
constexpr uint32_t blockFrames = 4096;
constexpr uint64_t blockCount = 128;
struct Block { uint64_t hostTime = 0; uint32_t frames = 0; std::array<float, blockFrames * 2> samples{}; };
struct TapState {
    std::array<Block, blockCount> blocks{};
    alignas(64) std::atomic<uint64_t> write{0};
    alignas(64) std::atomic<uint64_t> read{0};
    std::atomic<uint64_t> dropped{0};
    std::atomic<int> problem{0};
    double rate = 0;
    mach_timebase_info_data_t timebase{};
    TapState() { mach_timebase_info(&timebase); }
};
static_assert(std::atomic<uint64_t>::is_always_lock_free);

OSStatus capture(AudioObjectID, const AudioTimeStamp*, const AudioBufferList* input,
                 const AudioTimeStamp* stamp, AudioBufferList*, const AudioTimeStamp*, void* context) {
    auto& state = *static_cast<TapState*>(context);
    if (!input || !input->mNumberBuffers) return noErr;
    const bool planar = input->mNumberBuffers == 2 && input->mBuffers[0].mNumberChannels == 1 && input->mBuffers[1].mNumberChannels == 1;
    const bool interleaved = input->mNumberBuffers == 1 && input->mBuffers[0].mNumberChannels == 2;
    if ((!planar && !interleaved) || !input->mBuffers[0].mData || (planar && !input->mBuffers[1].mData)) {
        state.problem.store(1, std::memory_order_relaxed); return noErr;
    }
    if (!(stamp->mFlags & kAudioTimeStampHostTimeValid)) {
        state.problem.store(2, std::memory_order_relaxed); return noErr;
    }
    uint32_t frames = input->mBuffers[0].mDataByteSize / (sizeof(float) * (planar ? 1 : 2));
    if (planar) frames = std::min<uint32_t>(frames, input->mBuffers[1].mDataByteSize / sizeof(float));
    const auto* left = static_cast<const float*>(input->mBuffers[0].mData);
    const auto* right = planar ? static_cast<const float*>(input->mBuffers[1].mData) : nullptr;
    for (uint32_t offset = 0; offset < frames; offset += blockFrames) {
        const uint32_t count = std::min(blockFrames, frames - offset);
        const auto w = state.write.load(std::memory_order_relaxed);
        if (w - state.read.load(std::memory_order_acquire) == blockCount) {
            state.dropped.fetch_add(frames - offset, std::memory_order_relaxed);
            state.problem.store(3, std::memory_order_relaxed); break;
        }
        auto& block = state.blocks[w % blockCount];
        block.frames = count;
        const auto offsetTicks = uint64_t(double(offset) / state.rate * 1e9 * state.timebase.denom / state.timebase.numer);
        block.hostTime = stamp->mHostTime + offsetTicks;
        if (planar) {
            for (uint32_t i = 0; i < count; ++i) {
                block.samples[2*i] = left[offset+i]; block.samples[2*i+1] = right[offset+i];
            }
        } else std::memcpy(block.samples.data(), left + 2*offset, count*2*sizeof(float));
        state.write.store(w + 1, std::memory_order_release);
    }
    return noErr;
}
OSStatus changed(AudioObjectID, UInt32, const AudioObjectPropertyAddress[], void* context) {
    static_cast<TapState*>(context)->problem.store(4, std::memory_order_relaxed);
    return noErr;
}
}

@implementation AMProcessTap {
    NSArray<NSNumber*>* _processes;
    NSArray<NSString*>* _bundleIDs;
    std::unique_ptr<TapState> _state;
    AudioObjectID _tap;
    AudioObjectID _aggregate;
    AudioDeviceIOProcID _io;
    BOOL _started;
    std::vector<std::pair<AudioObjectID, AudioObjectPropertySelector>> _listeners;
}
- (instancetype)initWithProcesses:(NSArray<NSNumber*>*)processes bundleIDs:(NSArray<NSString*>*)bundleIDs {
    if ((self = [super init])) {
        _processes = [processes copy]; _bundleIDs = [bundleIDs copy];
        _state = std::make_unique<TapState>();
    }
    return self;
}
- (BOOL)startAndReturnError:(NSError**)error {
    auto fail = [&](OSStatus status, NSString* operation) {
        if (error) *error = audioError(status, operation);
        [self stop];
        return NO;
    };
    if (_tap || _aggregate) return fail(kAudioHardwareIllegalOperationError, @"Capture already started");
    CATapDescription* description = [[CATapDescription alloc] initStereoMixdownOfProcesses:_processes];
    description.name = @"AsyncMusic App Audio";
    description.privateTap = YES;
    description.muteBehavior = CATapMutedWhenTapped;
    if (@available(macOS 26.0, *)) {
        description.processRestoreEnabled = YES;
        if (_bundleIDs.count) {
            // Bundle selection remains valid if a process exited after the picker snapshot.
            description.processes = @[];
            description.bundleIDs = _bundleIDs;
        }
    }
    OSStatus status = AudioHardwareCreateProcessTap(description, &_tap);
    if (status) return fail(status, @"Cannot capture this app");
    AudioStreamBasicDescription format{};
    status = get(_tap, kAudioTapPropertyFormat, format);
    if (status) return fail(status, @"Cannot read app audio format");
    const UInt32 bytesPerFrame = (format.mFormatFlags & kAudioFormatFlagIsNonInterleaved) ? 4 : 8;
    if (format.mFormatID != kAudioFormatLinearPCM || !(format.mFormatFlags & kAudioFormatFlagIsFloat) ||
        (format.mFormatFlags & kAudioFormatFlagIsBigEndian) || format.mBitsPerChannel != 32 ||
        format.mChannelsPerFrame != 2 || format.mBytesPerFrame != bytesPerFrame || format.mSampleRate <= 0)
        return fail(kAudioHardwareUnsupportedOperationError, @"Unsupported app audio format");
    _state->rate = format.mSampleRate;
    NSString* uid = stringProperty(_tap, kAudioTapPropertyUID);
    NSDictionary* device = @{
        @kAudioAggregateDeviceNameKey: @"AsyncMusic App Audio Input",
        @kAudioAggregateDeviceUIDKey: NSUUID.UUID.UUIDString,
        @kAudioAggregateDeviceIsPrivateKey: @YES,
        @kAudioAggregateDeviceTapAutoStartKey: @NO,
        @kAudioAggregateDeviceTapListKey: @[@{@kAudioSubTapUIDKey: uid, @kAudioSubTapDriftCompensationKey: @YES}],
    };
    status = AudioHardwareCreateAggregateDevice((__bridge CFDictionaryRef)device, &_aggregate);
    if (status) return fail(status, @"Cannot open app audio input");
    status = AudioDeviceCreateIOProcID(_aggregate, capture, _state.get(), &_io);
    if (status) return fail(status, @"Cannot create app audio callback");
    for (auto [object, selector] : std::vector<std::pair<AudioObjectID, AudioObjectPropertySelector>>{
        {kAudioObjectSystemObject, kAudioHardwarePropertyDefaultOutputDevice},
        {kAudioObjectSystemObject, kAudioHardwarePropertyServiceRestarted},
        {_tap, kAudioTapPropertyFormat}}) {
        auto a = address(selector);
        status = AudioObjectAddPropertyListener(object, &a, changed, _state.get());
        if (status) return fail(status, @"Cannot observe audio changes");
        _listeners.emplace_back(object, selector);
    }
    status = AudioDeviceStart(_aggregate, _io);
    if (status) return fail(status, @"Cannot start app audio. Play audio in the app and try again");
    _started = YES;
    return YES;
}
- (void)stop {
    for (auto [object, selector] : _listeners) {
        auto a = address(selector);
        AudioObjectRemovePropertyListener(object, &a, changed, _state.get());
    }
    _listeners.clear();
    if (_started) { AudioDeviceStop(_aggregate, _io); _started = NO; }
    if (_io) { AudioDeviceDestroyIOProcID(_aggregate, _io); _io = nullptr; }
    if (_aggregate) { AudioHardwareDestroyAggregateDevice(_aggregate); _aggregate = 0; }
    if (_tap) { AudioHardwareDestroyProcessTap(_tap); _tap = 0; }
}
- (double)sampleRate { return _state->rate; }
- (uint64_t)droppedFrames { return _state->dropped.load(std::memory_order_relaxed); }
- (NSInteger)problem { return _state->problem.load(std::memory_order_relaxed); }
- (uint32_t)readFrames:(float*)destination capacity:(uint32_t)capacity hostTime:(uint64_t*)hostTime {
    const auto r = _state->read.load(std::memory_order_relaxed);
    if (r == _state->write.load(std::memory_order_acquire)) return 0;
    const auto& block = _state->blocks[r % blockCount];
    if (capacity < block.frames) return 0;
    std::memcpy(destination, block.samples.data(), block.frames*2*sizeof(float));
    *hostTime = block.hostTime;
    const uint32_t frames = block.frames;
    _state->read.store(r + 1, std::memory_order_release);
    return frames;
}
- (void)dealloc { [self stop]; }
@end

NSArray<NSDictionary<NSString*, id>*>* AMAudioProcesses(NSError** error) {
    auto a = address(kAudioHardwarePropertyProcessObjectList);
    UInt32 size = 0;
    OSStatus status = AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, nullptr, &size);
    if (status) { if (error) *error = audioError(status, @"Cannot list audio apps"); return nil; }
    std::vector<AudioObjectID> objects(size / sizeof(AudioObjectID));
    if (size) status = AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, nullptr, &size, objects.data());
    if (status) { if (error) *error = audioError(status, @"Cannot list audio apps"); return nil; }
    NSMutableArray* result = [NSMutableArray array];
    for (size_t i = 0; i < size / sizeof(AudioObjectID); ++i) {
        const auto object = objects[i];
        pid_t pid = 0; UInt32 running = 0;
        if (get(object, kAudioProcessPropertyPID, pid) != noErr || pid == getpid()) continue;
        if (get(object, kAudioProcessPropertyIsRunningOutput, running) != noErr) continue;
        char path[PROC_PIDPATHINFO_MAXSIZE]{};
        proc_pidpath(pid, path, sizeof(path));
        [result addObject:@{@"objectID": @(object), @"pid": @(pid), @"running": @(running != 0),
                            @"bundleID": stringProperty(object, kAudioProcessPropertyBundleID),
                            @"path": @(path)}];
    }
    return result;
}
#endif
