#pragma once
#include "Dls.h"
#include "Wav.h"

struct NoteSpec
{
	int program = 0;
	int key = 60;
	int velocity = 100;
	double holdSec = 2.0;
	double tailSec = 1.0;
};

// Plays one note from a DLS instrument offline (mono, float).
bool renderGmNote(const DlsFile& dls, const NoteSpec& n, int sampleRate, Audio& out);
