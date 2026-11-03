#include "GmRender.h"
#include <algorithm>
#include <cmath>

namespace
{
float hermite(const std::vector<float>& x, double pos)
{
	const long i = static_cast<long>(pos);
	const float t = static_cast<float>(pos - i);
	auto at = [&](long k) { return x[std::clamp<long>(k, 0, static_cast<long>(x.size()) - 1)]; };
	const float y0 = at(i - 1), y1 = at(i), y2 = at(i + 1), y3 = at(i + 2);
	const float c1 = 0.5f * (y2 - y0), c2 = y0 - 2.5f * y1 + 2 * y2 - 0.5f * y3, c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
	return ((c3 * t + c2) * t + c1) * t + y1;
}
} // namespace

bool renderGmNote(const DlsFile& dls, const NoteSpec& n, int sr, Audio& out)
{
	const DlsInstrument* inst = dls.find(n.program);
	if (!inst)
		return false;
	const DlsRegion* reg = nullptr;
	for (auto& r : inst->regions)
		if (n.key >= r.keyLo && n.key <= r.keyHi && n.velocity >= r.velLo && n.velocity <= r.velHi)
		{
			reg = &r;
			break;
		}
	if (!reg)
		return false;
	const DlsWave& w = dls.waves[reg->wave];

	const double step = std::exp2(((n.key - reg->unityNote) * 100.0 + reg->fineTuneCents) / 1200.0) * w.rate / sr;
	const size_t total = static_cast<size_t>((n.holdSec + n.tailSec) * sr), holdN = static_cast<size_t>(n.holdSec * sr);
	const double vel = n.velocity / 127.0, gain = vel * vel * std::pow(10.0, reg->gainDb / 20.0);

	out.sampleRate = sr;
	out.s.assign(total, 0.f);
	out.onset = 0;

	auto modTime = [&](double base, const DlsRegion::TimeMod& m) {
		return base <= 0 ? 0.0 : base * std::exp2((m.key * n.key / 127.0 + m.vel * n.velocity / 127.0) / 1200.0);
	};
	const double attack = modTime(reg->attack, reg->attackMod), hold = modTime(reg->hold, reg->holdMod);
	const double decay = modTime(reg->decay, reg->decayMod), release = modTime(reg->release, reg->releaseMod);
	const double atkN = attack * sr, holdEgN = hold * sr;
	// exponential decay/release: time constant such that 96 dB takes the stated time
	auto expCoef = [&](double secs) { return secs <= 0 ? 0.0 : std::pow(10.0, -96.0 / 20.0 / (secs * sr)); };
	const double decCoef = expCoef(decay), relCoef = expCoef(release);

	double pos = 0, env = 0;
	enum { Att, Hold, Dec, Sus, Rel } stage = Att;
	size_t stageN = 0;
	const double loopEnd = reg->loopStart + reg->loopLen;
	for (size_t i = 0; i < total; ++i)
	{
		if (i == holdN)
			stage = Rel;
		switch (stage)
		{
		case Att:
			env = atkN > 1 ? std::min(1.0, (stageN + 1) / atkN) : 1.0;
			if (env >= 1.0) { stage = Hold; stageN = 0; } else ++stageN;
			break;
		case Hold:
			if (++stageN >= holdEgN) stage = Dec;
			break;
		case Dec:
			if (decay <= 0 || reg->sustain >= 1.0) { stage = Sus; break; }
			env *= decCoef;
			if (env <= reg->sustain) { env = reg->sustain; stage = Sus; }
			break;
		case Sus: env = reg->sustain; break;
		case Rel: env *= relCoef; break;
		}

		if (pos >= w.pcm.size() - 1 && !reg->loop)
			break;
		out.s[i] = static_cast<float>(hermite(w.pcm, pos) * env * gain);
		pos += step;
		if (reg->loop && reg->loopLen > 1 && pos >= loopEnd) // the release keeps looping until the envelope is gone
			pos -= reg->loopLen;
		if (stage == Rel && env < 1e-5)
			break;
	}
	return true;
}
