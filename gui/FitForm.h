#pragma once
// The PresetMaker app's one form, two tabs: Fit (plugin, target, name, a few options, the Fit
// button and progress) and Parameters (the plugin's parameters, each with an Ignore tick that
// holds it at its current value). Built from gmpi_ui's widget set the way
// gmpi_ui/examples/FormsDemo is.
//
// The fit runs on a worker thread; it reports through a mutex-guarded Status that the form polls
// from its timer and re-renders when it changes.

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "GmpiUiDrawing.h"
#include "experimental/forms.h"
#include "experimental/theme.h"

#include "Fit.h"

class FitForm : public gmpi::ui::Form
{
public:
	FitForm();
	~FitForm();

	void setPlugin(const std::string& path) { s_.plugin = path; refreshStates(); } // before the window shows

	void Body() override;
	gmpi::ReturnCode measure(const gmpi::drawing::Size*, gmpi::drawing::Size*) override { return gmpi::ReturnCode::Ok; }
	gmpi::ReturnCode arrange(const gmpi::drawing::Rect* finalRect) override;
	gmpi::ReturnCode render(gmpi::drawing::api::IDeviceContext* dc) override;
	gmpi::ReturnCode getClipArea(gmpi::drawing::Rect* r) override { *r = bounds_; return gmpi::ReturnCode::Ok; }
	bool onTimer() override;

private:
	// what the user has entered (model), mirrored into the widget States below
	struct Settings
	{
		std::string plugin, sampleFolder, name, outDir;
		bool nameAuto = true; // the name follows the target until the user types one
		int gmProgram = 0, evals = 600;
		double preRoll = 0.15;
	} s_;
	std::vector<std::string> gmNames_; // by program number
	void autoName();

	gmpi_forms::State<std::string> pluginText_, folderText_, nameText_, outDirText_, evalsText_, preRollText_, gmList_;
	gmpi_forms::State<int32_t> gmProgram_, page_;
	gmpi_forms::State<float> progress_;
	void refreshStates();

	// the chosen plugin's parameters, one Ignore tick each; rebuilt in Body() (visuals are gone
	// then, so the old States can die) whenever the plugin path changes
	struct ParamRow { uint32_t id; std::string title; double value; };
	std::vector<ParamRow> params_;
	std::vector<std::unique_ptr<gmpi_forms::State<bool>>> ignore_;
	std::string paramsOf_, paramsNote_;
	void loadParams();
	void fitPage(gmpi::drawing::Rect area);
	void parametersPage(gmpi::drawing::Rect area);

	// worker
	struct Status
	{
		bool running = false, finished = false;
		std::string line, detail;         // progress / outcome, and where the preset went
		float fraction = 0;               // of the whole run: renders, then the held-out check
		std::vector<std::string> results; // the lines that matter once done
		unsigned version = 0;
	};
	std::mutex mx_;
	Status status_;
	std::atomic<bool> cancel_{false};
	std::thread worker_;
	unsigned shownVersion_ = 0;
	void startFit();
	void joinWorker();

	gmpi::drawing::Rect bounds_{};
	bool dirty_ = true;
	gmpi::ui::ThemeMode lastTheme_ = gmpi::ui::ThemeMode::Dark;
	void rebuild() { dirty_ = true; redraw(); }
};
