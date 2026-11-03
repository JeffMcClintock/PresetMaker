#include "FitForm.h"

#include <filesystem>
#include <format>
#include <memory>

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>

#include "experimental/builders.h"
#include "Dls.h"

using namespace gmpi::ui;
using gmpi::ui::builder::fr;
using gmpi::ui::builder::auto_size;
using eAutoFlow = gmpi::ui::builder::ViewParent::eAutoFlow;

namespace
{
constexpr float kRow = 24.0f, kGap = 6.0f, kMargin = 12.0f, kLabelW = 150.0f, kButtonW = 90.0f;

std::string documentsFolder()
{
	wchar_t docs[MAX_PATH]{};
	SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, SHGFP_TYPE_CURRENT, docs);
	char out[MAX_PATH * 3]{};
	WideCharToMultiByte(CP_UTF8, 0, docs, -1, out, sizeof out, nullptr, nullptr);
	return out;
}

// "Acoustic Grand Piano=0,Bright Acoustic Piano=1,..." for the combo box
std::string gmProgramList()
{
	DlsFile dls;
	std::string r;
	if (dls.load("C:\\Windows\\System32\\drivers\\gm.dls"))
		for (auto& i : dls.instruments)
			if (!i.drum && i.bank == 0)
			{
				std::string n = i.name;
				for (auto& c : n) if (c == ',' || c == '=') c = ' ';
				r += (r.empty() ? "" : ",") + std::format("{} {}={}", i.program, n, i.program);
			}
	return r.empty() ? "no gm.dls=0" : r;
}

// A .vst3 may be a bundle folder; a file picked inside it means the bundle root.
std::string bundleRoot(std::string path)
{
	for (auto& c : path) if (c == '/') c = '\\';
	size_t p = 0;
	while ((p = path.find(".vst3", p)) != std::string::npos)
	{
		p += 5;
		if (p == path.size() || path[p] == '\\') return path.substr(0, p);
	}
	return path;
}

void parseFix(const std::string& s, std::map<std::string, double>& fixed)
{
	size_t p = 0;
	while (p <= s.size())
	{
		const size_t e = s.find(';', p);
		const std::string item = s.substr(p, e == std::string::npos ? std::string::npos : e - p);
		const auto eq = item.rfind('=');
		if (eq != std::string::npos) fixed[item.substr(0, eq)] = atof(item.c_str() + eq + 1);
		if (e == std::string::npos) break;
		p = e + 1;
	}
}
} // namespace

FitForm::FitForm()
{
	s_.outDir = documentsFolder() + "\\PresetMaker";
	gmList_ = gmProgramList();
	refreshStates();
}

FitForm::~FitForm()
{
	cancel_ = true;
	joinWorker();
	clear();
}

void FitForm::joinWorker()
{
	if (worker_.joinable()) worker_.join();
}

void FitForm::refreshStates()
{
	pluginText_ = s_.plugin;
	folderText_ = s_.sampleFolder;
	nameText_ = s_.name;
	fixedText_ = s_.fixed;
	outDirText_ = s_.outDir;
	evalsText_ = std::to_string(s_.evals);
	preRollText_ = std::format("{}", s_.preRoll);
	gmProgram_ = s_.gmProgram;
}

gmpi::ReturnCode FitForm::arrange(const gmpi::drawing::Rect* finalRect)
{
	bounds_ = *finalRect;
	dirty_ = true;
	Invalidate({});
	return gmpi::ReturnCode::Ok;
}

gmpi::ReturnCode FitForm::render(gmpi::drawing::api::IDeviceContext* dc)
{
	const auto mode = gmpi::ui::themeModeStorage();
	if (mode != lastTheme_) dirty_ = true;
	if (dirty_)
	{
		dirty_ = false;
		lastTheme_ = mode;
		renderVisuals();
	}
	gmpi::drawing::Graphics g(dc);
	g.clear(gmpi::ui::currentTheme().panelBackground);
	return Form::render(dc);
}

bool FitForm::onTimer()
{
	Form::onTimer();
	unsigned v;
	{
		std::lock_guard lock(mx_);
		v = status_.version;
		if (status_.finished && worker_.joinable()) { worker_.join(); status_.finished = false; }
	}
	if (v != shownVersion_) { shownVersion_ = v; rebuild(); }
	return true;
}

void FitForm::startFit()
{
	{
		std::lock_guard lock(mx_);
		if (status_.running) return;
		status_ = {};
		status_.running = true;
		status_.line = "starting...";
		++status_.version;
	}
	cancel_ = false;
	joinWorker();

	FitOptions o;
	o.plugin = s_.plugin;
	o.reference = s_.sampleFolder.empty() ? std::to_string(s_.gmProgram) : s_.sampleFolder;
	o.name = s_.name;
	o.budget = s_.evals;
	o.preRollSec = s_.preRoll;
	parseFix(s_.fixed, o.fixed);
	std::string safe = s_.name.empty() ? "fit" : s_.name;
	for (auto& c : safe) if (strchr("\\/:*?\"<>|", c)) c = '_';
	o.outDir = s_.outDir + "\\" + safe;

	worker_ = std::thread([this, o] {
		CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
		auto post = [this](auto&& f) { std::lock_guard lock(mx_); f(status_); ++status_.version; };
		auto log = [&](const std::string& l) {
			// per-note scores only; the breakdown after "(fine" is in preset.txt's folder, not on screen
			if (l.rfind("key ", 0) == 0 || l.rfind("held-out key", 0) == 0 || l.rfind("vstpreset failed", 0) == 0)
				post([&](Status& s) { s.results.push_back(l.substr(0, l.find(" (fine"))); });
		};
		const FitResult r = runFit(o, ScoreWeights{}, [&](const FitProgress& p) {
			post([&](Status& s) { s.line = std::format("RUNNING: {} of {} renders, best {:.4f}, step {:.3f}, {:.0f} s", p.evals, p.budget, p.best, p.sigma, p.seconds); });
			return !cancel_.load();
		}, log);
		std::string line, detail;
		if (r.ok)
		{
			post([&](Status& s) { s.line = "RUNNING: checking held-out notes..."; });
			const double mean = checkPreset(o, ScoreWeights{}, r.presetTxt, log);
			line = std::format("FINISHED. Held-out similarity {:.1f}%", mean);
			detail = r.vstPreset.empty() ? "Saved " + r.presetTxt + "  (no .vstpreset: give it a preset name)" : "Saved " + r.vstPreset;
		}
		else if (!r.presetTxt.empty()) { line = "CANCELLED"; detail = "Best so far saved to " + r.presetTxt; }
		else line = "FAILED: " + r.error;
		post([&](Status& s) { s.line = line; s.detail = detail; s.running = false; s.finished = true; });
		CoUninitialize();
	});
}

void FitForm::Body()
{
	gmpi::drawing::Rect area = bounds_;
	area.left += kMargin; area.right -= kMargin; area.top += kMargin; area.bottom -= kMargin;

	Grid outer({ .gap = kGap, .auto_flow = eAutoFlow::rows, .default_track_size = auto_size() }, area);

	Status st;
	{
		std::lock_guard lock(mx_);
		st = status_;
	}
	const bool running = st.running;

	auto beginRow = [&](std::string_view label, std::vector<float> cols = { kLabelW, fr(1.0f) }) {
		auto row = std::make_unique<Grid>(
			gmpi::ui::builder::ViewParent::Initializer{ .gap = kGap, .auto_rows = kRow, .auto_flow = eAutoFlow::columns, .column_widths = std::move(cols) },
			gmpi::drawing::Rect{});
		Label l(label);
		return row;
	};
	auto browse = [&](gmpi_forms::State<std::string>& state, std::function<void(const std::string&)> save) {
		auto b = std::make_unique<gmpi::ui::builder::FileBrowseButtonView>(gmpi::drawing::Rect{});
		b->value.setSource(&state);
		b->validateAndSave = std::move(save);
		gmpi::ui::builder::ThreadLocalCurrentBuilder->push_back(std::move(b));
	};

	{
		auto row = beginRow("Plugin (.vst3)", { kLabelW, fr(1.0f), kRow });
		TextEdit e(pluginText_, [this](const std::string& v) { s_.plugin = bundleRoot(v); refreshStates(); });
		browse(pluginText_, [this](const std::string& v) { s_.plugin = bundleRoot(v); refreshStates(); rebuild(); });
	}
	{
		auto row = beginRow("Target (GM)");
		ComboBox cb({});
		cb.view->enum_list.setSource(&gmList_);
		cb.view->enum_value.setSource(&gmProgram_);
		cb.view->validateAndSave = [this](int32_t id) { s_.gmProgram = id; refreshStates(); };
	}
	{
		auto row = beginRow("Sample folder", { kLabelW, fr(1.0f), kRow });
		TextEdit e(folderText_, [this](const std::string& v) { s_.sampleFolder = v; refreshStates(); });
		browse(folderText_, [this](const std::string& v) {
			// a file picker: take the folder of whatever WAV was chosen
			s_.sampleFolder = std::filesystem::path(v).parent_path().string();
			refreshStates(); rebuild();
		});
	}
	{
		auto row = beginRow("Preset name");
		TextEdit e(nameText_, [this](const std::string& v) { s_.name = v; refreshStates(); });
	}
	{
		auto row = beginRow("Hold", { kLabelW, fr(1.0f) });
		TextEdit e(fixedText_, [this](const std::string& v) { s_.fixed = v; refreshStates(); });
	}
	{
		auto row = beginRow("Renders, pre-roll", { kLabelW, 80.0f, 80.0f, fr(1.0f) });
		TextEdit e1(evalsText_, [this](const std::string& v) { if (atoi(v.c_str()) > 0) s_.evals = atoi(v.c_str()); refreshStates(); });
		TextEdit e2(preRollText_, [this](const std::string& v) { s_.preRoll = atof(v.c_str()); refreshStates(); });
		Label filler("");
	}
	{
		auto row = beginRow("Output folder");
		// (fitted WAVs and preset.txt go here; the .vstpreset goes to the DAW folder)
		TextEdit e(outDirText_, [this](const std::string& v) { s_.outDir = v; refreshStates(); });
	}
	{
		Grid row({ .gap = kGap, .auto_rows = kRow, .auto_flow = eAutoFlow::columns, .column_widths = { kButtonW, kButtonW, kButtonW, fr(1.0f) } }, {});
		Button fit(running ? "Fitting..." : "Fit");
		fit.view->onClick = [this] { if (!s_.plugin.empty()) startFit(); };
		Button cancel("Cancel");
		cancel.view->onClick = [this] { cancel_ = true; };
		Button open("Presets...");
		open.view->onClick = [this] {
			Vst3Plugin vst;
			std::string err;
			const std::string dir = vst.load(s_.plugin, err) ? vst.defaultPresetDir() : documentsFolder() + "\\VST3 Presets";
			std::filesystem::create_directories(dir);
			ShellExecuteA(nullptr, "open", dir.c_str(), nullptr, nullptr, SW_SHOW);
		};
		Label filler("");
	}
	Label hint1("Hold: parameters kept fixed, e.g. Volume=0.3;Pan=0.5", { 0, 0, 0, 14 });
	Label hint2("Sample folder: WAVs named <key>_<vel>.wav, used instead of the GM target", { 0, 0, 0, 14 });
	Spacer sp({ 0, 0, 0, kGap });
	Label status(st.line, { 0, 0, 0, 20 });
	if (!st.detail.empty()) Label detail(st.detail, { 0, 0, 0, 16 });
	for (auto& l : st.results) Label line(l, { 0, 0, 0, 16 });
}
