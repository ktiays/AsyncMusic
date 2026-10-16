# AsyncMusic for Windows

A WinUI 3 app in C++/WinRT on top of the portable core (`../Core`). It joins a group hosted on
a Mac, iPhone or iPad as a speaker, including either side of a stereo pair. Hosting from
Windows comes next.

| Path | What it is |
|---|---|
| `AsyncMusic/` | The app: welcome screen, the speaker screen (nearby hosts, then what this device plays) and diagnostics |
| `Engine/` | Windows-specific parts, plain C++: WASAPI output (`AudioOutput`), mDNS discovery (`LeaderBrowser`), registry settings, and `Engine`, which ties them to Core's `GroupSession` |
| `Cli/` | `asyncmusic-cli`, the same engine without UI, for testing over ssh |
| `Tools/` | Build, test and remote-control scripts (below) |

## Building

Needs Visual Studio 2026 with the C++ desktop workload and the Windows 11 SDK (10.0.26100).
NuGet packages (Windows App SDK 2.5, C++/WinRT, WIL) are restored by the build script.

```bat
build.cmd [Debug|Release] [x64|ARM64]
```

or open `AsyncMusic.slnx` in Visual Studio. The app is unpackaged and self-contained:
`x64\Debug\AsyncMusic\AsyncMusic.exe` runs as is. The project compiles Core and `Engine`
straight from their folders, so new source files there are picked up automatically.

The command line tool and Core's tests build with CMake:

```bat
Tools\build-cli.cmd
build-cli\asyncmusic-cli.exe list
build-cli\asyncmusic-cli.exe join "Some Mac" --seconds 30
Tools\test-core.cmd
```

## Timing on Windows

The host clock is `QueryPerformanceCounter`. `IAudioClock::GetPosition` pairs the device's
play position with the counter, from which each buffer's presentation time follows. Those
readings scatter by 100–200 µs from one callback to the next, so `AudioOutput` stamps buffers
from a line fitted through the last four seconds of readings instead (the scatter is shown in
diagnostics as timestamp jitter). With that, the renderer holds the schedule within a few
tens of microseconds over Wi-Fi.

Devices whose real latency differs from what Windows reports (Bluetooth in particular) can be
corrected with the extra delay slider in diagnostics.

## Developing from a Mac

The scripts assume an ssh host `edy` (override with `ASYNCMUSIC_WINDOWS_HOST`):

- `./sync.sh` copies `Core` and `Windows` to `%USERPROFILE%\src\AsyncMusic`.
- `ssh edy 'src\AsyncMusic\Windows\build.cmd'` builds there.
- ssh sessions have no desktop, so GUI apps start through
  `Tools\run-interactive.ps1 -Command <exe>`, which runs them in the signed-in user's session.
- `Tools\screenshot-remote.cmd` saves a screenshot to `%USERPROFILE%\screen.png`, and
  `Tools\ui-remote.cmd invoke <name>` / `scroll <percent>` / `dump` drive the app through UI
  Automation.
- Without an Apple host around, `Core/tools/test_leader` on the Mac stands in for one
  (register it with `dns-sd -R` to make it discoverable).
