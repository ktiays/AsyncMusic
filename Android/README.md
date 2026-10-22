# AsyncMusic for Android

A Jetpack Compose app in Kotlin on top of the portable core (`../Core`), which it links
through JNI. It hosts a group (plays a file on every device that joins, as a stereo pair if you
like) or joins one hosted on another Android device, a Windows PC, a Mac, an iPhone or an iPad
as a speaker. Android 9 (API 28) or later.

| Path | What it is |
|---|---|
| `app/src/main/cpp/` | The native half: AAudio output (`AudioOutput`), NDK media decoding (`MediaLoader`), `Engine`, which ties them to Core's `GroupSession`, and the JNI bridge (`Jni.cpp`), which hands snapshots over as JSON |
| `app/src/main/java/.../engine/` | The Kotlin half of the engine: mDNS discovery and advertising with `NsdManager` (`LeaderBrowser`, `LeaderAdvertiser`), settings, `GroupController` (the process-wide owner of everything, which also holds the wake and Wi-Fi locks) and `PlaybackService`, the foreground service that keeps a group playing in the background |
| `app/src/main/java/.../ui/` | The screens: welcome, host, speaker (nearby hosts, then what this device plays) and diagnostics |

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
  ALAC. Opening an audio file with AsyncMusic starts hosting with it chosen.
- **Staying alive.** While part of a group the app runs a media playback foreground service
  (with a notification to leave), holds a partial wake lock and a low-latency Wi-Fi lock, which
  keeps the radio out of power save; otherwise clock probes pick up tens of milliseconds of
  jitter.
- **Finding each other.** `NsdManager` browses for and advertises `_asyncmusic._udp`. For
  networks that keep devices from seeing each other, the speaker screen can also join a host
  by address and clock port (shown in the host's diagnostics).

## Testing with the emulator

The emulator sits behind its own NAT, so mDNS does not reach it and it cannot be found by
other devices, but it can still join a host on the Mac or host one:

- **As a speaker:** run `Core/build/test_host some.wav` on the Mac and join by address
  `10.0.2.2` and the port it prints.
- **As a host:** find the app's UDP clock port (diagnostics) and its TCP stream port
  (`adb shell cat /proc/net/tcp6`, the listening socket of the app's uid), forward both with
  `adb emu redir add udp:<port>:<port>` and `tcp:<port>:<port>`, and follow it with
  `Core/build/test_follower 127.0.0.1 <port>`. The emulator answers redirected UDP from
  another port, which a connected socket ignores, so put a small UDP relay in between that
  passes replies back from the port the follower sent to.

Real devices on the same Wi-Fi need none of this.
