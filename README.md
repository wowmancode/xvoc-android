<<<<<<< HEAD
# xvoc — Android app

An Android port of `xvoc.c`, your command-line channel vocoder. The original
DSP file is compiled **unmodified** into a native library; a small JNI
bridge calls its existing `read_wav()` / `run_vocoder()` / `randomize_cfg()`
functions directly instead of reimplementing any of the audio math.

## What's here

- `app/src/main/cpp/xvoc_2_.c` — your original file, untouched.
- `app/src/main/cpp/jni_bridge.c` — JNI glue: takes parameter arrays from
  Kotlin, fills a `Cfg`, calls the engine, writes the output WAV.
- `app/src/main/java/com/xvoc/app/`
  - `Params.kt` — the full parameter table (defaults, ranges, grouping)
    that drives every slider in the UI. The index layout here must match
    the enums in `jni_bridge.c` — see the comment at the top of `Idx`.
  - `NativeVocoder.kt` — Kotlin declarations for the native calls.
  - `AudioIO.kt` — imports any audio format Android can decode
    (mp3/m4a/aac/ogg/flac/wav/...) to 16-bit PCM WAV via `MediaExtractor`
    + `MediaCodec` (since the native engine only speaks WAV), and exports
    the result through the system file picker (no storage permission
    needed).
  - `MainActivity.kt` — the Compose UI: Import / Render & Play / Randomize
    / Export, and a scrolling list of every tunable parameter grouped into
    Carrier, Filter bank, Envelope, Band mapping, Sibilance, Mix & post,
    and Output.

## Build

1. Install **Android Studio** (Koala/2024.1 or newer) with the NDK and
   CMake components (SDK Manager → SDK Tools → NDK, CMake).
2. Open this folder as a project. Android Studio will offer to generate
   the Gradle wrapper jar automatically on first sync — accept it (or run
   `gradle wrapper` yourself if you have Gradle installed).
3. Sync, then Run on a device/emulator (arm64-v8a, armeabi-v7a and
   x86_64 are all configured in `app/build.gradle.kts`).

There's no `google()`/`mavenCentral()` fetch possible from this chat
environment, so the project couldn't be compiled to a signed APK here —
this is a complete, ready-to-build Gradle project instead.

## About "live playback"

xvoc's engine is a batch/offline algorithm — it reads a whole WAV, analyzes
it, and writes a whole WAV out; it isn't structured as a streaming,
sample-by-sample real-time processor. So "live" here means: **move a
slider → tap Render & Play → hear the result in about the time it takes to
reprocess your clip** (typically well under a second for a few seconds of
speech on a modern phone), not a zero-latency live mic-through-vocoder
effect. Making it truly real-time would mean rewriting the engine's
block-based analysis (autocorrelation pitch tracking, whole-buffer
level-matching/normalization, etc.) into a fixed-latency streaming
pipeline — a much bigger project than wrapping the existing tool.

## Parameter coverage

Every scalar in `Cfg` that CLI users would reasonably tweak by hand is a
slider/switch/dropdown, matched 1:1 by index in `Params.kt` ↔
`jni_bridge.c`. Left out of the UI on purpose (they're batch/CLI-only
concerns, not per-render parameters): `--mutate` (batch variant
generation), loading a second WAV as the carrier (`--carrier-file`), and
multi-note chords (`nnotes`/`notes[]`) — the engine still defaults to a
single-voice carrier the same way the CLI does with no `-f`/chord flags.

## Files produced at runtime

- `cache/xvoc_in.wav` — your imported audio, decoded to PCM WAV.
- `cache/xvoc_out.wav` — the last render; this is what Export saves and
  what Render & Play plays back. Both live in the app's cache, so they're
  cleared automatically by Android if space is needed.
=======
# xvoc-android
