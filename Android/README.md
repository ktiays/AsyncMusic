# AsyncMusic for Android

A Jetpack Compose app in Kotlin (Material 3 Expressive) on top of the portable core
(`../Core`), which it links through JNI. Like the other apps it either **listens** — hears
shares announced on the network, alerts you to new ones and joins one, playing the left or right
side of a stereo pair if you like — or **shares** an audio file with every device that joins.
Android 9 (API 28) or later.

| Path | What it is |
|---|---|
| `app/src/main/cpp/` | The native half: AAudio output (`AudioOutput`, with the output volume and "pause here"), NDK media decoding (`MediaLoader`), `Engine`, which ties them to Core's `GroupSession` and `ShareCenter` (the Listen/Share model, with share discovery by UDP broadcast), and the JNI bridge (`Jni.cpp`), which hands state over as JSON |
| `app/src/main/java/.../engine/` | The Kotlin half of the engine: settings, `GroupController` (the process-wide owner of everything, which also holds the wake, Wi-Fi and multicast locks), `PlaybackService`, the foreground service that keeps listening or sharing going in the background (and takes a share notification's Join), and `ShareNotifications` |
| `app/src/main/java/.../ui/` | The screens: Listen (a radar of shares nearby, or the share joined), Share (choosing a file, then the share under way), the mode in a floating toolbar, share alerts, Settings (name, alerts, join by address) and Diagnostics |

## Building

Needs the Android SDK with platform 36, NDK 30 and CMake 3.22.1 (Android Studio installs them),
and JDK 17 or later. The Gradle build compiles Core from its folder with CMake.

```sh
./gradlew :app:assembleDebug
./gradlew :app:installDebug
```

or open this folder in Android Studio.

## How it fits Android

- **Timing.** The host clock is `CLOCK_MONOTONIC`, the clock `AAudioStream_getTimestamp`
  reports presentation times on. That call is not meant for the real-time callback, so a
  control thread reads it every 50 ms, fits a line through the last four seconds of readings
  (frame position against time) and publishes it to the callback, which stamps each buffer
  with when its first frame is heard. The scatter of the raw readings around the line is
  shown in diagnostics as timestamp jitter. Output latency is whatever the timestamps say, so
  Bluetooth is covered as far as the system reports it; the extra delay slider in diagnostics
  corrects the rest.
- **Files** are copied into the app's storage when chosen (hashing them on the way, for the ID
  every device agrees on), then read with `AMediaExtractor` and `AMediaCodec`: MP3, AAC, FLAC,
  Opus, Vorbis, WAV and whatever else the device decodes. They are streamed to followers as
  ALAC. Opening an audio file with AsyncMusic switches to Share with it chosen.
- **Staying alive.** While listening to a share or sharing, the app runs a media playback
  foreground service (with a notification to leave or stop), holds a partial wake lock and a
  low-latency Wi-Fi lock, which keeps the radio out of power save; otherwise clock probes pick up
  tens of milliseconds of jitter.
- **Finding each other.** Core's `ShareDiscovery` broadcasts and hears share announcements on
  UDP 41750; the app holds a multicast lock while it listens, since many devices filter
  broadcasts otherwise. A new share raises an alert in the app, or a notification with Join
  while the app is in the background. Android 15+ cuts a backgrounded app off the network unless
  it runs a foreground service, so in the background new shares are only heard while listening
  or sharing. For networks that keep devices apart, Settings can join a share by address and
  clock port.

## Testing with the emulator

The emulator sits behind its own NAT, so broadcasts do not reach it and it cannot be found by
other devices, but it can still listen to a share on the Mac or share one:

- **As a listener:** run `Core/build/test_host some.wav` on the Mac and join by address
  `10.0.2.2:<the port it prints>` in Settings. To see it as a share instead, forward
  announcements: `adb emu redir add udp:41751:41750`, relay UDP 41750 on the Mac to
  `127.0.0.1:41751` (a socket bound with `SO_REUSEPORT` hears the broadcasts), and run
  `Core/build/share_beacon share "Name" --clock-port <that port>`; joining it then follows the
  test host at `10.0.2.2`.
- **Sharing:** find the app's UDP clock port (diagnostics) and its TCP stream port
  (`adb shell cat /proc/net/tcp6`, the listening socket of the app's uid), forward both with
  `adb emu redir add udp:<port>:<port>` and `tcp:<port>:<port>`, and follow it with
  `Core/build/test_follower 127.0.0.1 <port>`. The emulator answers redirected UDP from
  another port, which a connected socket ignores, so put a small UDP relay in between that
  passes replies back from the port the follower sent to.

Real devices on the same Wi-Fi need none of this.
