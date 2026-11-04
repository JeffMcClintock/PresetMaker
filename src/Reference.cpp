#include "Reference.h"
#include "Platform.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <vector>

namespace fs = std::filesystem;

namespace
{
struct GmReference : ReferenceSource
{
	DlsFile dls;
	int program = 0;
	bool render(const NoteSpec& n, int sr, Audio& out) override
	{
		NoteSpec m = n;
		m.program = program;
		return renderGmNote(dls, m, sr, out);
	}
	std::string describe() const override
	{
		for (auto& i : dls.instruments)
			if (!i.drum && i.bank == 0 && static_cast<int>(i.program) == program) return "GM " + std::to_string(program) + " " + i.name;
		return "GM " + std::to_string(program);
	}
};

struct WavSample
{
	int key = 60, vel = 100;
	Audio a;
};

// A one-shot sampler: nearest key (and velocity) is played back at the ratio that puts it in tune.
struct WavReference : ReferenceSource
{
	std::vector<WavSample> samples;
	std::string folder;

	bool render(const NoteSpec& n, int sr, Audio& out) override
	{
		if (samples.empty()) return false;
		const WavSample* best = &samples[0];
		for (auto& s : samples)
			if (std::abs(s.key - n.key) * 1000 + std::abs(s.vel - n.velocity) < std::abs(best->key - n.key) * 1000 + std::abs(best->vel - n.velocity)) best = &s;
		const double ratio = std::pow(2.0, (n.key - best->key) / 12.0) * best->a.sampleRate / sr;
		const size_t total = static_cast<size_t>((n.holdSec + n.tailSec) * sr);
		out.sampleRate = sr;
		out.s.assign(total, 0.f);
		const double gain = n.velocity / 127.0;
		for (size_t i = 0; i < total; ++i)
		{
			const double p = i * ratio;
			const size_t k = static_cast<size_t>(p);
			if (k + 1 >= best->a.s.size()) break;
			const double f = p - k;
			out.s[i] = static_cast<float>(gain * (best->a.s[k] * (1 - f) + best->a.s[k + 1] * f));
		}
		return true;
	}
	std::string describe() const override { return folder + " (" + std::to_string(samples.size()) + " samples)"; }
};

bool parseSampleName(const std::string& stem, int& key, int& vel)
{
	char* end;
	key = static_cast<int>(strtol(stem.c_str(), &end, 10));
	if (end == stem.c_str()) return false;
	vel = 100;
	if (*end == '_' || *end == '-') vel = atoi(end + 1);
	return key >= 0 && key < 128;
}
} // namespace

std::unique_ptr<ReferenceSource> openReference(const std::string& spec, std::string& error)
{
	std::string s = spec;
	if (s.rfind("gm:", 0) == 0) s = s.substr(3);
	if (!s.empty() && std::all_of(s.begin(), s.end(), ::isdigit))
	{
		auto r = std::make_unique<GmReference>();
		r->program = atoi(s.c_str());
		const std::string dls = platform::gmSoundSetPath();
		if (!r->dls.load(dls)) { error = "cannot read " + dls; return nullptr; }
		return r;
	}
	auto r = std::make_unique<WavReference>();
	r->folder = spec;
	std::vector<fs::path> files;
	std::error_code ec;
	if (fs::is_directory(spec, ec))
		for (auto& e : fs::directory_iterator(spec)) files.push_back(e.path());
	else
		files.push_back(spec);
	for (auto& f : files)
	{
		std::string ext = f.extension().string();
		std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
		if (ext != ".wav") continue;
		WavSample ws;
		if (files.size() > 1 && !parseSampleName(f.stem().string(), ws.key, ws.vel)) continue;
		if (!readWav(f.string(), ws.a)) continue;
		r->samples.push_back(std::move(ws));
	}
	if (r->samples.empty()) { error = "no WAV samples in " + spec + " (expected <key>.wav or <key>_<vel>.wav)"; return nullptr; }
	return r;
}
