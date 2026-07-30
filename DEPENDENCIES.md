# Dependencies

Icy Beauty’s source code and documentation are released under the
[MIT Licence](LICENSE).

## Expert Sleepers distingNT API

- Repository: <https://github.com/expertsleepersltd/distingNT_API>
- Pinned revision: `cd12d876dbe060859828053efab1cbc98c9df251`
- License: MIT (`distingNT_API/LICENSE`)
- Use: compile-time host API for the Expert Sleepers disting NT plug-in

The API is license-compatible with this project. The Git submodule pin makes
the development interface reproducible; updates must be reviewed and pinned
deliberately.

## Physical latency harness

The optional macOS-only `make hardware-latency` target also requires:

- PortAudio 19 (`pkg-config` package `portaudio-2.0`) to open all 12 disting NT
  USB-audio input channels and timestamp input blocks;
- CoreMIDI and CoreFoundation from the macOS SDK to send immediate MIDI packets
  on the same process clock as the capture;
- `clang`, `codesign`, and `open` from macOS to build, ad-hoc sign, and launch
  the foreground capture app with its declared microphone purpose; and
- FFmpeg/FFprobe to retain and verify a 48 kHz, 24-bit PCM mono WAV.

These tools are used only by the local physical-hardware gate. They are not
plug-in runtime dependencies and are not part of hosted CI.
