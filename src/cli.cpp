#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "Fit.h"
#include "Pitch.h"
#include "Platform.h"

namespace
{
constexpr int kSr = 48000;

int usage()
{
	puts("pmcli gm-list\n"
	     "pmcli gm-render <program> <key> <vel> <out.wav>\n"
	     "pmcli vst-params <plugin.vst3>\n"
	     "pmcli vst-render <plugin.vst3> <key> <vel> <out.wav> [\"Title=0..1\" ...]\n"
	     "pmcli score <ref.wav> <cand.wav>\n"
	     "pmcli pitch <note.wav>\n"
	     "pmcli fit <plugin.vst3> <reference> <outdir> [options]\n"
	     "pmcli check <plugin.vst3> <reference> <preset.txt> [options]\n"
	     "pmcli export <plugin.vst3> <preset.txt> <name> [--preset-dir D]\n"
	     "pmcli fit-batch <plugin.vst3> <list.txt> <outdir> [--jobs N] [options]\n"
	     "\n"
	     "<reference>: a GM program number (gm.dls), a folder of <key>_<vel>.wav samples, or one .wav\n"
	     "<list.txt>:  one 'reference<TAB>Preset Name' per line\n"
	     "options: --evals N (1200)  --keys 48,60,72  --vel 100  --hold 1.5  --tail 0.5\n"
	     "         --fix \"Title=v;Title=v\"  --skip \"substr;substr\"  --start preset.txt  --sigma S\n"
	     "         --preroll S (0.15)  --name \"Preset Name\" (writes a .vstpreset)  --preset-dir D\n"
	     "         --no-vstpreset  --delta W (spectral-change weight, 0.3)\n"
	     "         --transpose N (play the target N semitones up; default: whole octaves detected)");
	return 1;
}

std::vector<int> parseInts(const std::string& s)
{
	std::vector<int> r;
	std::stringstream ss(s);
	for (std::string item; std::getline(ss, item, ',');) r.push_back(atoi(item.c_str()));
	return r;
}

void parseFix(const std::string& s, std::map<std::string, double>& fixed)
{
	std::stringstream ss(s);
	for (std::string item; std::getline(ss, item, ';');)
	{
		const auto eq = item.rfind('=');
		if (eq != std::string::npos) fixed[item.substr(0, eq)] = atof(item.c_str() + eq + 1);
	}
}

struct Args
{
	FitOptions o;
	ScoreWeights w;
	int jobs = 4;
	bool noVstPreset = false;
	std::vector<std::string> positional;
};

// Environment variables (the older interface) are defaults; flags override them.
Args parseArgs(int argc, char** argv)
{
	Args a;
	if (const char* v = getenv("PM_PREROLL")) a.o.preRollSec = atof(v);
	if (const char* v = getenv("PM_FIX")) parseFix(v, a.o.fixed);
	if (const char* v = getenv("PM_START")) a.o.start = v;
	if (const char* v = getenv("PM_SIGMA")) a.o.sigma = atof(v);
	if (const char* v = getenv("PM_DELTA")) a.w.wDelta = atof(v);
	for (int i = 2; i < argc; ++i)
	{
		const std::string s = argv[i];
		auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
		if (s == "--evals") a.o.budget = atoi(next().c_str());
		else if (s == "--keys") a.o.keys = parseInts(next());
		else if (s == "--vel") a.o.velocity = atoi(next().c_str());
		else if (s == "--hold") a.o.holdSec = atof(next().c_str());
		else if (s == "--tail") a.o.tailSec = atof(next().c_str());
		else if (s == "--fix") parseFix(next(), a.o.fixed);
		else if (s == "--skip") { std::stringstream ss(next()); for (std::string it; std::getline(ss, it, ';');) a.o.skip.push_back(it); }
		else if (s == "--start") a.o.start = next();
		else if (s == "--sigma") a.o.sigma = atof(next().c_str());
		else if (s == "--preroll") a.o.preRollSec = atof(next().c_str());
		else if (s == "--name") a.o.name = next();
		else if (s == "--preset-dir") a.o.presetDir = next();
		else if (s == "--no-vstpreset") a.noVstPreset = true;
		else if (s == "--delta") a.w.wDelta = atof(next().c_str());
		else if (s == "--jobs") a.jobs = atoi(next().c_str());
		else if (s == "--seed") a.o.seed = atoi(next().c_str());
		else if (s == "--transpose") { a.o.transpose = atoi(next().c_str()); a.o.autoTranspose = false; }
		else a.positional.push_back(s);
	}
	return a;
}

void print(const std::string& line) { puts(line.c_str()); fflush(stdout); }

std::string quote(const std::string& s) { return "\"" + s + "\""; }

// Runs one child pmcli per list line, `jobs` at a time, each with its own log in outDir.
int fitBatch(const Args& a, const std::string& plugin, const std::string& list, const std::string& outDir, int argc, char** argv)
{
	std::ifstream in(list);
	if (!in) { print("cannot read " + list); return 2; }
	std::filesystem::create_directories(outDir);
	const std::string exe = platform::executablePath();

	// options pass through untouched; the batch-only ones are dropped
	std::string passthrough;
	for (int i = 5; i < argc; ++i)
	{
		const std::string s = argv[i];
		if (s == "--jobs") { ++i; continue; }
		passthrough += " " + quote(s);
	}

	struct Job { std::string ref, name, dir; std::unique_ptr<platform::Process> proc; bool done = false; };
	std::vector<Job> jobs;
	for (std::string line; std::getline(in, line);)
	{
		if (!line.empty() && line.back() == '\r') line.pop_back();
		const auto tab = line.find('\t');
		if (line.empty() || line[0] == '#' || tab == std::string::npos) continue;
		Job j;
		j.ref = line.substr(0, tab);
		j.name = line.substr(tab + 1);
		std::string safe = j.name;
		for (auto& c : safe) if (strchr("\\/:*?\"<>| ", c)) c = '_';
		j.dir = outDir + "/" + safe;
		jobs.push_back(std::move(j));
	}
	print(std::format("{} fits, {} at a time", jobs.size(), a.jobs));

	size_t next = 0, running = 0, finished = 0;
	while (finished < jobs.size())
	{
		while (next < jobs.size() && running < static_cast<size_t>(a.jobs))
		{
			Job& j = jobs[next++];
			std::filesystem::create_directories(j.dir);
			std::string cmd = quote(exe) + " fit " + quote(plugin) + " " + quote(j.ref) + " " + quote(j.dir) + passthrough + " --name " + quote(j.name);
			cmd += " --check";
			j.proc = platform::Process::start(cmd, j.dir + "/fit.log");
			if (!j.proc)
			{
				print("failed to start " + j.name);
				j.done = true; ++finished;
				continue;
			}
			print("started " + j.name);
			++running;
		}
		std::vector<platform::Process*> live;
		for (auto& j : jobs) if (j.proc && !j.done) live.push_back(j.proc.get());
		if (live.empty()) break;
		int code = 0;
		platform::Process* const ended = live[platform::Process::waitAny(live, code)];
		for (auto& j : jobs)
			if (!j.done && j.proc.get() == ended)
			{
				j.proc.reset();
				j.done = true; --running; ++finished;
				// the last "held-out mean" / "key" lines of the log summarise it
				std::ifstream lg(j.dir + "/fit.log");
				std::string summary;
				for (std::string l; std::getline(lg, l);)
					if (l.rfind("held-out mean", 0) == 0) summary = l;
				print(std::format("finished {} (exit {}) {}", j.name, code, summary));
			}
	}
	return 0;
}
} // namespace

int main(int argc, char** argv)
{
	platform::ThreadRuntime runtime;
	if (argc < 2) return usage();
	const std::string cmd = argv[1];
	Args a = parseArgs(argc, argv);
	const auto& pos = a.positional;
	bool doCheck = false;
	for (int i = 2; i < argc; ++i) if (std::string(argv[i]) == "--check") doCheck = true;
	std::erase(a.positional, std::string("--check"));

	if (cmd == "gm-list")
	{
		DlsFile dls;
		if (!dls.load(platform::gmSoundSetPath())) { puts("cannot read gm.dls"); return 2; }
		for (auto& i : dls.instruments)
			printf("%s prog %3u bank %u  %-28s %zu regions\n", i.drum ? "drum " : "pitch", i.program, i.bank, i.name.c_str(), i.regions.size());
		return 0;
	}
	if (cmd == "gm-render" && pos.size() >= 4)
	{
		std::string err;
		auto ref = openReference(pos[0], err);
		if (!ref) { print(err); return 2; }
		NoteSpec n;
		n.key = atoi(pos[1].c_str()); n.velocity = atoi(pos[2].c_str());
		n.holdSec = a.o.holdSec; n.tailSec = a.o.tailSec;
		Audio out;
		if (!ref->render(n, kSr, out)) { puts("no region for that note"); return 2; }
		writeWav(pos[3], out);
		return 0;
	}
	if (cmd == "score" && pos.size() >= 2)
	{
		Audio r, c;
		if (!readWav(pos[0], r) || !readWav(pos[1], c)) { puts("wav read failed"); return 2; }
		const Score s = Scorer(r, a.w).compare(c);
		printf("similarity %.1f%%  total %.4f  (fine %.4f tonal %.4f envelope %.4f delta %.4f level %.4f | attack-only %.4f)  gain %+.1f dB\n",
		       s.similarity(), s.total, s.fine, s.tonal, s.envelope, s.delta, s.level, s.attack, s.gainDb);
		return 0;
	}
	if (pos.empty()) return usage();
	a.o.plugin = pos[0];

	if (cmd == "fit-batch" && pos.size() >= 3) return fitBatch(a, pos[0], pos[1], pos[2], argc, argv);

	if (cmd == "fit" && pos.size() >= 3)
	{
		a.o.reference = pos[1];
		a.o.outDir = pos[2];
		if (a.noVstPreset) a.o.name.clear();
		const FitResult r = runFit(a.o, a.w, [](const FitProgress&) { return true; }, print);
		if (!r.ok) { print(r.error); return 2; }
		if (doCheck) checkPreset(a.o, a.w, r.presetTxt, print);
		return 0;
	}
	if (cmd == "pitch" && pos.size() >= 1)
	{
		Audio a;
		if (!readWav(pos[0], a)) { print("cannot read " + pos[0]); return 2; }
		const PitchEstimate p = estimatePitch(a);
		print(std::format("{:.2f} Hz  midi {:.2f}  confidence {:.2f}", p.hz, p.hz > 0 ? 69 + 12 * std::log2(p.hz / 440) : 0.0, p.confidence));
		return 0;
	}
	if (cmd == "check" && pos.size() >= 3)
	{
		a.o.reference = pos[1];
		checkPreset(a.o, a.w, pos[2], print);
		return 0;
	}

	Vst3Plugin vst;
	std::string err;
	if (!vst.load(a.o.plugin, err)) { print("load failed: " + err); return 2; }
	vst.preRollSec = a.o.preRollSec;
	applyFixed(vst, a.o);

	if (cmd == "vst-params")
	{
		printf("%s by %s: %zu parameters\n%s", vst.name().c_str(), vst.vendor().c_str(), vst.params().size(), vst.describeBuses().c_str());
		const auto free = freeParams(vst, a.o);
		for (size_t i = 0; i < vst.params().size(); ++i)
		{
			auto& p = vst.params()[i];
			const bool isFree = std::find(free.begin(), free.end(), i) != free.end();
			printf("%4zu id %-10u %-32s steps %-3d value %.3f %s\n", i, p.id, p.title.c_str(), p.steps, vst.values[i], isFree ? "" : (p.tweakable ? "(held)" : "(not automatable)"));
		}
		printf("presets go to %s\n", vst.defaultPresetDir().c_str());
		return 0;
	}
	if (cmd == "vst-render" && pos.size() >= 4)
	{
		NoteSpec n;
		n.key = atoi(pos[1].c_str()); n.velocity = atoi(pos[2].c_str());
		n.holdSec = a.o.holdSec; n.tailSec = a.o.tailSec;
		for (size_t i = 4; i < pos.size(); ++i) // optional "Parameter Title=0..1" overrides
		{
			const auto eq = pos[i].rfind('=');
			for (size_t k = 0; eq != std::string::npos && k < vst.params().size(); ++k)
				if (vst.params()[k].title == pos[i].substr(0, eq)) vst.values[k] = atof(pos[i].c_str() + eq + 1);
		}
		Audio out;
		if (!vst.render(n, kSr, out)) { puts("render failed"); return 2; }
		writeWav(pos[3], out);
		return 0;
	}
	if (cmd == "export" && pos.size() >= 3)
	{
		if (!loadPresetTxt(vst, pos[1])) { print("cannot read " + pos[1]); return 2; }
		const std::string dir = a.o.presetDir.empty() ? vst.defaultPresetDir() : a.o.presetDir;
		std::filesystem::create_directories(dir);
		const std::string path = dir + "\\" + pos[2] + ".vstpreset";
		if (!vst.savePreset(path, err)) { print(err); return 2; }
		print("wrote " + path);
		return 0;
	}
	return usage();
}
