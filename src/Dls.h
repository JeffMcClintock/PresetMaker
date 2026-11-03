#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Reader for DLS Level 1/2 banks, enough to play gm.dls (the Windows GM wavetable).
struct DlsWave
{
	std::vector<float> pcm; // mono
	double rate = 22050;
};

struct DlsRegion
{
	int keyLo = 0, keyHi = 127, velLo = 0, velHi = 127;
	int wave = -1;
	int unityNote = 60;
	double fineTuneCents = 0;
	double gainDb = 0;
	bool loop = false;
	uint32_t loopStart = 0, loopLen = 0;
	// EG1, seconds (sustain is a 0..1 amplitude fraction)
	double attack = 0.0, hold = 0.0, decay = 0.0, sustain = 1.0, release = 0.2;
	// Time modulation by key number / velocity, in timecents over the full 0..127 range (added to the log of the time).
	struct TimeMod { double key = 0, vel = 0; };
	TimeMod attackMod, holdMod, decayMod, releaseMod;
};

struct DlsInstrument
{
	std::string name;
	uint32_t bank = 0, program = 0;
	bool drum = false;
	std::vector<DlsRegion> regions;
};

struct DlsFile
{
	std::vector<DlsWave> waves;
	std::vector<DlsInstrument> instruments;

	bool load(const std::string& path);
	const DlsInstrument* find(int program, bool drum = false) const;
};
