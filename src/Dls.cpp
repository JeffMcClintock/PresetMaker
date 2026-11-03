#include "Dls.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>

namespace
{
struct Chunk
{
	char id[5] = {};
	char type[5] = {}; // for RIFF/LIST
	size_t hdr = 0;    // offset of the chunk header
	size_t data = 0;   // offset of payload (after the list type for LIST/RIFF)
	size_t size = 0;   // payload size (excludes the list type)
};

struct Reader
{
	std::vector<uint8_t> b;

	template <class T> T get(size_t p) const { T v{}; if (p + sizeof(T) <= b.size()) memcpy(&v, &b[p], sizeof(T)); return v; }

	std::vector<Chunk> children(size_t begin, size_t end) const
	{
		std::vector<Chunk> out;
		for (size_t p = begin; p + 8 <= end && p + 8 <= b.size();)
		{
			Chunk c;
			memcpy(c.id, &b[p], 4);
			const uint32_t sz = get<uint32_t>(p + 4);
			c.hdr = p;
			c.data = p + 8;
			c.size = sz;
			if (!memcmp(c.id, "LIST", 4) || !memcmp(c.id, "RIFF", 4))
			{
				memcpy(c.type, &b[p + 8], 4);
				c.data += 4;
				c.size = sz >= 4 ? sz - 4 : 0;
			}
			out.push_back(c);
			p += 8 + sz + (sz & 1);
		}
		return out;
	}
};

bool is(const Chunk& c, const char* id, const char* type = nullptr)
{
	return !memcmp(c.id, id, 4) && (!type || !memcmp(c.type, type, 4));
}

double timeCentsToSec(int32_t scale)
{
	if (static_cast<uint32_t>(scale) == 0x80000000u)
		return 0.0;
	return std::exp2(scale / 65536.0 / 1200.0);
}

// Static (unmodulated) EG1 connections of an art1 chunk.
void applyArt(const Reader& r, const Chunk& art, DlsRegion& g)
{
	const uint32_t cbSize = r.get<uint32_t>(art.data);
	const uint32_t n = r.get<uint32_t>(art.data + 4);
	for (uint32_t i = 0; i < n; ++i)
	{
		const size_t p = art.data + cbSize + i * 12;
		const uint16_t src = r.get<uint16_t>(p), ctl = r.get<uint16_t>(p + 2), dst = r.get<uint16_t>(p + 4), xf = r.get<uint16_t>(p + 6);
		const int32_t scale = r.get<int32_t>(p + 8);
		if (ctl != 0 || xf != 0)
			continue;
		// src 2 = velocity, 3 = key number: a time offset in timecents over the full range
		if (src == 2 || src == 3)
		{
			DlsRegion::TimeMod* m = dst == 0x206 ? &g.attackMod : dst == 0x207 ? &g.decayMod : dst == 0x209 ? &g.releaseMod : dst == 0x20C ? &g.holdMod : nullptr;
			if (m)
				(src == 2 ? m->vel : m->key) = scale / 65536.0;
			continue;
		}
		if (src != 0)
			continue;
		switch (dst)
		{
		case 0x206: g.attack = timeCentsToSec(scale); break;
		case 0x207: g.decay = timeCentsToSec(scale); break;
		case 0x209: g.release = timeCentsToSec(scale); break;
		case 0x20A: g.sustain = std::clamp(scale / 65536.0 / 1000.0, 0.0, 1.0); break;
		case 0x20C: g.hold = timeCentsToSec(scale); break;
		}
	}
}

void applyWsmp(const Reader& r, const Chunk& c, DlsRegion& g)
{
	const uint32_t cb = r.get<uint32_t>(c.data);
	g.unityNote = r.get<uint16_t>(c.data + 4);
	g.fineTuneCents = r.get<int16_t>(c.data + 6);
	g.gainDb = r.get<int32_t>(c.data + 8) / 655360.0;
	const uint32_t loops = r.get<uint32_t>(c.data + 16);
	g.loop = loops > 0;
	if (g.loop)
	{
		g.loopStart = r.get<uint32_t>(c.data + cb + 8);
		g.loopLen = r.get<uint32_t>(c.data + cb + 12);
	}
}
} // namespace

bool DlsFile::load(const std::string& path)
{
	Reader r;
	FILE* f = fopen(path.c_str(), "rb");
	if (!f)
		return false;
	uint8_t tmp[65536];
	for (size_t n; (n = fread(tmp, 1, sizeof tmp, f)) > 0;)
		r.b.insert(r.b.end(), tmp, tmp + n);
	fclose(f);

	const auto top = r.children(0, r.b.size());
	if (top.empty() || !is(top[0], "RIFF", "DLS "))
		return false;
	const Chunk& root = top[0];
	const auto kids = r.children(root.data, root.data + root.size);

	std::vector<uint32_t> cues;
	for (auto& k : kids)
		if (is(k, "ptbl"))
		{
			const uint32_t cb = r.get<uint32_t>(k.data), n = r.get<uint32_t>(k.data + 4);
			for (uint32_t i = 0; i < n; ++i)
				cues.push_back(r.get<uint32_t>(k.data + cb + i * 4));
		}

	// wave pool: map each cue offset to a wave
	std::map<size_t, int> byOffset;
	for (auto& k : kids)
		if (is(k, "LIST", "wvpl"))
			for (auto& w : r.children(k.data, k.data + k.size))
			{
				if (!is(w, "LIST", "wave"))
					continue;
				DlsWave wave;
				int bits = 16, ch = 1;
				size_t dataPos = 0, dataSize = 0;
				for (auto& wc : r.children(w.data, w.data + w.size))
				{
					if (is(wc, "fmt "))
					{
						ch = r.get<uint16_t>(wc.data + 2);
						wave.rate = r.get<uint32_t>(wc.data + 4);
						bits = r.get<uint16_t>(wc.data + 14);
					}
					else if (is(wc, "data"))
						dataPos = wc.data, dataSize = wc.size;
				}
				const size_t bytes = bits / 8, frames = dataSize / (bytes * ch);
				wave.pcm.resize(frames);
				for (size_t i = 0; i < frames; ++i)
				{
					const size_t p = dataPos + i * bytes * ch;
					wave.pcm[i] = bits == 16 ? r.get<int16_t>(p) / 32768.f : (r.get<uint8_t>(p) - 128) / 128.f;
				}
				byOffset[w.hdr - k.data] = static_cast<int>(waves.size());
				waves.push_back(std::move(wave));
			}

	auto waveForCue = [&](uint32_t cue) -> int {
		if (cue >= cues.size())
			return -1;
		auto it = byOffset.find(cues[cue]);
		return it != byOffset.end() ? it->second : (cue < waves.size() ? static_cast<int>(cue) : -1);
	};

	for (auto& k : kids)
	{
		if (!is(k, "LIST", "lins"))
			continue;
		for (auto& ins : r.children(k.data, k.data + k.size))
		{
			if (!is(ins, "LIST", "ins "))
				continue;
			DlsInstrument inst;
			DlsRegion base; // instrument-level articulation
			const auto parts = r.children(ins.data, ins.data + ins.size);
			for (auto& p : parts)
			{
				if (is(p, "insh"))
				{
					const uint32_t bank = r.get<uint32_t>(p.data + 4);
					inst.program = r.get<uint32_t>(p.data + 8) & 0x7F;
					inst.bank = (bank >> 8) & 0x7F;
					inst.drum = (bank & 0x80000000u) != 0;
				}
				else if (is(p, "LIST", "INFO"))
				{
					for (auto& ic : r.children(p.data, p.data + p.size))
						if (is(ic, "INAM"))
							inst.name = std::string(reinterpret_cast<const char*>(&r.b[ic.data]), strnlen(reinterpret_cast<const char*>(&r.b[ic.data]), ic.size));
				}
				else if (is(p, "LIST", "lart"))
				{
					for (auto& a : r.children(p.data, p.data + p.size))
						if (is(a, "art1"))
							applyArt(r, a, base);
				}
			}
			for (auto& p : parts)
			{
				if (!is(p, "LIST", "lrgn"))
					continue;
				for (auto& rg : r.children(p.data, p.data + p.size))
				{
					if (!is(rg, "LIST", "rgn ") && !is(rg, "LIST", "rgn2"))
						continue;
					DlsRegion reg = base;
					for (auto& c : r.children(rg.data, rg.data + rg.size))
					{
						if (is(c, "rgnh"))
						{
							reg.keyLo = r.get<uint16_t>(c.data); reg.keyHi = r.get<uint16_t>(c.data + 2);
							reg.velLo = r.get<uint16_t>(c.data + 4); reg.velHi = r.get<uint16_t>(c.data + 6);
						}
						else if (is(c, "wlnk"))
							reg.wave = waveForCue(r.get<uint32_t>(c.data + 8));
						else if (is(c, "wsmp"))
							applyWsmp(r, c, reg);
						else if (is(c, "LIST", "lart"))
							for (auto& a : r.children(c.data, c.data + c.size))
								if (is(a, "art1"))
									applyArt(r, a, reg);
					}
					if (reg.wave >= 0)
						inst.regions.push_back(reg);
				}
			}
			instruments.push_back(std::move(inst));
		}
	}
	return !instruments.empty();
}

const DlsInstrument* DlsFile::find(int program, bool drum) const
{
	for (auto& i : instruments)
		if (static_cast<int>(i.program) == program && i.drum == drum && i.bank == 0)
			return &i;
	return nullptr;
}
