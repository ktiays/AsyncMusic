# macOS application audio input

The macOS host can select **Choose Source → App Audio…**, select an audio app, and
press Play. The source app can be silent and need not have initialized audio yet.
AsyncMusic waits for its audio output, then automatically captures that app's
output, mutes its direct output while reading the tap, and feeds the existing local
renderer and ALAC network stream. Stop, Stop Hosting, source changes, sleep, and
capture failures release the tap.

The feature follows the app's existing macOS 15 deployment target. Its native code,
Swift declarations, picker, permission description and sandbox audio-input
entitlement are restricted to macOS. Xcode can still build the package dependency
on other Apple platforms; the package compiles to an empty module there. The
cross-platform protocol and C++ core do not change, so existing speakers can receive
the stream.

## Implementation

- `NativeCapture/AMProcessTap.mm` owns the private Core Audio Process Tap and tap-only
  aggregate device. Its HAL callback uses preallocated C++ SPSC blocks; it calls no
  Swift or Objective-C, takes no locks and performs no file or network I/O.
- `ApplicationAudioCapture` exposes process enumeration and serial-queue capture
  reads. The picker lists running foreground apps through NSWorkspace, independently
  of audio initialization, and merges audio clients/helpers by their containing app
  bundle. Active command-line audio players are also selectable.
- `LiveAudioTimeline` maps capture timestamps to a host-clock frame grid, resampling
  small clock differences and preserving pauses/missing callbacks as silence.
- The app's `ProcessTapStreamFeeder` appends those samples to the existing
  `StreamBuffer`. `SyncedPlayer` renders the host through the same path as followers;
  `AudioStreamServer` forwards that buffer through the existing ALAC protocol.

Frame zero is anchored to the first captured block's host timestamp. Every speaker
plays that timeline **1.5 seconds later** to allow capture, conversion, transmission
and device output latency. This delay also applies on the host; source-app video is
not delayed. The latency is currently fixed, not a user setting.

macOS 26+ uses Bundle IDs and `processRestoreEnabled` to survive a source process
restart. Earlier macOS versions resolve fresh audio object IDs at each start and
stop sharing if those audio processes disappear; start sharing again after the
source resumes. Source selection must be refreshed when an app introduces new
helpers with different Bundle IDs.

Starting sharing for a silent app arms a cancellable wait instead of opening an
inactive HAL graph. Audio clients, including newly created helpers, are resolved
every 200 ms; the graph opens when output becomes active. There is no silence
timeout. The group timeline starts at the first non-silent captured block, so a
long initial wait does not become old audio that must be streamed. The first
instant of output can precede tap startup during this detection interval.
Once started, a paused source can remain silent without restarting the group
timeline. Device/format/service changes
stop sharing with a retry message instead of feeding old-format samples.

`Info-macOS.plist` supplies `NSAudioCaptureUsageDescription`. Both macOS build
configurations retain App Sandbox and Hardened Runtime, with sandbox audio input
enabled. No private TCC functions, driver installation or system output-device
changes are used. System audio recording access must be granted by the user when
macOS requests it.

## Tests

From the repository root:

```sh
swift test --package-path MacAudioCapture
```

Tests cover timestamp anchoring, clock drift, missing callbacks, pause/resume, and
stale timestamps.

Bluetooth/AirPlay switching, physical multi-device acoustic alignment, extended
soak testing, older macOS runtime versions and DRM sources have not been validated.
