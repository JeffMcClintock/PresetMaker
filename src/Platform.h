#pragma once
// Everything PresetMaker needs from the operating system, so the fitting code, the command line
// and the form stay free of OS headers. PlatformWin.cpp implements it; another OS would add its own.
#include <memory>
#include <string>
#include <vector>

namespace platform
{
std::string documentsFolder();   // the user's Documents, UTF-8
std::string gmSoundSetPath();    // the system General MIDI sound set (gm.dls)
std::string vst3PluginFolder();  // where VST3 plugins are installed for all users
std::string executablePath();    // this program
void openFolder(const std::string& path); // in the file manager
void quietPluginDialogs();       // a plugin's crash or Debug-CRT assert reports to stderr, never a dialog

// Component runtime for the current thread (COM apartment on Windows); plugins need one.
struct ThreadRuntime
{
	ThreadRuntime();
	~ThreadRuntime();
	ThreadRuntime(const ThreadRuntime&) = delete;
	ThreadRuntime& operator=(const ThreadRuntime&) = delete;
};

// A child process whose output goes to a log file.
class Process
{
public:
	static std::unique_ptr<Process> start(const std::string& commandLine, const std::string& logPath);
	// blocks until one of them exits: its index, and its exit code
	static size_t waitAny(const std::vector<Process*>& running, int& exitCode);
	~Process();

private:
	struct Impl;
	std::unique_ptr<Impl> p;
	Process();
};
} // namespace platform
