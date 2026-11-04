#pragma once
#include <functional>
#include <map>
#include <string>
#include <vector>
#include "Metrics.h"
#include "Reference.h"
#include "Vst3Host.h"

struct FitOptions
{
	std::string plugin;          // path to the .vst3
	std::string reference;       // see openReference()
	std::string outDir;          // ref_*.wav, fit_*.wav, preset.txt
	std::string name;            // preset name; empty = no .vstpreset
	std::string presetDir;       // empty = the standard VST3 Presets folder
	std::vector<int> keys{48, 60, 72};
	int velocity = 100;
	double holdSec = 1.5, tailSec = 0.5;
	int budget = 1200;
	std::string start;           // preset.txt to warm-start from
	double sigma = 0;            // initial CMA step; 0 = 0.25 cold / 0.05 warm
	double preRollSec = 0.15;
	std::map<std::string, double> fixed; // parameter title or "#index" -> held value (left out of the fit)
	std::map<uint32_t, double> fixedById; // the same by parameter id (the GUI's Ignore ticks; titles can repeat)
	// title substrings left out of the fit (at the plugin's value); MIDI controller parameters by default
	std::vector<std::string> skip{"MIDI CC", "Pitch Bend", "Aftertouch", "Channel Pressure", "Program Change"};
	int seed = 7;
	int sampleRate = 48000;
	// Semitones added to the target's key: a target pitched an octave below the plugin is played an
	// octave up so the two can meet. Auto: whole octaves, detected from the pitch of each training
	// note and confirmed by a trial, else 0. The preset carries no offset; the fitted sound sits
	// where the plugin plays it.
	int transpose = 0;
	bool autoTranspose = true;
};

struct FitProgress
{
	int evals = 0, budget = 0;
	double best = 0, sigma = 0, seconds = 0;
};

struct FitResult
{
	bool ok = false;
	std::string error;
	std::string presetTxt, vstPreset;
	int transpose = 0;  // what the target was played at, semitones
	double trainBest = 0;
	std::vector<std::pair<int, Score>> perKey;
};

// Parameters the search may move: tweakable, not fixed, not skipped.
std::vector<size_t> freeParams(const Vst3Plugin& vst, const FitOptions& o);
void applyFixed(Vst3Plugin& vst, const FitOptions& o);
const double* fixedValue(const Vst3Plugin& vst, const FitOptions& o, size_t k); // held value for parameter k, or null
bool loadPresetTxt(Vst3Plugin& vst, const std::string& path, const FitOptions* skipFixedOf = nullptr);
bool savePresetTxt(const Vst3Plugin& vst, const std::string& path, int transpose = 0);
int presetTranspose(const std::string& path); // the "# transpose N" line, else 0

// progress returns false to cancel. log receives one line at a time.
FitResult runFit(const FitOptions& o, ScoreWeights weights,
                 std::function<bool(const FitProgress&)> progress, std::function<void(const std::string&)> log);

// Held-out notes (keys 43/55/67/79 at vel 100, key 60 at vel 60/120): returns the mean similarity.
double checkPreset(const FitOptions& o, ScoreWeights weights, const std::string& presetTxt, std::function<void(const std::string&)> log);
