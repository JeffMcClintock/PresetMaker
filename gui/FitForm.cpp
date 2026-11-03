#include "FitForm.h"

#include <algorithm>
#include <cctype>
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
constexpr float kRow = 24.0f, kGap = 6.0f, kMargin = 12.0f, kLabelW = 150.0f, kButtonW = 90.0f, kTabsH = 32.0f;

std::string documentsFolder()
{
	wchar_t docs[MAX_PATH]{};
	SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, SHGFP_TYPE_CURRENT, docs);
	char out[MAX_PATH * 3]{};
	WideCharToMultiByte(CP_UTF8, 0, docs, -1, out, sizeof out, nullptr, nullptr);
	return out;
}

// "0 Piano 1=0,1 Piano 2=1,..." for the combo box, and the names by program number
std::string gmProgramList(std::vector<std::string>& names)
{
	DlsFile dls;
	std::string r;
	names.assign(128, "");
	if (dls.load("C:\\Windows\\System32\\drivers\\gm.dls"))
		for (auto& i : dls.instruments)
			if (!i.drum && i.bank == 0 && i.program < 128)
			{
				std::string n = i.name;
				for (auto& c : n) if (c == ',' || c == '=') c = ' ';
				names[i.program] = i.name;
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

// Ticked by default: level and tuning controls, which a search should not be allowed to move.
bool ignoredByDefault(std::string title)
{
	for (auto& c : title) c = (char)tolower((unsigned char)c);
	for (const char* w : { "volume", "gain", "output", "master", "level", "fine", "tune" })
		if (title.find(w) != std::string::npos) return true;
	return false;
}
} // namespace

FitForm::FitForm()
{
	s_.outDir = documentsFolder() + "\\PresetMaker";
	gmList_ = gmProgramList(gmNames_);
	autoName();
	refreshStates();
}

// the target's name: the GM instrument, or the sample folder's own name
void FitForm::autoName()
{
	if (!s_.nameAuto) return;
	if (!s_.sampleFolder.empty()) s_.name = std::filesystem::path(s_.sampleFolder).filename().string();
	else if (s_.gmProgram >= 0 && s_.gmProgram < (int)gmNames_.size()) s_.name = gmNames_[s_.gmProgram];
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
	outDirText_ = s_.outDir;
	evalsText_ = std::to_string(s_.evals);
	preRollText_ = std::format("{}", s_.preRoll);
	gmProgram_ = s_.gmProgram;
}

// Only from Body(): the previous visuals are already released, so the old tick States may go.
void FitForm::loadParams()
{
	paramsOf_ = s_.plugin;
	params_.clear();
	ignore_.clear();
	paramsNote_.clear();
	if (s_.plugin.empty()) { paramsNote_ = "Choose a plugin on the Fit tab."; return; }

	Vst3Plugin vst;
	std::string err;
	if (!vst.load(s_.plugin, err)) { paramsNote_ = "Cannot load the plugin: " + err; return; }
	FitOptions defaults;
	for (size_t i = 0; i < vst.params().size(); ++i)
	{
		auto& p = vst.params()[i];
		bool midi = false;
		for (auto& s : defaults.skip) midi |= p.title.find(s) != std::string::npos;
		if (!p.tweakable || midi) continue;
		params_.push_back({ p.id, p.title, vst.values[i] });
		ignore_.push_back(std::make_unique<gmpi_forms::State<bool>>(ignoredByDefault(p.title)));
	}
	if (params_.empty()) paramsNote_ = "The plugin exposes no parameters the search could move.";
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
	for (size_t i = 0; i < params_.size(); ++i)
		if (ignore_[i]->get()) o.fixedById[params_[i].id] = params_[i].value;
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
			post([&](Status& s) {
				s.line = std::format("RUNNING: {} of {} renders, best {:.4f}, step {:.3f}, {:.0f} s", p.evals, p.budget, p.best, p.sigma, p.seconds);
				s.fraction = 0.9f * float(p.evals) / float(std::max(1, p.budget)); // the last tenth is the held-out check
			});
			return !cancel_.load();
		}, log);
		std::string line, detail;
		if (r.ok)
		{
			post([&](Status& s) { s.line = "RUNNING: checking held-out notes..."; s.fraction = 0.9f; });
			const double mean = checkPreset(o, ScoreWeights{}, r.presetTxt, log);
			line = std::format("FINISHED. Held-out similarity {:.1f}%", mean);
			detail = r.vstPreset.empty() ? "Saved " + r.presetTxt + "  (no .vstpreset: give it a preset name)" : "Saved " + r.vstPreset;
		}
		else if (!r.presetTxt.empty()) { line = "CANCELLED"; detail = "Best so far saved to " + r.presetTxt; }
		else line = "FAILED: " + r.error;
		post([&](Status& s) { s.line = line; s.detail = detail; if (r.ok) s.fraction = 1; s.running = false; s.finished = true; });
		CoUninitialize();
	});
}

void FitForm::Body()
{
	if (paramsOf_ != s_.plugin) loadParams();

	gmpi::drawing::Rect area = bounds_;
	area.left += kMargin; area.right -= kMargin; area.top += kMargin; area.bottom -= kMargin;

	{
		TabBar tabs({ "Fit", "Parameters" }, { area.left, area.top, area.right, area.top + kTabsH });
		tabs.view->selected.setSource(&page_);
		tabs.view->validateAndSave = [this](int32_t i) { page_ = i; rebuild(); };
	}
	area.top += kTabsH + kGap;

	if (page_.get() == 1) parametersPage(area);
	else fitPage(area);
}

void FitForm::fitPage(gmpi::drawing::Rect area)
{
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
		TextEdit e(pluginText_, [this](const std::string& v) { s_.plugin = bundleRoot(v); refreshStates(); rebuild(); });
		browse(pluginText_, [this](const std::string& v) { s_.plugin = bundleRoot(v); refreshStates(); rebuild(); });
	}
	{
		auto row = beginRow("Target (GM)");
		ComboBox cb({});
		cb.view->enum_list.setSource(&gmList_);
		cb.view->enum_value.setSource(&gmProgram_);
		cb.view->validateAndSave = [this](int32_t id) { s_.gmProgram = id; autoName(); refreshStates(); };
	}
	{
		auto row = beginRow("Sample folder", { kLabelW, fr(1.0f), kRow });
		TextEdit e(folderText_, [this](const std::string& v) { s_.sampleFolder = v; autoName(); refreshStates(); });
		browse(folderText_, [this](const std::string& v) {
			// a file picker: take the folder of whatever WAV was chosen
			s_.sampleFolder = std::filesystem::path(v).parent_path().string();
			autoName(); refreshStates(); rebuild();
		});
	}
	{
		auto row = beginRow("Preset name");
		TextEdit e(nameText_, [this](const std::string& v) { s_.name = v; s_.nameAuto = v.empty(); autoName(); refreshStates(); });
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
		fit.view->accent = !running;
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
	size_t held = 0;
	for (auto& t : ignore_) held += t->get();
	Label hint1(params_.empty() ? std::string("Sample folder: WAVs named <key>_<vel>.wav, used instead of the GM target")
	                            : std::format("{} of {} parameters searched; the Parameters tab chooses which", params_.size() - held, params_.size()), { 0, 0, 0, 14 });
	Spacer sp({ 0, 0, 0, kGap });
	Label status(st.line, { 0, 0, 0, 20 });
	progress_ = st.fraction;
	ProgressBar bar({ 0, 0, 0, 6 });
	bar.view->fraction.setSource(&progress_);
	if (!st.detail.empty()) Label detail(st.detail, { 0, 0, 0, 16 });
	for (auto& l : st.results) Label line(l, { 0, 0, 0, 16 });
}

void FitForm::parametersPage(gmpi::drawing::Rect area)
{
	{
		Label head(paramsNote_.empty() ? "Ticked parameters are ignored: left at their current value and not searched." : paramsNote_,
		           { area.left, area.top, area.right, area.top + 16 });
	}
	area.top += 16 + kGap;
	if (params_.empty()) return;

	// inside the portal, coordinates are the portal's own; leave room for its scrollbar
	ScrollPortal portal(area);
	Grid list({ .gap = 2.0f, .auto_rows = kRow, .auto_flow = eAutoFlow::rows, .default_track_size = kRow },
	          { 0, 0, area.right - area.left - 20.0f, 0 });
	for (size_t i = 0; i < params_.size(); ++i)
	{
		Grid row({ .gap = kGap, .auto_rows = kRow, .auto_flow = eAutoFlow::columns, .column_widths = { kRow, fr(1.0f), 70.0f } }, {});
		ToggleSwitch tick("", *ignore_[i]);
		tick.view->validateAndSave = [this, i](bool on) { *ignore_[i] = on; };
		Label title(params_[i].title);
		Label value(std::format("{:.2f}", params_[i].value));
	}
}
