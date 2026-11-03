#include "Vst3Host.h"
#include <windows.h>
#include <algorithm>
#include <cmath>

#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "public.sdk/source/vst/hosting/eventlist.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "public.sdk/source/vst/hosting/plugprovider.h"
#include "public.sdk/source/vst/hosting/processdata.h"
#include "public.sdk/source/vst/utility/stringconvert.h"
#include "public.sdk/source/vst/vstpresetfile.h"
#include <shlobj.h>

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace
{
// The plugin may call back into us; accept everything.
struct Handler : IComponentHandler
{
	tresult PLUGIN_API beginEdit(ParamID) override { return kResultOk; }
	tresult PLUGIN_API performEdit(ParamID, ParamValue) override { return kResultOk; }
	tresult PLUGIN_API endEdit(ParamID) override { return kResultOk; }
	tresult PLUGIN_API restartComponent(int32) override { return kResultOk; }
	tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override
	{
		if (FUnknownPrivate::iidEqual(iid, IComponentHandler::iid) || FUnknownPrivate::iidEqual(iid, FUnknown::iid))
		{
			*obj = this;
			return kResultOk;
		}
		*obj = nullptr;
		return kNoInterface;
	}
	uint32 PLUGIN_API addRef() override { return 1; }
	uint32 PLUGIN_API release() override { return 1; }
};

// A Debug-built plugin brings its own debug CRT, whose asserts raise a modal dialog. Send them to stderr.
void quietDebugCrt()
{
	if (HMODULE crt = LoadLibraryA("ucrtbased.dll"))
	{
		using SetMode = int(__cdecl*)(int, int);
		using SetFile = void*(__cdecl*)(int, void*);
		auto setMode = reinterpret_cast<SetMode>(GetProcAddress(crt, "_CrtSetReportMode"));
		auto setFile = reinterpret_cast<SetFile>(GetProcAddress(crt, "_CrtSetReportFile"));
		if (setMode && setFile)
			for (int type = 0; type < 3; ++type) // warn, error, assert
			{
				setMode(type, 0x1);                       // _CRTDBG_MODE_FILE
				setFile(type, reinterpret_cast<void*>(-4)); // _CRTDBG_FILE_STDERR
			}
	}
}

HostApplication& hostApp()
{
	static HostApplication app;
	return app;
}
} // namespace

struct Vst3Plugin::Impl
{
	VST3::Hosting::Module::Ptr module;
	IPtr<PlugProvider> provider;
	IPtr<IComponent> component;
	IPtr<IEditController> controller;
	IPtr<IAudioProcessor> processor;
	Handler handler;
	std::string name, vendor;
	FUID classId;
	std::vector<VstParam> params;
	bool wasActive = false;
};

Vst3Plugin::Vst3Plugin() : p(new Impl) {}

Vst3Plugin::~Vst3Plugin()
{
	if (p->wasActive)
	{
		p->processor->setProcessing(false);
		p->component->setActive(false);
	}
	// Order matters: every reference into the plugin must be gone before its DLL unloads, or its
	// timer clients are still registered at unload (SynthEdit's debug build asserts on that).
	if (p->controller)
		p->controller->setComponentHandler(nullptr);
	p->processor = nullptr;
	p->controller = nullptr;
	p->component = nullptr;
	p->provider = nullptr; // terminates the plugin
	p->module = nullptr;
}

const std::string& Vst3Plugin::name() const { return p->name; }
const std::string& Vst3Plugin::vendor() const { return p->vendor; }

std::string Vst3Plugin::defaultPresetDir() const
{
	wchar_t docs[MAX_PATH]{};
	SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, SHGFP_TYPE_CURRENT, docs);
	std::string r = VST3::StringConvert::convert(std::u16string(reinterpret_cast<const char16_t*>(docs)));
	auto safe = [](std::string s) { for (auto& c : s) if (strchr("\\/:*?\"<>|", c)) c = '_'; return s; };
	return r + "\\VST3 Presets\\" + safe(p->vendor.empty() ? "Unknown" : p->vendor) + "\\" + safe(p->name);
}

bool Vst3Plugin::savePreset(const std::string& path, std::string& error)
{
	// the component only holds the values once they have been through process()
	NoteSpec n;
	n.holdSec = 0.05; n.tailSec = 0;
	Audio a;
	if (!render(n, 48000, a)) { error = "render failed"; return false; }
	IPtr<IBStream> stream = owned(FileStream::open(path.c_str(), "wb"));
	if (!stream) { error = "cannot write " + path; return false; }
	if (!PresetFile::savePreset(stream, p->classId, p->component, p->controller)) { error = "plugin refused to save its state"; return false; }
	return true;
}
const std::vector<VstParam>& Vst3Plugin::params() const { return p->params; }

std::string Vst3Plugin::describeBuses() const
{
	std::string r;
	for (auto [type, tn] : {std::pair{kAudio, "audio"}, std::pair{kEvent, "event"}})
		for (auto dir : {kInput, kOutput})
		{
			const int32 n = p->component->getBusCount(type, dir);
			r += std::string(tn) + (dir == kInput ? " in " : " out ") + std::to_string(n);
			for (int32 i = 0; i < n; ++i)
			{
				BusInfo bi{};
				p->component->getBusInfo(type, dir, i, bi);
				r += " [" + VST3::StringConvert::convert(bi.name) + (bi.flags & BusInfo::kDefaultActive ? "" : " inactive") + " ch" + std::to_string(bi.channelCount) + "]";
			}
			r += "\n";
		}
	return r;
}

bool Vst3Plugin::load(const std::string& path, std::string& error)
{
	quietDebugCrt();
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
	p->module = VST3::Hosting::Module::create(path, error);
	if (!p->module)
		return false;
	PluginContextFactory::instance().setPluginContext(&hostApp());

	auto factory = p->module->getFactory();
	for (auto& ci : factory.classInfos())
	{
		if (ci.category() != kVstAudioEffectClass)
			continue;
		p->provider = owned(new PlugProvider(factory, ci, true));
		if (!p->provider->initialize())
		{
			error = "plugin failed to initialise";
			return false;
		}
		p->name = ci.name();
		p->vendor = ci.vendor().empty() ? factory.info().vendor() : ci.vendor();
		p->classId = FUID::fromTUID(ci.ID().data());
		break;
	}
	if (!p->provider)
	{
		error = "no audio effect class in module";
		return false;
	}
	p->component = p->provider->getComponentPtr();
	p->controller = p->provider->getControllerPtr();
	p->processor = FUnknownPtr<IAudioProcessor>(p->component);
	if (!p->processor || !p->controller)
	{
		error = "plugin has no processor or controller";
		return false;
	}
	p->controller->setComponentHandler(&p->handler);

	const int32 n = p->controller->getParameterCount();
	for (int32 i = 0; i < n; ++i)
	{
		ParameterInfo pi{};
		p->controller->getParameterInfo(i, pi);
		VstParam v;
		v.id = pi.id;
		v.title = VST3::StringConvert::convert(pi.title);
		v.steps = pi.stepCount;
		v.def = p->controller->getParamNormalized(pi.id); // SynthEdit reports 0 as the 'default' for everything
		v.tweakable = !(pi.flags & (ParameterInfo::kIsReadOnly | ParameterInfo::kIsHidden | ParameterInfo::kIsProgramChange | ParameterInfo::kIsBypass)) &&
		              (pi.flags & ParameterInfo::kCanAutomate);
		for (const char* skip : {"MIDI CC", "Pitch Bend", "Aftertouch", "Channel Pressure", "Program Change"})
			if (v.title.find(skip) != std::string::npos)
				v.tweakable = false;
		p->params.push_back(v);
		values.push_back(v.def);
	}
	return true;
}

bool Vst3Plugin::render(const NoteSpec& note, int sr, Audio& out)
{
	constexpr int32 block = 512;
	auto& c = *p->component;

	// stereo out, no sidechain; instruments commonly have 0 audio inputs
	const int32 inBuses = c.getBusCount(kAudio, kInput), outBuses = c.getBusCount(kAudio, kOutput);
	SpeakerArrangement stereo = SpeakerArr::kStereo;
	std::vector<SpeakerArrangement> ins(inBuses, stereo), outs(outBuses, stereo);
	p->processor->setBusArrangements(ins.empty() ? nullptr : ins.data(), inBuses, outs.empty() ? nullptr : outs.data(), outBuses);
	for (int32 b = 0; b < outBuses; ++b) c.activateBus(kAudio, kOutput, b, b == 0);
	for (int32 b = 0; b < inBuses; ++b) c.activateBus(kAudio, kInput, b, false);
	c.activateBus(kEvent, kInput, 0, true);

	ProcessSetup setup{kOffline, kSample32, block, static_cast<double>(sr)};
	if (p->processor->setupProcessing(setup) != kResultOk)
		return false;

	// controller mirrors what the processor is given. Only the parameters we own are sent: SynthEdit's
	// MIDI CC / pitch-bend parameters read back 0 before any MIDI arrives, and sending that 0 bends
	// every note two semitones flat.
	for (size_t i = 0; i < p->params.size(); ++i)
		if (p->params[i].tweakable)
			p->controller->setParamNormalized(p->params[i].id, values[i]);

	c.setActive(true);
	p->processor->setProcessing(true);
	p->wasActive = true;

	HostProcessData data;
	data.prepare(c, block, kSample32);
	ParameterChanges changes(static_cast<int32>(p->params.size()) + 4);
	EventList events;
	data.inputParameterChanges = &changes;
	data.inputEvents = &events;
	ProcessContext ctx{};
	ctx.sampleRate = sr;
	ctx.tempo = 120;
	ctx.timeSigNumerator = ctx.timeSigDenominator = 4;
	ctx.state = ProcessContext::kPlaying | ProcessContext::kTempoValid | ProcessContext::kTimeSigValid;
	data.processContext = &ctx;
	data.numSamples = block;

	for (size_t i = 0; i < p->params.size(); ++i)
	{
		if (!p->params[i].tweakable) continue;
		int32 idx;
		if (auto* q = changes.addParameterData(p->params[i].id, idx))
			q->addPoint(0, values[i], idx);
	}

	const int64 holdN = static_cast<int64>(note.holdSec * sr), total = static_cast<int64>((note.holdSec + note.tailSec) * sr);
	out.sampleRate = sr;
	out.s.assign(static_cast<size_t>(total), 0.f);
	out.onset = 0; // the pre-roll is not recorded; note-on is the first sample

	// Silent pre-roll: plugins that rebuild tables on a worker thread (FretPlucked bakes after a 60 ms
	// debounce) only adopt the result at note-on, so give the new parameters real time to land first.
	const int64 preBlocks = static_cast<int64>(preRollSec * sr / block);
	for (int64 b = 0; b < preBlocks; ++b)
	{
		events.clear();
		ctx.projectTimeSamples = (b - preBlocks) * block;
		p->processor->process(data);
		changes.clearQueue();
		Sleep(8);
	}

	bool on = false, off = false;
	for (int64 pos = 0; pos < total; pos += block)
	{
		events.clear();
		if (!on)
		{
			Event e{};
			e.type = Event::kNoteOnEvent;
			e.noteOn.channel = 0; e.noteOn.pitch = static_cast<int16>(note.key);
			e.noteOn.velocity = note.velocity / 127.f; e.noteOn.noteId = 1; e.noteOn.length = 0;
			events.addEvent(e);
			on = true;
		}
		if (!off && holdN >= pos && holdN < pos + block)
		{
			Event e{};
			e.type = Event::kNoteOffEvent;
			e.sampleOffset = static_cast<int32>(holdN - pos);
			e.noteOff.channel = 0; e.noteOff.pitch = static_cast<int16>(note.key); e.noteOff.noteId = 1;
			events.addEvent(e);
			off = true;
		}
		ctx.projectTimeSamples = pos;
		if (p->processor->process(data) != kResultOk)
			break;
		changes.clearQueue(); // parameters apply once, at sample 0

		if (data.numOutputs > 0 && data.outputs[0].numChannels > 0)
		{
			const int ch = data.outputs[0].numChannels;
			const int64 n = std::min<int64>(block, total - pos);
			for (int64 i = 0; i < n; ++i)
			{
				float sum = 0;
				for (int k = 0; k < ch; ++k) sum += data.outputs[0].channelBuffers32[k][i];
				out.s[static_cast<size_t>(pos + i)] = sum / ch;
			}
		}
	}

	p->processor->setProcessing(false);
	c.setActive(false);
	p->wasActive = false;
	data.unprepare();
	return true;
}
