# FastPlugs

FastRoute's built-in effects as CLAP, VST3 and Audio Unit plugins, for Windows
and macOS.

These are the same effects that run inside FastRoute. Nothing has been
rewritten. The build takes FastRoute's own effect sources and compiles them
unchanged. Fix something in FastRoute and the plugins get the fix the next
time they are built.

## The plugins

There is one bundle per format, and each bundle holds all thirteen plugins:
`FastPlugs.clap`, `FastPlugs.vst3` and, on the Mac, `FastPlugs.component`.
Hosts list them separately under the vendor FastPlugs.

| Plugin | What it does | Parameters |
|---|---|---|
| Speex Denoise | SpeexDSP noise suppression | Suppression |
| RNNoise Denoise | RNNoise neural network noise suppression | (none besides Mix) |
| Bass and Treble | Low shelf at 100 Hz, high shelf at 4 kHz | Bass, Treble |
| 10 Band EQ | Graphic EQ, one band per octave from 31 Hz to 16 kHz | One gain per band |
| 4 Band Parametric EQ | Each band is a peak, a shelf, a low pass or a high pass | Shape, frequency, gain and Q per band |
| Channel Mixer | Level and pan per channel, then width, swap and centre cancel | Left/right level and pan, Width, Swap channels, Centre cancel |
| Compressor | Stereo linked, 6 dB soft knee | Threshold, Ratio, Attack, Release, Automatic makeup gain, Makeup gain |
| Limiter | Brickwall limiter with lookahead | Ceiling, Release, Lookahead |
| Noise Gate | Hysteresis, hold and lookahead | Threshold, Range, Attack, Hold, Release, Lookahead |
| Normaliser | Rides the level towards a target | Target, Maximum boost, Release, Lookahead |
| Pitch Shifter | Changes pitch without changing speed (Signalsmith Stretch) | Semitones, Cents, Quality |
| Clipping Monitor | Beeps when the signal reaches a threshold, without touching the audio | Threshold, Least time between beeps |
| Buffer | Keeps the last minutes of audio, rewinds through them, saves them | Length, Skip, Rewind, Fast forward, Jump to live, Save to file |

Every plugin also has **Bypass**, which hosts treat as their own bypass
switch. All of them except the Clipping Monitor also have **Mix**, the
dry/wet balance. Both fade over 20 ms rather than switching instantly.

The ranges, defaults and value text are FastRoute's. The ranges and defaults
are read from FastRoute's own `model.h` at build time, so they cannot drift.

FastRoute's plugin-host block is not included, because it is a host, not an
effect.

### No custom interface

The plugins have no editor window of their own. The host shows its standard
parameter list instead: REAPER's generic controls, Logic's Controls view,
Live's device panel. That is deliberate. A host's own controls work with the
host's screen reader support and keyboard navigation, and a custom drawn
editor usually works with neither.

Every value reads as text a screen reader can say: "-20 dB", "4:1",
"Left 40", "Off", "Full scale". You can type values back in the same form, so
"2k" works for a frequency and "centre" works for a pan.

### Mono tracks

Every plugin accepts mono in, mono out, stereo in and out, and mono in with
stereo out. FastRoute's effects are stereo inside. A mono input goes to both
sides, and a mono output is the average of the two. That leaves every effect
but the Channel Mixer exactly as it would be on one channel.

### Latency

Hosts are told about latency so they can compensate. Several of these effects
change their latency as you change their settings, though. The Pitch Shifter
adds 120 ms as soon as it moves off zero; the gate and the normaliser follow
their lookahead.

So a plugin reports whatever latency it needed when it was started. If a
setting later needs less, the plugin delays the audio by the difference
itself, so the host's compensation stays correct and nothing restarts. Only a
change that needs more latency than was reported makes the plugin ask the
host to restart it. The settings that can do that are marked as not
automatable: the three Lookaheads, the Pitch Shifter's Quality and the
Buffer's Length. Two automatable settings can still trigger a restart, and
only if the plugin was started with them at zero: moving the Pitch Shifter off
zero, and raising the Noise Gate's Range from zero, since the gate switches
itself off, lookahead included, at zero range.

### The Buffer

**Rewind**, **Fast forward**, **Jump to live** and **Save to file** are
buttons. Hosts draw them as switches. Any change to one presses it, and the
plugin sets it back to Off afterwards for hosts that support that.

**Save to file** writes 24-bit WAV to `Documents/FastPlugs`. A host has no way
to offer a folder picker for a parameter, so that location is fixed. FastRoute
can save as FLAC, MP3 and AAC as well, but it does that through Media
Foundation, which only exists on Windows. The plugins only write WAV.

Changing **Length** clears the buffer, just as it does in FastRoute.

### The Clipping Monitor

On Windows it beeps through the default output device at the same pitch
FastRoute uses. On the Mac it plays your alert sound instead, because macOS
has no simple call that plays a tone at a given pitch. Either way, the beep
never goes into the track. Bypass stops it watching.

### Speex Denoise and unusual sample rates

Speex works on 10 ms frames, and its FFT can only handle lengths whose prime
factors are 17 or smaller. Every rate a sound card actually runs at is fine.
At rates where that fails (37 kHz, for example), the plugin refuses to start
and tells the host why. Without that check, SpeexDSP would call `exit()` and
take the host down with it.

## Installing

Windows:

- `FastPlugs.clap` goes in `C:\Program Files\Common Files\CLAP`
- the `FastPlugs.vst3` folder goes in `C:\Program Files\Common Files\VST3`

macOS:

- `FastPlugs.clap` goes in `~/Library/Audio/Plug-Ins/CLAP`
- `FastPlugs.vst3` goes in `~/Library/Audio/Plug-Ins/VST3`
- `FastPlugs.component` goes in `~/Library/Audio/Plug-Ins/Components`

The Mac builds are signed ad hoc, not with a Developer ID, and they are not
notarised. A copy downloaded through a browser is quarantined and will not
load until you clear the quarantine:

    xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/Components/FastPlugs.component

(The same goes for the `.clap` and the `.vst3`.)

## Building

You need CMake 3.24 or later, plus Visual Studio 2022 on Windows or Xcode's
command line tools on the Mac. Everything else is fetched on the first
configure: FastRoute, RNNoise, SpeexDSP, Signalsmith Stretch, the CLAP
headers, clap-wrapper, and the VST3 and AudioUnit SDKs.

    cmake -S . -B build
    cmake --build build --config Release
    ctest --test-dir build -C Release --output-on-failure

The plugins end up in `build/out`. On the Mac, the build is universal (arm64
and x86_64) and supports macOS 10.15 and later.

By default the build fetches FastRoute at the commit pinned in
`cmake/dependencies.cmake`. To build against a working copy instead:

    cmake -S . -B build -DFASTROUTE_SOURCE_DIR=C:/git/FastRoute

### How FastRoute's sources are compiled

FastRoute's effects include `audio.h` and `util.h`. The real versions of those
headers pull in `windows.h`, WASAPI and the rest of the application. So the
effect sources are copied into the build tree, where those two includes find
the stand-ins in `src/shim` instead. The stand-ins declare only what the
effects use: the `Effect` interface, the recorder the Buffer saves through,
`FormatW`, `DurationText`, `LogError` and `Beep`. `model.h` is FastRoute's
own, used as it is.

If FastRoute changes the `Effect` interface, this build stops compiling. Make
the matching change in `src/shim/audio.h`.

Two short functions from FastRoute's `model.cpp` are repeated in
`src/shim/shim.cpp`: `EqBandHz` and `AutoMakeupDb`. They are the only logic
here that is duplicated rather than shared.

### Layout

    cmake/dependencies.cmake   everything fetched, and pinned
    src/shim/                  stand-ins for FastRoute's audio.h and util.h
    src/plugin/effects.cpp     each effect's name, parameters and Block mapping
    src/plugin/plugin.cpp      the CLAP plugin around any one effect
    src/plugin/factory.cpp     the bundle's factory, with AU and VST3 identities
    tests/smoke.cpp            a small host that exercises every plugin

### Testing

`tests/smoke.cpp` loads every plugin and checks the following:

- each parameter's text reads back as the same value
- every channel layout runs
- random automation at odd block sizes produces finite audio
- the reported latency matches the latency measured with an impulse, including
  after a setting reduces it
- needing more latency asks the host for a restart
- saved state loads back exactly
- unusual sample rates are refused or handled, never crash

Set `FASTPLUGS_TEST_SAVE=1` to also have it press the Buffer's Save. That
writes a real file into `Documents/FastPlugs`.

CI also runs clap-validator on the CLAP, pluginval (strictness 8) on the VST3
and `auval` on every Audio Unit.

## Identity

The plugin ids (`io.github.masonasons.fastplugs.*`), the AU manufacturer code
`FPlg` and the AU subtypes are what saved projects refer to. The VST3 class
ids are derived from the plugin ids. Changing any of them after a release
means existing projects can no longer find the plugin. They are set in
`CMakeLists.txt` and `src/plugin/effects.cpp`.

## Licence

GPLv3, the same as FastRoute, whose code this is. The VST3 SDK (MIT since
3.8), Apple's AudioUnitSDK (Apache 2.0), clap-wrapper and the CLAP headers
(both MIT) are all compatible with it.
