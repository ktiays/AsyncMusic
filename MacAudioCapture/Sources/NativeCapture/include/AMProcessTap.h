#import <Foundation/Foundation.h>
#include <TargetConditionals.h>

#if TARGET_OS_OSX

NS_ASSUME_NONNULL_BEGIN

/// Control and read methods belong to one non-real-time serial queue. The HAL callback
/// only touches preallocated C++ storage. No Swift or Objective-C runs in that callback.
@interface AMProcessTap : NSObject
- (instancetype)initWithProcesses:(NSArray<NSNumber*>*)processes bundleIDs:(NSArray<NSString*>*)bundleIDs;
- (BOOL)startAndReturnError:(NSError* _Nullable * _Nullable)error;
- (void)stop;
@property(nonatomic, readonly) double sampleRate;
@property(nonatomic, readonly) uint64_t droppedFrames;
/// Nonzero means the graph must be stopped; old-format data must never reach a renderer.
@property(nonatomic, readonly) NSInteger problem;
- (uint32_t)readFrames:(float*)interleaved capacity:(uint32_t)capacity hostTime:(uint64_t*)hostTime;
@end

/// Snapshot of HAL audio clients, including executable paths for associating helpers with apps.
FOUNDATION_EXPORT NSArray<NSDictionary<NSString*, id>*>* _Nullable AMAudioProcesses(NSError** error);
NS_ASSUME_NONNULL_END
#endif
