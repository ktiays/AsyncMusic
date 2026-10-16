# AsyncMusic Core

The platform-independent part of AsyncMusic's synchronized playback, in portable C++17 with no
dependencies. The Apple app uses it through Swift's C++ interoperability; other platforms link
it directly.

| Header | What it does |
|---|---|
| `clock.hpp`, `clock_estimator.hpp` | NTP-style probe math and the min-RTT-filtered, RTT-weighted fit of offset and skew to the leader's clock |
| `playback.hpp` | Playback state on the leader's timeline; `PlaybackPosition` (heard media time, thread-safe) |
| `stream_buffer.hpp` | Lock-free single-writer ring of stereo frames addressed by absolute frame number |
| `renderer.hpp` | Per-callback scheduling onto the leader's timeline, PI rate correction, repositioning, gap fades, channel mix switched at a scheduled leader time, metronome and left/right test signal |
| `speakers.hpp` | Speaker layouts (everywhere, stereo pair; room for 5.1/7.1), the channel each device plays, and the matrix that routes it |
| `protocol.hpp` | Wire formats of the clock sync datagrams and the audio stream frames, PCM16 helpers |
| `host_clock.hpp` | The host clock and sleep detection per platform |

## What each platform provides

- **Audio output**: a callback that calls `SyncedRenderer::render` with the host time at which
  the buffer's first frame is presented, minus whatever is set with `setPresentationDelay`.
  On Apple that is `mHostTime` plus the reported latency; on Android (AAudio) and Windows
  (WASAPI `IAudioClock`) derive it from the frame-position/time pairs those APIs report.
- **The same clock**: those timestamps must be on the clock `hostClockNanoseconds()` reads
  (see `host_clock.hpp` for which one per platform).
- **Networking**: UDP for `ClockSyncMessage`, TCP for `AudioStreamMessage` frames, and mDNS to
  discover leaders (service `_asyncmusic._udp`). The probe scheduling, stream sending,
  adaptive start and speaker assignment (which device plays which channel, with fallback to
  the full mix while a channel's device is missing) live in the Apple app (`AsyncMusic/ClockSync`, `AsyncMusic/Streaming`) as a
  reference for porting.
- **Codecs**: the stream carries PCM16 or ALAC (`AudioStreamCodec`); encoding and decoding
  ALAC is left to the platform.

## Building and testing

```sh
cmake -S . -B build -G Ninja
cmake --build build
./build/asyncmusic_core_tests
```

The tests cover clock estimation under simulated Wi-Fi jitter, start timing and drift tracking,
channel routing and its scheduled switch,
bit-exact playback through the ring (including wrap-around and late joins), concurrent ring
access, gap fades, and the wire format against bytes captured from the original Swift
implementation, so devices on either implementation interoperate.

## Real-time rules

`SyncedRenderer::render` and `StreamBuffer` reads never block, allocate or touch reference
counts. The renderer does not retain the `StreamBuffer` it is given: keep it alive until a later
`setPlayback` has had time to reach the render thread (the Apple player releases replaced
buffers two seconds later).
