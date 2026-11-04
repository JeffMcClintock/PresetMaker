#include "Platform.h"
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>

namespace platform
{
namespace
{
std::string utf8(const wchar_t* w)
{
	const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
	std::string s(n > 0 ? n - 1 : 0, '\0');
	if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
	return s;
}
} // namespace

std::string documentsFolder()
{
	wchar_t docs[MAX_PATH]{};
	SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, SHGFP_TYPE_CURRENT, docs);
	return utf8(docs);
}

std::string gmSoundSetPath()
{
	wchar_t sys[MAX_PATH]{};
	GetSystemDirectoryW(sys, MAX_PATH);
	return utf8(sys) + "\\drivers\\gm.dls";
}

std::string vst3PluginFolder()
{
	PWSTR common = nullptr;
	std::string r;
	if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramFilesCommon, 0, nullptr, &common))) r = utf8(common) + "\\VST3";
	CoTaskMemFree(common);
	return r;
}

std::string executablePath()
{
	wchar_t exe[MAX_PATH]{};
	GetModuleFileNameW(nullptr, exe, MAX_PATH);
	return utf8(exe);
}

void openFolder(const std::string& path)
{
	ShellExecuteA(nullptr, "open", path.c_str(), nullptr, nullptr, SW_SHOW);
}

void quietPluginDialogs()
{
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
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

ThreadRuntime::ThreadRuntime() { CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); }
ThreadRuntime::~ThreadRuntime() { CoUninitialize(); }

struct Process::Impl
{
	PROCESS_INFORMATION pi{};
	HANDLE log{};
};

Process::Process() : p(new Impl) {}

Process::~Process()
{
	if (p->pi.hProcess) CloseHandle(p->pi.hProcess);
	if (p->pi.hThread) CloseHandle(p->pi.hThread);
	if (p->log) CloseHandle(p->log);
}

std::unique_ptr<Process> Process::start(const std::string& commandLine, const std::string& logPath)
{
	std::unique_ptr<Process> r(new Process);
	SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
	r->p->log = CreateFileA(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, 0, nullptr);
	if (r->p->log == INVALID_HANDLE_VALUE) { r->p->log = nullptr; return nullptr; }
	STARTUPINFOA si{sizeof si};
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdOutput = si.hStdError = r->p->log;
	si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
	std::string cmd = commandLine; // CreateProcess may write to it
	if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &r->p->pi))
		return nullptr;
	return r;
}

size_t Process::waitAny(const std::vector<Process*>& running, int& exitCode)
{
	std::vector<HANDLE> hs;
	for (auto* q : running) hs.push_back(q->p->pi.hProcess);
	const DWORD r = WaitForMultipleObjects(static_cast<DWORD>(hs.size()), hs.data(), FALSE, INFINITE);
	const size_t i = r - WAIT_OBJECT_0;
	DWORD code = 0;
	GetExitCodeProcess(hs[i], &code);
	exitCode = static_cast<int>(code);
	return i;
}
} // namespace platform
