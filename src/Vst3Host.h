#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "GmRender.h"

struct VstParam
{
	uint32_t id = 0;
	std::string title;
	int steps = 0;        // 0 = continuous; n = n+1 discrete values
	double def = 0;
	bool tweakable = true; // not read-only / hidden / program-change / bypass
};

// Loads a VST3 instrument and renders notes offline with a given parameter set.
class Vst3Plugin
{
public:
	Vst3Plugin();
	~Vst3Plugin();

	bool load(const std::string& path, std::string& error);
	const std::string& name() const;
	const std::string& vendor() const;
	const std::vector<VstParam>& params() const;
	std::string describeBuses() const;

	// The standard folder DAW preset browsers scan: Documents\VST3 Presets\<vendor>\<plugin>.
	std::string defaultPresetDir() const;
	// Writes a .vstpreset holding the plugin's own state for the current values().
	bool savePreset(const std::string& path, std::string& error);

	std::vector<double> values; // normalised, one per params() entry
	double preRollSec = 0.0;    // silent audio (and wall-clock time) before the note, for background-baking plugins

	// Fresh activation per call so voices and effect tails never leak between renders.
	bool render(const NoteSpec& note, int sampleRate, Audio& out);

private:
	struct Impl;
	std::unique_ptr<Impl> p;
};
