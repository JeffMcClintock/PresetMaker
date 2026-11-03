#include "Fit.h"
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <format>
#include <random>
#include "Cma.h"

namespace
{
bool skipped(const VstParam& p, const FitOptions& o)
{
	for (auto& s : o.skip)
		if (p.title.find(s) != std::string::npos) return true;
	return false;
}

std::string scoreLine(const Score& s)
{
	return std::format("similarity {:.1f}%  total {:.4f}  (fine {:.4f} tonal {:.4f} envelope {:.4f} delta {:.4f} level {:.4f} | attack-only {:.4f})  gain {:+.1f} dB",
	                   s.similarity(), s.total, s.fine, s.tonal, s.envelope, s.delta, s.level, s.attack, s.gainDb);
}
} // namespace

std::vector<size_t> freeParams(const Vst3Plugin& vst, const FitOptions& o)
{
	std::vector<size_t> free;
	for (size_t i = 0; i < vst.params().size(); ++i)
	{
		auto& p = vst.params()[i];
		if (p.tweakable && !o.fixed.count(p.title) && !o.fixedById.count(p.id) && !skipped(p, o)) free.push_back(i);
	}
	return free;
}

void applyFixed(Vst3Plugin& vst, const FitOptions& o)
{
	for (size_t k = 0; k < vst.params().size(); ++k)
	{
		if (auto it = o.fixed.find(vst.params()[k].title); it != o.fixed.end()) vst.values[k] = it->second;
		if (auto it = o.fixedById.find(vst.params()[k].id); it != o.fixedById.end()) vst.values[k] = it->second;
	}
}

bool loadPresetTxt(Vst3Plugin& vst, const std::string& path, const FitOptions* skipFixedOf)
{
	FILE* pf = fopen(path.c_str(), "r");
	if (!pf) return false;
	for (char line[512]; fgets(line, sizeof line, pf);)
	{
		unsigned id; double v;
		if (sscanf(line, "%u\t%lf", &id, &v) != 2) continue;
		for (size_t k = 0; k < vst.params().size(); ++k)
			if (vst.params()[k].id == id && !(skipFixedOf && (skipFixedOf->fixed.count(vst.params()[k].title) || skipFixedOf->fixedById.count(id)))) vst.values[k] = v;
	}
	fclose(pf);
	return true;
}

bool savePresetTxt(const Vst3Plugin& vst, const std::string& path)
{
	FILE* pf = fopen(path.c_str(), "w");
	if (!pf) return false;
	for (size_t i = 0; i < vst.params().size(); ++i)
		fprintf(pf, "%u\t%.6f\t%s\n", vst.params()[i].id, vst.values[i], vst.params()[i].title.c_str());
	fclose(pf);
	return true;
}

FitResult runFit(const FitOptions& o, ScoreWeights weights, std::function<bool(const FitProgress&)> progress, std::function<void(const std::string&)> log)
{
	FitResult r;
	std::string err;
	auto ref = openReference(o.reference, err);
	if (!ref) { r.error = err; return r; }
	Vst3Plugin vst;
	if (!vst.load(o.plugin, err)) { r.error = "load failed: " + err; return r; }
	vst.preRollSec = o.preRollSec;
	applyFixed(vst, o);

	std::filesystem::create_directories(o.outDir);
	const int sr = o.sampleRate;
	std::vector<NoteSpec> notes;
	std::vector<Scorer> scorers;
	for (int k : o.keys)
	{
		NoteSpec n;
		n.key = k; n.velocity = o.velocity; n.holdSec = o.holdSec; n.tailSec = o.tailSec;
		Audio a;
		if (!ref->render(n, sr, a)) { r.error = "reference has no note at key " + std::to_string(k); return r; }
		writeWav(o.outDir + "/ref_" + std::to_string(k) + ".wav", a);
		notes.push_back(n);
		scorers.emplace_back(a, weights);
	}

	const auto free = freeParams(vst, o);
	log(std::format("fitting {} free parameters of {} to {}, budget {} evaluations", free.size(), vst.name(), ref->describe(), o.budget));
	if (free.empty()) { r.error = "no free parameters"; return r; }

	auto quantise = [&](size_t pi, double v) {
		const int steps = vst.params()[pi].steps;
		return steps > 0 ? std::round(v * steps) / steps : v;
	};
	auto apply = [&](const std::vector<double>& x) {
		for (size_t j = 0; j < free.size(); ++j) vst.values[free[j]] = quantise(free[j], x[j]);
	};
	bool cancelled = false;
	auto objective = [&](const std::vector<double>& x) {
		if (cancelled) return 1e9;
		apply(x);
		double tot = 0;
		for (size_t i = 0; i < notes.size(); ++i)
		{
			Audio a;
			if (!vst.render(notes[i], sr, a)) return 1e9;
			tot += scorers[i].compare(a).total;
		}
		return tot / notes.size();
	};

	const bool warm = !o.start.empty();
	if (warm && !loadPresetTxt(vst, o.start, &o)) { r.error = "cannot read start preset " + o.start; return r; }
	std::vector<double> start;
	for (size_t i : free) start.push_back(vst.values[i]);

	// seed: best of a random sample (cheap global look), then CMA from there. A warm start skips
	// the sample and begins with a small step.
	const double sigma0 = o.sigma > 0 ? o.sigma : (warm ? 0.05 : 0.25);
	std::mt19937 rng(o.seed);
	std::uniform_real_distribution<double> u;
	double bestF = objective(start);
	std::vector<double> bestX = start;
	int used = 1;
	log(std::format("{}: {:.4f}", warm ? "start preset" : "defaults", bestF));
	const auto t0 = std::chrono::steady_clock::now();
	auto report = [&](double sigma) {
		FitProgress p;
		p.evals = used; p.budget = o.budget; p.best = bestF; p.sigma = sigma;
		p.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		if (!progress(p)) cancelled = true;
	};
	for (int i = 0; i < (warm ? 0 : o.budget / 10) && !cancelled; ++i, ++used)
	{
		std::vector<double> x(free.size());
		for (auto& v : x) v = u(rng);
		const double f = objective(x);
		if (f < bestF) { bestF = f; bestX = x; }
		if (i % 10 == 9) report(sigma0);
	}
	if (!warm) log(std::format("after random seeding ({} evals): {:.4f}", used, bestF));

	SepCma cma(bestX, sigma0);
	while (used < o.budget && !cancelled)
	{
		used += cma.step(objective);
		if (cma.bestF() < bestF) { bestF = cma.bestF(); bestX = cma.best(); }
		report(cma.sigma());
		log(std::format("evals {:5}  best {:.4f}  sigma {:.3f}  ({:.0f}s)", used, bestF, cma.sigma(),
		                std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count()));
	}

	apply(bestX);
	r.trainBest = bestF;
	r.presetTxt = o.outDir + "/preset.txt";
	savePresetTxt(vst, r.presetTxt);
	for (size_t i = 0; i < notes.size(); ++i)
	{
		Audio a;
		vst.render(notes[i], sr, a);
		writeWav(o.outDir + "/fit_" + std::to_string(notes[i].key) + ".wav", a);
		const Score s = scorers[i].compare(a);
		r.perKey.emplace_back(notes[i].key, s);
		log(std::format("key {}: {}", notes[i].key, scoreLine(s)));
	}
	if (!o.name.empty())
	{
		const std::string dir = o.presetDir.empty() ? vst.defaultPresetDir() : o.presetDir;
		std::filesystem::create_directories(dir);
		r.vstPreset = dir + "/" + o.name + ".vstpreset";
		if (vst.savePreset(r.vstPreset, err)) log("wrote " + r.vstPreset);
		else { log("vstpreset failed: " + err); r.vstPreset.clear(); }
	}
	r.ok = !cancelled;
	if (cancelled) r.error = "cancelled";
	return r;
}

double checkPreset(const FitOptions& o, ScoreWeights weights, const std::string& presetTxt, std::function<void(const std::string&)> log)
{
	std::string err;
	auto ref = openReference(o.reference, err);
	if (!ref) { log(err); return 0; }
	Vst3Plugin vst;
	if (!vst.load(o.plugin, err)) { log("load failed: " + err); return 0; }
	vst.preRollSec = o.preRollSec;
	applyFixed(vst, o);
	if (!loadPresetTxt(vst, presetTxt)) { log("cannot read " + presetTxt); return 0; }
	const int checks[][2] = {{43, 100}, {55, 100}, {67, 100}, {79, 100}, {60, 60}, {60, 120}};
	double sum = 0;
	int n = 0;
	for (auto& c : checks)
	{
		NoteSpec note;
		note.key = c[0]; note.velocity = c[1]; note.holdSec = o.holdSec; note.tailSec = o.tailSec;
		Audio refA, a;
		if (!ref->render(note, o.sampleRate, refA) || !vst.render(note, o.sampleRate, a)) { log(std::format("key {} vel {}: no render", c[0], c[1])); continue; }
		const Score s = Scorer(refA, weights).compare(a);
		sum += s.similarity();
		++n;
		log(std::format("held-out key {} vel {}: {:.1f}%  (attack-only {:.3f})", c[0], c[1], s.similarity(), s.attack));
	}
	const double mean = n ? sum / n : 0;
	log(std::format("held-out mean similarity {:.1f}%", mean));
	return mean;
}
