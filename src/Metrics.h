#pragma once
#include <vector>
#include "Wav.h"

// Perceptual distance between a reference note and a candidate render. Lower is better.
//
// Model of the listener:
//  - ERB-spaced bands (the ear's frequency resolution), not linear FFT bins.
//  - Terhardt threshold-in-quiet: bands below audibility cost nothing, and the 2-5 kHz
//    sensitivity peak makes those bands matter more than deep bass or air.
//  - Compressive specific loudness (Zwicker exponent 0.23): a 3 dB error in a loud band
//    counts for less than the same error at modest level, large level swings saturate.
//  - Masking: band power is spread onto its neighbours (Schroeder spreading function) before the
//    loudness step, so a quiet partial beside a loud one is hidden rather than scored.
//  - The attack identifies an instrument, so a fixed share of the score comes from the first
//    ~100 ms, however long the note is. Each part is judged by error relative to its own loudness.
//  - Spectral change: the loudness change of every band over ~50 ms is compared as well, so a
//    partial that should die quickly is not satisfied by one that merely sits quietly.
//  - Overall gain is fitted out (a master-volume knob), the shape is what is scored.
struct ScoreWeights
{
	double attackTauSec = 0.06;   // decay constant of the attack emphasis
	double attackShare = 0.40;    // fraction of the score decided by the attack (the rest: the whole note)
	double wFine = 0.50;          // 1024-point frames: attack transients, envelope detail
	double wTonal = 0.30;         // 4096-point frames: partial structure
	double wEnvelope = 0.20;      // whole-spectrum loudness vs time
	double wDelta = 0.30;         // change of each band's loudness over deltaLagSec: how partials decay, not just where they sit
	double deltaLagSec = 0.05;
	double wLevel = 0.25;         // |gain offset| per 40 dB: a small cost for any level mismatch, a slope off the near-silent plateau
	double peakSpl = 75.0;        // reference's loudest frame is placed here (dB SPL)
	double maskSpread = 0.0;      // width of the masking spread, 1 = Schroeder, 0 = off (calibration: no gain, see README)
};

struct Score
{
	double total = 1e9;    // weighted distance, 0 = identical
	double fine = 0, tonal = 0, envelope = 0, delta = 0;
	double attack = 0;     // diagnostic: fine-resolution distance over the first 100 ms only
	double gainDb = 0;     // level offset applied to the candidate to match
	double level = 0;      // |gainDb| / 40: the level mismatch as a distance
	double similarity() const; // 0..100
};

class Scorer
{
public:
	Scorer(const Audio& ref, ScoreWeights w = {});
	Score compare(const Audio& cand) const;

	struct Feat
	{
		int n = 0, hop = 0, frames = 0, bands = 0;
		std::vector<float> power; // frames x bands, linear, re full scale
	};

private:
	ScoreWeights w_;
	int sr_ = 48000;
	size_t len_ = 0;
	Audio ref_;
	double offDb_ = 0; // reference calibration
	double refSumSq_ = 0; // of the trimmed reference: seeds the gain fit-out
	std::vector<Feat> refFeat_;
};
