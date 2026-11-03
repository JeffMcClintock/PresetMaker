#pragma once
// The one page of the PresetMaker app: plugin, target, name, a few options, a Fit button and a
// status line. Built from gmpi_ui's widget set the way gmpi_ui/examples/FormsDemo is.
//
// The fit runs on a worker thread; it reports through a mutex-guarded Status that the form polls
// from its timer and re-renders when it changes.

#include <atomic>
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
		std::string plugin, sampleFolder, name, fixed = "Volume=0.3", outDir;
		int gmProgram = 0, evals = 600;
		double preRoll = 0.15;
	} s_;

	gmpi_forms::State<std::string> pluginText_, folderText_, nameText_, fixedText_, outDirText_, evalsText_, preRollText_, gmList_;
	gmpi_forms::State<int32_t> gmProgram_;
	void refreshStates();

	// worker
	struct Status
	{
		bool running = false, finished = false;
		std::string line, detail;         // progress / outcome, and where the preset went
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
