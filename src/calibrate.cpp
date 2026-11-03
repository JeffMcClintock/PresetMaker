// Calibration of the perceptual score against things whose answer is known:
//  retrieval   - for each GM program, do the nearest others by score share its family?
//  degrade     - does the score fall monotonically as a note is damaged by known amounts?
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <future>
#include <map>
#include <string>

#include "Dls.h"
#include "GmRender.h"
#include "Metrics.h"

namespace
{
const char* kDls = "C:\\Windows\\System32\\drivers\\gm.dls";
constexpr int kSr = 48000;

Audio resample(const Audio& a, double ratio) // ratio > 1 raises pitch
{
	Audio o;
	o.sampleRate = a.sampleRate;
	o.s.resize(size_t(a.s.size() / ratio));
	for (size_t i = 0; i < o.s.size(); ++i)
	{
		const double p = i * ratio;
		const size_t k = size_t(p);
		const float t = float(p - k);
		const float y0 = a.s[k], y1 = k + 1 < a.s.size() ? a.s[k + 1] : 0.f;
		o.s[i] = y0 + (y1 - y0) * t;
	}
	return o;
}

Audio lowpass(const Audio& a, double hz)
{
	Audio o = a;
	const float c = float(1 - std::exp(-2 * 3.14159265 * hz / a.sampleRate));
	float y1 = 0, y2 = 0;
	for (auto& v : o.s) { y1 += c * (v - y1); y2 += c * (y1 - y2); v = y2; }
	return o;
}

Audio addNoise(const Audio& a, double snrDb)
{
	double e = 0;
	for (float v : a.s) e += double(v) * v;
	const double rms = std::sqrt(e / a.s.size()), nrms = rms * std::pow(10, -snrDb / 20);
	Audio o = a;
	unsigned s = 12345;
	for (auto& v : o.s)
	{
		s = s * 1664525u + 1013904223u;
		v += float((double(s >> 8) / 8388608.0 - 1.0) * nrms * 1.7320508);
	}
	return o;
}

Audio softAttack(const Audio& a, double ms) // fade in over ms, from the first audible sample
{
	Audio o = a;
	size_t i0 = 0;
	float pk = 0;
	for (float v : a.s) pk = std::max(pk, std::fabs(v));
	while (i0 < a.s.size() && std::fabs(a.s[i0]) < pk * 1e-3f) ++i0;
	const size_t n = size_t(ms * 1e-3 * a.sampleRate);
	for (size_t i = 0; i < n && i0 + i < o.s.size(); ++i)
		o.s[i0 + i] *= float(i) / n;
	return o;
}

Audio cutTail(const Audio& a, double keepSec)
{
	Audio o = a;
	const size_t n = size_t(keepSec * a.sampleRate);
	for (size_t i = n; i < o.s.size(); ++i) o.s[i] = 0;
	return o;
}

struct Opt
{
	ScoreWeights w;
	std::vector<int> keys{60};
	int progStep = 1;
	std::string mode = "all";
};

Opt parse(int argc, char** argv)
{
	Opt o;
	for (int i = 1; i < argc; ++i)
	{
		const std::string a = argv[i];
		const auto eq = a.find('=');
		if (eq == std::string::npos) { o.mode = a; continue; }
		const std::string k = a.substr(0, eq);
		const double v = atof(a.c_str() + eq + 1);
		if (k == "tau") o.w.attackTauSec = v;
		else if (k == "share") o.w.attackShare = v;
		else if (k == "fine") o.w.wFine = v;
		else if (k == "tonal") o.w.wTonal = v;
		else if (k == "env") o.w.wEnvelope = v;
		else if (k == "spl") o.w.peakSpl = v;
		else if (k == "mask") o.w.maskSpread = v;
		else if (k == "delta") o.w.wDelta = v;
		else if (k == "step") o.progStep = int(v);
		else if (k == "key") o.keys = {int(v)};
		else printf("unknown option %s\n", k.c_str());
	}
	return o;
}

void retrieval(const DlsFile& dls, const Opt& o)
{
	std::vector<int> progs;
	for (int p = 0; p < 128; p += o.progStep)
		if (dls.find(p)) progs.push_back(p);

	for (int key : o.keys)
	{
		std::vector<Audio> notes(progs.size());
		std::vector<bool> ok(progs.size(), false);
		for (size_t i = 0; i < progs.size(); ++i)
		{
			NoteSpec n;
			n.program = progs[i]; n.key = key; n.velocity = 100; n.holdSec = 1.5; n.tailSec = 0.5;
			ok[i] = renderGmNote(dls, n, kSr, notes[i]);
		}
		std::vector<std::future<std::vector<double>>> jobs;
		for (size_t q = 0; q < progs.size(); ++q)
			jobs.push_back(std::async(std::launch::async, [&, q] {
				std::vector<double> d(progs.size(), 1e9);
				if (!ok[q]) return d;
				Scorer sc(notes[q], o.w);
				for (size_t c = 0; c < progs.size(); ++c)
					if (c != q && ok[c]) d[c] = sc.compare(notes[c]).total;
				return d;
			}));

		int queries = 0, top1 = 0, top3 = 0;
		double rankSum = 0, chance = 0;
		std::map<int, std::pair<int, int>> byFamily; // family -> (top1 hits, queries)
		for (size_t q = 0; q < progs.size(); ++q)
		{
			auto d = jobs[q].get();
			if (!ok[q]) continue;
			std::vector<size_t> idx;
			for (size_t c = 0; c < progs.size(); ++c)
				if (c != q && ok[c]) idx.push_back(c);
			std::sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return d[a] < d[b]; });
			const int fam = progs[q] / 8;
			int same = 0;
			for (auto c : idx) same += progs[c] / 8 == fam;
			++queries;
			const bool hit1 = progs[idx[0]] / 8 == fam;
			top1 += hit1;
			for (int r = 0; r < 3 && r < int(idx.size()); ++r)
				if (progs[idx[r]] / 8 == fam) { ++top3; break; }
			// mean normalised rank of the same-family members (0 = all first, 0.5 = random)
			double rs = 0; int seen = 0;
			for (size_t r = 0; r < idx.size(); ++r)
				if (progs[idx[r]] / 8 == fam) { rs += double(r) / idx.size(); ++seen; }
			if (seen) rankSum += rs / seen;
			chance += double(same) / idx.size();
			byFamily[fam].first += hit1;
			byFamily[fam].second += 1;
		}
		printf("key %d: %d queries | top-1 same family %.1f%% (chance %.1f%%) | top-3 %.1f%% | mean same-family rank %.3f (random 0.500)\n",
		       key, queries, 100.0 * top1 / queries, 100.0 * chance / queries, 100.0 * top3 / queries, rankSum / queries);
		static const char* names[] = {"Piano", "ChromPerc", "Organ", "Guitar", "Bass", "Strings", "Ensemble", "Brass", "Reed", "Pipe", "SynLead", "SynPad", "SynFX", "Ethnic", "Percuss", "SoundFX"};
		printf("  per family top-1:");
		for (auto& [f, v] : byFamily) printf(" %s %d/%d", names[f], v.first, v.second);
		printf("\n");
	}
}

// Same instrument at a different velocity (a different sample layer, so not a copy) must be the
// nearest of all 128 programs. The harder and more honest test than family labels.
void identity(const DlsFile& dls, const Opt& o)
{
	std::vector<int> progs;
	for (int p = 0; p < 128; p += o.progStep)
		if (dls.find(p)) progs.push_back(p);
	for (int key : o.keys)
	{
		std::vector<Audio> q(progs.size()), c(progs.size());
		std::vector<bool> ok(progs.size(), false);
		for (size_t i = 0; i < progs.size(); ++i)
		{
			NoteSpec n;
			n.program = progs[i]; n.key = key; n.holdSec = 1.5; n.tailSec = 0.5;
			n.velocity = 110; const bool a = renderGmNote(dls, n, kSr, q[i]);
			n.velocity = 60;  const bool b = renderGmNote(dls, n, kSr, c[i]);
			ok[i] = a && b;
		}
		std::vector<std::future<std::vector<double>>> jobs;
		for (size_t i = 0; i < progs.size(); ++i)
			jobs.push_back(std::async(std::launch::async, [&, i] {
				std::vector<double> d(progs.size(), 1e9);
				if (!ok[i]) return d;
				Scorer sc(q[i], o.w);
				for (size_t j = 0; j < progs.size(); ++j)
					if (ok[j]) d[j] = sc.compare(c[j]).total;
				return d;
			}));
		int n = 0, top1 = 0, top5 = 0;
		double rankSum = 0;
		std::string misses;
		for (size_t i = 0; i < progs.size(); ++i)
		{
			auto d = jobs[i].get();
			if (!ok[i]) continue;
			int rank = 0;
			for (size_t j = 0; j < progs.size(); ++j)
				if (ok[j] && d[j] < d[i]) ++rank;
			++n; top1 += rank == 0; top5 += rank < 5; rankSum += rank;
			if (rank > 0 && misses.size() < 400) misses += " " + std::to_string(progs[i]) + "(r" + std::to_string(rank) + ")";
		}
		printf("key %d identity: top-1 %.1f%%  top-5 %.1f%%  mean rank %.2f of %d\n", key, 100.0 * top1 / n, 100.0 * top5 / n, rankSum / n, n);
		if (o.progStep == 1) printf("  misses:%s\n", misses.c_str());
	}
}

void neighbours(const DlsFile& dls, const Opt& o)
{
	std::vector<int> progs;
	std::vector<Audio> notes;
	for (int p = 0; p < 128; ++p)
	{
		NoteSpec n;
		n.program = p; n.key = o.keys[0]; n.velocity = 100; n.holdSec = 1.5; n.tailSec = 0.5;
		Audio a;
		if (dls.find(p) && renderGmNote(dls, n, kSr, a)) { progs.push_back(p); notes.push_back(a); }
	}
	auto name = [&](int p) { return dls.find(p)->name; };
	for (int q : {24, 25, 26, 27, 32, 33, 45, 46, 104, 105, 106, 107})
	{
		auto it = std::find(progs.begin(), progs.end(), q);
		if (it == progs.end()) continue;
		Scorer sc(notes[it - progs.begin()], o.w);
		std::vector<std::pair<double, int>> d;
		for (size_t c = 0; c < progs.size(); ++c)
			if (progs[c] != q) d.push_back({sc.compare(notes[c]).total, progs[c]});
		std::sort(d.begin(), d.end());
		printf("%3d %-18s ->", q, name(q).c_str());
		for (int r = 0; r < 4; ++r) printf("  %d %s (%.0f%%)", d[r].second, name(d[r].second).c_str(), Score{d[r].first}.similarity());
		printf("\n");
	}
}

void degrade(const DlsFile& dls, const Opt& o)
{
	struct Test { const char* name; std::vector<double> levels; std::function<Audio(const Audio&, double)> f; };
	const std::vector<Test> tests = {
		{"detune cents", {3, 10, 25, 50, 100}, [](const Audio& a, double v) { return resample(a, std::exp2(v / 1200)); }},
		{"lowpass Hz", {12000, 6000, 3000, 1500, 700}, [](const Audio& a, double v) { return lowpass(a, v); }},
		{"noise SNR dB", {50, 40, 30, 20, 10}, [](const Audio& a, double v) { return addNoise(a, v); }},
		{"attack fade ms", {2, 5, 10, 25, 60}, [](const Audio& a, double v) { return softAttack(a, v); }},
		{"tail cut at s", {1.8, 1.4, 1.0, 0.6, 0.3}, [](const Audio& a, double v) { return cutTail(a, v); }},
	};
	const int progs[] = {0, 24, 40, 56, 73, 81};
	int violations = 0, checks = 0;
	for (auto& t : tests)
	{
		printf("%-15s", t.name);
		for (double l : t.levels) printf(" %8g", l);
		printf("\n");
		for (int p : progs)
		{
			NoteSpec n;
			n.program = p; n.key = o.keys[0]; n.velocity = 100; n.holdSec = 1.5; n.tailSec = 0.5;
			Audio ref;
			if (!renderGmNote(dls, n, kSr, ref)) continue;
			Scorer sc(ref, o.w);
			printf("  prog %-3d      ", p);
			double prev = -1;
			for (double l : t.levels)
			{
				const double d = sc.compare(t.f(ref, l)).total;
				printf(" %7.1f%%", Score{d}.similarity());
				++checks;
				if (d < prev - 1e-9) ++violations; // the damage grew, the distance must not shrink
				prev = d;
			}
			printf("\n");
		}
	}
	printf("monotonicity violations: %d of %d steps\n", violations, checks);
}
} // namespace

int main(int argc, char** argv)
{
	const Opt o = parse(argc, argv);
	DlsFile dls;
	if (!dls.load(kDls)) { puts("cannot read gm.dls"); return 2; }
	if (o.mode == "all" || o.mode == "retrieval") retrieval(dls, o);
	if (o.mode == "all" || o.mode == "identity") identity(dls, o);
	if (o.mode == "neighbours") neighbours(dls, o);
	if (o.mode == "all" || o.mode == "degrade") degrade(dls, o);
	return 0;
}
