#pragma once
#include <string>
#include <vector>

// The envelope and tremolo of a note, read from its broadband level (dB) against time. Independent of gain.
struct EnvelopeFeatures
{
	double attackSec = 0;   // note-on to within 2 dB of the peak
	double decaySec = 0;    // peak to within 1 dB of the sustain level
	double sustainDb = 0;   // held level relative to the peak (<= 0)
	double releaseSec = -1; // note-off to 20 dB below the level at note-off; -1 = note-off unknown
	double tremoloHz = 0;   // 0 = none found
	double tremoloDb = 0;   // peak-to-peak ripple on the held part
};

// levelDb: one value per frame (any offset); frameSec: seconds per frame; noteOffFrame: -1 when unknown
EnvelopeFeatures extractFeatures(const std::vector<double>& levelDb, double frameSec, long long noteOffFrame);

// 0 (same) .. 1 (unrelated): the mean over the features both sides can supply
double featureDistance(const EnvelopeFeatures& a, const EnvelopeFeatures& b);

std::string describe(const EnvelopeFeatures& f);
