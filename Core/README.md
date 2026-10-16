# AsyncMusic Core

The platform-independent part of AsyncMusic's synchronized playback, in portable C++17 with no
dependencies beyond the vendored ALAC codec. The Apple app uses it through Swift's C++
interoperability; other platforms (Windows so far, see `../Windows`) link it directly.

| Header | What it does |
|---|---|
| `clock.hpp`, `clock_estimator.hpp` | NTP-style probe math and the min-RTT-filtered, RTT-weighted fit of offset and skew to the leader's clock |
| `playback.hpp` | Playback state on the leader's timeline; `PlaybackPosition` (heard media time, thread-safe) |
| `stream_buffer.hpp` | Lock-free single-writer ring of stereo frames addressed by absolute frame number |
| `renderer.hpp` | Per-callback scheduling onto the leader's timeline, PI rate correction, repositioning, gap fades, channel mix switched at a scheduled leader time, metronome and left/right test signal |
| `speakers.hpp` | Speaker layouts (everywhere, stereo pair; room for 5.1/7.1), the channel each device plays, and the matrix that routes it |
| `protocol.hpp` | Wire formats of the clock sync datagrams and the audio stream frames, PCM16 helpers |
| `alac.hpp` | Apple Lossless encoder and decoder for stream chunks (Apple's reference codec in `third_party/alac`), interoperable with AudioConverter both ways |
| `host_clock.hpp` | The host clock and sleep detection per platform |
| `net.hpp`, `serial_queue.hpp` | Blocking BSD/Winsock sockets with timeouts, and a serial task queue with delays (the stand-in for dispatch queues) |
| `clock_sync.hpp` | `ClockSyncFollower`: probe rounds against a leader, hello, playback/speaker/audio-port messages, reconnects. `ClockSyncLeader`: answers probes on a dual-stack UDP socket and keeps followers told what to play and which channel |
| `audio_stream.hpp` | `AudioStreamClient`: receives a leader's stream over TCP into a `StreamBuffer`, decoding ALAC or PCM16, and reports its buffer. `AudioStreamServer`: forwards the leader's buffer to every follower and tells when all have enough (adaptive start) |
| `media.hpp` | `AudioDecoder` (implemented by the platform) and `MediaFile`, and `FileStreamFeeder`, which decodes a file a few seconds ahead of playback |
| `player.hpp` | `Player`: keeps a renderer fed with the clock map, playback state, channel and what to play: a local file stream or the leader's (the engine-independent part of the Apple app's `SyncedPlayer`) |
| `session.hpp` | `GroupSession`: leads (play/stop, adaptive start, speaker layouts and channel assignment) or follows, and keeps the state, round history and log a UI shows (the counterpart of the Apple app's `ClockSyncSession`) |

The headers from `alac.hpp` down are not part of the Swift module (`asyncmusic.hpp`); the Apple
app keeps its Swift versions of those components for now.

## What each platform provides

- **Audio output**: a callback that calls `SyncedRenderer::render` with the host time at which
  the buffer's first frame is presented, minus whatever is set with `setPresentationDelay`.
  On Apple that is `mHostTime` plus the reported latency; on Android (AAudio) and Windows
  (WASAPI `IAudioClock`) derive it from the frame-position/time pairs those APIs report.
- **The same clock**: those timestamps must be on the clock `hostClockNanoseconds()` reads
  (see `host_clock.hpp` for which one per platform).
- **mDNS** for the service `_asyncmusic._udp` (`kClockSyncServiceType`): finding leaders
  (`GroupSession::follow` takes a resolver that looks the leader up again on every reconnect),
  and advertising `Snapshot::leaderPort` under the device's name while leading.
- **Decoding files**: an `AudioDecoder` per `MediaFile` that seeks sample-accurately, and the
  file's ID from the SHA-256 of its contents (`mediaFileID`).
- **Identity and settings**: a stable device ID and a display name (`DeviceIdentity`), and
  somewhere to keep the leader's speaker settings (`onSpeakerSettingsChange`).

Everything else, leading and following, is in the core: sockets, probe scheduling, the audio
stream with ALAC, adaptive start and speaker assignment. The Apple app keeps its own Swift
implementation of the same (`AsyncMusic/ClockSync`, `AsyncMusic/Streaming`).

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
implementation, so devices on either implementation interoperate. ALAC is checked against
packets from Apple's encoder (`tests/fixtures`, regenerated with the Swift script there). Over
loopback, the follower and stream client run against a fake leader, the core's leader and
stream server against the core's follower and client, and a leading and a following
`GroupSession` play a file with adaptive start and a stereo pair end to end.

Two tools are built with the tests, for trying devices against each other without full apps:
`tools/test_leader.cpp`, a bare-bones leader that plays the metronome or the channel test
(optionally as one side of a stereo pair), and `tools/test_host.cpp`, which hosts a WAV file
with `GroupSession` (an Apple device following it plays the ALAC stream).

## Real-time rules

`SyncedRenderer::render` and `StreamBuffer` reads never block, allocate or touch reference
counts. The renderer does not retain the `StreamBuffer` it is given: keep it alive until a later
`setPlayback` has had time to reach the render thread (the Apple player releases replaced
buffers two seconds later).
