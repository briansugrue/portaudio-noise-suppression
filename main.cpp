// REALTIME AUDIO NOISE SUPPRESSION (VAD-gated Wiener filter)

/*
 * Captures mono audio input, suppresses noise with a Wiener filter whose
 * noise estimate is controlled by a voice activity detector (wiener_vad.h),
 * applies a user-adjustable gain, and outputs stereo audio in real time
 * using PortAudio.
 * Audio processing runs on PortAudio's high-priority callback thread,
 * while user input and control run on the main thread.
 *
 * Use headphones: the microphone is routed straight to the output.
 */

#include <portaudio.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <sndfile.h>

#include "wiener_vad.h"

 // The filter hop must equal the PortAudio buffer size (frame = 2 * hop).
constexpr int frames_per_buffer = 256;   // Process 256 frames at a time

std::atomic<float> gGain(0.5f);          // Global gain, shared between threads
std::atomic<bool>  gBypass(false);       // true = hear the unprocessed input
std::atomic<float> gInRms(0.0f);         // Smoothed input level, for the live meter
std::atomic<float> gOutRms(0.0f);        // Smoothed filter-output level

// Recording: buffers are allocated once in main(), so the callback never allocates.
constexpr double kMaxRecordSec = 60.0;
std::vector<float> gRecIn;               // Raw microphone samples
std::vector<float> gRecOut;              // Filter output (before the gain stage)
std::atomic<int>    gRecCmd(0);          // 1 = start, 2 = stop (main -> callback)
std::atomic<bool>   gRecActive(false);   // Set by the callback
std::atomic<size_t> gRecLen(0);          // Samples recorded so far

// Writes n float samples (-1..1, clipped) as a mono 16-bit WAV via libsndfile.
static bool writeWav16(const std::string& path, const float* x, size_t n, int sampleRate) {
	SF_INFO info{};
	info.samplerate = sampleRate;
	info.channels = 1;
	info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
	SNDFILE* f = sf_open(path.c_str(), SFM_WRITE, &info);
	if (!f) return false;
	const sf_count_t written = sf_write_float(f, x, static_cast<sf_count_t>(n));
	sf_close(f);
	return written == static_cast<sf_count_t>(n);
}

// Runs on the audio thread: no allocation, no locking, no printing.
static int audioCallback(const void* inputBuffer, void* outputBuffer,
	unsigned long framesPerBuffer, const PaStreamCallbackTimeInfo*,
	PaStreamCallbackFlags, void* userData)
{
	const float* in = static_cast<const float*>(inputBuffer);
	float* out = static_cast<float*>(outputBuffer);
	WienerVad* denoiser = static_cast<WienerVad*>(userData);

	// If there is no input, output silence (2 channels per frame)
	if (!in) {
		std::fill(out, out + framesPerBuffer * 2, 0.0f);
		return paContinue;
	}

	// Always run the filter so its state stays current, even when bypassed
	const float* clean = denoiser->process(in, framesPerBuffer);
	const float* src = gBypass.load() ? in : clean;

	// Levels for the live meter (single writer: this thread)
	float inE = 0.0f, outE = 0.0f;
	for (unsigned long i = 0; i < framesPerBuffer; ++i) {
		inE += in[i] * in[i];
		outE += clean[i] * clean[i];
	}
	const float n = static_cast<float>(framesPerBuffer);
	gInRms.store(0.95f * gInRms.load() + 0.05f * std::sqrt(inE / n));
	gOutRms.store(0.95f * gOutRms.load() + 0.05f * std::sqrt(outE / n));

	// Recording of the raw input and the filter output
	const int recCmd = gRecCmd.exchange(0);
	if (recCmd == 1) { gRecLen.store(0); gRecActive.store(true); }
	else if (recCmd == 2) { gRecActive.store(false); }
	if (gRecActive.load()) {
		size_t pos = gRecLen.load();
		for (unsigned long i = 0; i < framesPerBuffer && pos < gRecIn.size(); ++i, ++pos) {
			gRecIn[pos] = in[i];
			gRecOut[pos] = clean[i];
		}
		gRecLen.store(pos);
		if (pos >= gRecIn.size()) gRecActive.store(false);   // buffer full
	}

	const float gain = gGain.load();   // Read once per buffer
	for (unsigned long i = 0; i < framesPerBuffer; ++i) {
		const float sample = src[i] * gain;
		out[i * 2] = sample;       // Left
		out[i * 2 + 1] = sample;       // Right
	}
	return paContinue;
}

// Prints a PortAudio error and returns false if err indicates failure.
static bool ok(PaError err, const char* what) {
	if (err >= 0) return true;
	std::cerr << what << " failed: " << Pa_GetErrorText(err) << std::endl;
	return false;
}

// Asks the user for a device index. Pressing Enter accepts the default.
static int chooseDevice(const char* label, int defaultIndex, bool isInput) {
	const int count = Pa_GetDeviceCount();
	while (true) {
		std::cout << "Select " << label << " device [Enter = " << defaultIndex << "]: ";
		std::string line;
		if (!std::getline(std::cin, line)) return paNoDevice;
		if (line.empty()) return defaultIndex;

		std::istringstream ss(line);
		int idx;
		if (ss >> idx && idx >= 0 && idx < count) {
			const PaDeviceInfo* info = Pa_GetDeviceInfo(idx);
			const int channels = isInput ? info->maxInputChannels : info->maxOutputChannels;
			if (channels > 0) return idx;
			std::cout << "That device has no " << (isInput ? "input" : "output") << " channels.\n";
		}
		else {
			std::cout << "Invalid choice.\n";
		}
	}
}

int main() {
	if (!ok(Pa_Initialize(), "Pa_Initialize")) return 1;

	const int numDevices = Pa_GetDeviceCount();
	if (numDevices <= 0) {
		std::cerr << "No audio devices found." << std::endl;
		Pa_Terminate();
		return 1;
	}

	for (int i = 0; i < numDevices; ++i) {
		const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
		const PaHostApiInfo* api = Pa_GetHostApiInfo(info->hostApi);
		std::cout << i << ": " << info->name << " (" << (api ? api->name : "?") << ")"
			<< " | Inputs: " << info->maxInputChannels
			<< " | Outputs: " << info->maxOutputChannels << std::endl;
	}

	// Defaults come from the OS, so this works on any machine's IO setup
	const int inputDeviceIndex = chooseDevice("input", Pa_GetDefaultInputDevice(), true);
	const int outputDeviceIndex = chooseDevice("output", Pa_GetDefaultOutputDevice(), false);
	if (inputDeviceIndex == paNoDevice || outputDeviceIndex == paNoDevice) {
		std::cerr << "No usable input/output device." << std::endl;
		Pa_Terminate();
		return 1;
	}

	const PaDeviceInfo* inInfo = Pa_GetDeviceInfo(inputDeviceIndex);
	const PaDeviceInfo* outInfo = Pa_GetDeviceInfo(outputDeviceIndex);

	PaStreamParameters inputParams{};
	inputParams.device = inputDeviceIndex;
	inputParams.channelCount = 1;
	inputParams.sampleFormat = paFloat32;
	inputParams.suggestedLatency = inInfo->defaultLowInputLatency;

	PaStreamParameters outputParams{};
	outputParams.device = outputDeviceIndex;
	outputParams.channelCount = 2;
	outputParams.sampleFormat = paFloat32;
	outputParams.suggestedLatency = outInfo->defaultLowOutputLatency;

	// Try 48 kHz first, then fall back to rates the devices actually support
	const std::vector<double> candidateRates = {
		48000.0, 44100.0, inInfo->defaultSampleRate, outInfo->defaultSampleRate };
	double sampleRate = 0.0;
	for (double rate : candidateRates) {
		if (Pa_IsFormatSupported(&inputParams, &outputParams, rate) == paFormatIsSupported) {
			sampleRate = rate;
			break;
		}
	}
	if (sampleRate == 0.0) {
		std::cerr << "These devices don't share a supported sample rate with "
			"1 input channel and 2 output channels." << std::endl;
		Pa_Terminate();
		return 1;
	}
	std::cout << "Using sample rate: " << sampleRate << " Hz" << std::endl;

	// Declared before the stream so it outlives the audio callback
	WienerVadParams vadParams;
	vadParams.minGain = 0.25f;
	WienerVad denoiser(sampleRate, frames_per_buffer, vadParams);

	const size_t recCapacity = static_cast<size_t>(sampleRate * kMaxRecordSec);
	gRecIn.assign(recCapacity, 0.0f);
	gRecOut.assign(recCapacity, 0.0f);
	bool recording = false;

	PaStream* stream = nullptr;
	if (!ok(Pa_OpenStream(&stream, &inputParams, &outputParams, sampleRate,
		frames_per_buffer, paNoFlag, audioCallback, &denoiser), "Pa_OpenStream")) {
		Pa_Terminate();
		return 1;
	}

	if (!ok(Pa_StartStream(stream), "Pa_StartStream")) {
		Pa_CloseStream(stream);
		Pa_Terminate();
		return 1;
	}
	std::cout << "Stream started. Current gain: " << gGain.load() << std::endl;

	// The filter learns the noise from the first moments: stay quiet.
	std::cout << "Learning the background noise - stay quiet for "
		<< vadParams.calibrationSec << " s..." << std::endl;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (denoiser.isCalibrating() && std::chrono::steady_clock::now() < deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	std::cout << "Noise suppression is active.\n"
		"Commands: a gain 0.0-1.0, 'b' = toggle bypass, "
		"'r' = re-learn noise, 'm' = live meter,\n"
		"'s' = start/stop recording (saves WAV files), -1 = quit." << std::endl;

	// User controls the stream until they enter a negative number
	while (true) {
		std::cout << "> ";
		std::string line;
		if (!std::getline(std::cin, line)) break;

		if (!line.empty() && (line[0] == 'b' || line[0] == 'B')) {
			const bool now = !gBypass.load();
			gBypass.store(now);
			std::cout << (now ? "Bypass ON (unprocessed audio)\n"
				: "Bypass OFF (noise suppression)\n");
			continue;
		}
		if (!line.empty() && (line[0] == 'r' || line[0] == 'R')) {
			denoiser.recalibrate();
			std::cout << "Re-learning noise - stay quiet for "
				<< vadParams.calibrationSec << " s...\n";
			continue;
		}

		if (!line.empty() && (line[0] == 's' || line[0] == 'S')) {
			const auto waitFor = [](bool wantActive) {
				for (int t = 0; t < 200 && gRecActive.load() != wantActive; ++t)
					std::this_thread::sleep_for(std::chrono::milliseconds(5));
				};
			if (!recording) {
				gRecCmd.store(1);
				waitFor(true);
				recording = true;
				std::cout << "Recording (max " << kMaxRecordSec
					<< " s). Enter 's' again to stop and save.\n";
				continue;
			}

			gRecCmd.store(2);
			waitFor(false);
			recording = false;

			const size_t hop = frames_per_buffer;
			const size_t len = gRecLen.load();
			if (len <= hop) {
				std::cout << "Recording too short, nothing saved.\n";
				continue;
			}

			// The filter output lags the input by one hop: skip it to line them up.
			char stamp[32];
			const std::time_t now = std::time(nullptr);
			std::tm tmv{};
#ifdef _WIN32
			localtime_s(&tmv, &now);
#else
			localtime_r(&now, &tmv);
#endif
			std::strftime(stamp, sizeof stamp, "%Y%m%d_%H%M%S", &tmv);

			const std::string base = std::string("rec_") + stamp;
			const std::string inFile = base + "_input.wav";
			const std::string outFile = base + "_filtered.wav";
			const int fsInt = static_cast<int>(sampleRate + 0.5);
			const size_t n = len - hop;
			const bool a = writeWav16(inFile, gRecIn.data(), n, fsInt);
			const bool b = writeWav16(outFile, gRecOut.data() + hop, n, fsInt);
			if (a && b) {
				std::cout << "Saved " << n / sampleRate << " s to:\n  "
					<< std::filesystem::absolute(inFile).string() << "\n  "
					<< std::filesystem::absolute(outFile).string() << "\n";
			}
			else {
				std::cout << "Could not write the WAV files.\n";
			}
			continue;
		}

		if (!line.empty() && (line[0] == 'm' || line[0] == 'M')) {
			std::cout << "Live meter for 10 s (dB, 0 dB = full scale). "
				"'change' is output minus input.\n";
			for (int t = 0; t < 100; ++t) {
				const float inDb = 20.0f * std::log10(gInRms.load() + 1e-9f);
				const float outDb = 20.0f * std::log10(gOutRms.load() + 1e-9f);
				std::printf("\rin %6.1f dB | out %6.1f dB | change %6.1f dB | %-11s",
					inDb, outDb, outDb - inDb,
					denoiser.isCalibrating() ? "calibrating"
					: (denoiser.speechDetected() ? "SPEECH" : "noise"));
				std::fflush(stdout);
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
			}
			std::printf("\n");
			continue;
		}

		std::istringstream ss(line);
		float newGain;
		if (!(ss >> newGain)) {
			std::cout << "Please enter a number.\n";
			continue;
		}
		if (newGain < 0.0f) break;

		gGain.store(std::clamp(newGain, 0.0f, 1.0f));
		std::cout << "Updated gain level: " << gGain.load() << std::endl;
	}

	Pa_StopStream(stream);    // Stops the callback
	Pa_CloseStream(stream);   // Frees the stream
	Pa_Terminate();           // Shuts down PortAudio
	std::cout << "Audio stream closed." << std::endl;
	return 0;
}