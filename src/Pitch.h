#pragma once
#include "Wav.h"

// The pitch of a sustained note, from the spacing of its three loudest partials: the highest
// fundamental whose harmonics explain them. The loudest partial alone is wrong a third of the
// time (it is often the 2nd or 3rd harmonic). Returns 0 when nothing is sustained.
struct PitchEstimate
{
	double hz = 0;
	double confidence = 0; // share of the three partials' amplitude that sits on the harmonic series, 0..1
};
PitchEstimate estimatePitch(const Audio& a);
