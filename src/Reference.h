#pragma once
#include <memory>
#include <string>
#include "GmRender.h"

// Where the target notes come from: a GM program in gm.dls, or a folder of sampled WAVs.
struct ReferenceSource
{
	virtual ~ReferenceSource() = default;
	virtual bool render(const NoteSpec& n, int sampleRate, Audio& out) = 0;
	virtual std::string describe() const = 0;
};

// "gm:0" / "0" = GM program (Windows gm.dls); a folder = WAVs named <key>.wav or <key>_<vel>.wav
// (nearest key is resampled to pitch); a single .wav serves every note the same way.
std::unique_ptr<ReferenceSource> openReference(const std::string& spec, std::string& error);
