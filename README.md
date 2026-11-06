# portaudio-noise-suppression

Real-time noise suppression in C++. Microphone audio is captured with PortAudio, cleaned with a Wiener filter whose noise estimate is controlled by a voice activity detector (VAD), and played back in stereo with an adjustable gain.

> **Use headphones.** The microphone is routed straight to the output, so speakers will cause feedback.

## How it works

Each audio block of 256 samples goes through the following steps (frame = 512 samples, 50% overlap):

1. sqrt-Hann window, then real FFT (KissFFT)
2. VAD on the summed magnitude in the 250–820 Hz band, with hangover
3. Noise PSD update on noise-only frames
4. Wiener gain with a decision-directed a priori SNR estimate and a gain floor
5. Inverse FFT, sqrt-Hann synthesis window, overlap-add

sqrt-Hann analysis and synthesis windows at 50% overlap sum to exactly 1, so no window normalisation is needed. The algorithmic latency is one hop (256 samples). The filter state is allocated up front, so `process()` never allocates, locks or prints and is safe to run on the PortAudio callback thread.

All time constants are specified in seconds, so behaviour does not depend on the sample rate. The filter is a C++ port of an earlier MATLAB prototype, and the VAD follows `vad_vad()` from *Real-Time Digital Signal Processing* (3rd ed.) by Kuo, Lee and Tian.

## Features

- Input and output device selection at start-up (Enter accepts the OS default)
- Automatic sample rate selection (48 kHz, then 44.1 kHz, then the device defaults)
- Noise learning during the first moments of the stream (stay quiet)
- Live bypass toggle for A/B comparison
- Live input/output level meter
- Recording of the raw input and the filtered output to 16-bit WAV files (up to 60 s)

## Controls

| Input | Action |
|-------|--------|
| `0.0`–`1.0` | Set output gain |
| `b` | Toggle bypass (unprocessed audio) |
| `r` | Re-learn the background noise |
| `m` | Show a live meter for 10 s (input, output, change in dB, VAD state) |
| `s` | Start/stop recording; saves `rec_<timestamp>_input.wav` and `rec_<timestamp>_filtered.wav` |
| `-1` | Quit |

## Building

Requires Windows, Visual Studio with the C++ CMake tools (MSVC), and CMake 3.15 or newer. PortAudio, KissFFT and libsndfile are downloaded and built automatically with CMake's FetchContent, so the first configure needs an internet connection and takes a while.

The presets use `$env{VSINSTALLDIR}`, so run them from Visual Studio or a Developer Command Prompt:

```
cmake --preset x64-release
cmake --build out/build/x64-release
```

Alternatively, open the folder in Visual Studio and build from there. The MSVC runtime is linked statically, so the `.exe` does not need the VC++ Redistributable.

## Tuning

The main parameters are in `WienerVadParams` in `wiener_vad.h`:

| Parameter | Default | Meaning |
|-----------|---------|---------|
| `calibrationSec` | 0.5 | Noise-only time at start |
| `hangoverSec` | 0.25 | VAD hangover |
| `noiseTau` | 0.30 | Noise PSD averaging time on noise frames |
| `ddAlpha` | 0.95 | Decision-directed smoothing |
| `minGain` | 0.1 | Gain floor (`main.cpp` sets 0.25) |

A higher `minGain` leaves more residual noise but reduces musical-noise artefacts.

## Dependencies

- [PortAudio](https://github.com/PortAudio/portaudio) v19.7.0
- [KissFFT](https://github.com/mborgerding/kissfft) 131.1.0 (float build)
- [libsndfile](https://github.com/libsndfile/libsndfile) 1.2.2 (WAV only)

## Related

The MATLAB prototype this is ported from: [wiener-filter-noise-suppression](https://github.com/briansugrue/wiener-filter-noise-suppression)
