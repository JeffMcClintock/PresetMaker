// PresetMaker's window: an overlapped HWND filled by gmpi_ui's Direct2D DrawingFrame, with a
// FitForm as the frame's client. After gmpi_ui/examples/FormsDemo/MainWin32.cpp.
#define NOMINMAX
#include <windows.h>

#include "backends/DrawingFrameWin.h"
#include "FitForm.h"

namespace
{
constexpr DWORD kStyle = WS_OVERLAPPEDWINDOW;
constexpr wchar_t kClass[] = L"PresetMakerToplevel";
constexpr int kWidthDips = 640, kHeightDips = 460;

// DPI awareness + STA; first member, since DrawingFrame's construction needs COM for its WIC factory.
struct ComApartment
{
	HRESULT hr;
	ComApartment()
	{
		::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
		hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	}
	~ComApartment() { if (SUCCEEDED(hr)) ::CoUninitialize(); }
};

class App
{
public:
	~App() { close(); }

	bool create()
	{
		const HINSTANCE instance = ::GetModuleHandleW(nullptr);
		WNDCLASSEXW wc{};
		wc.cbSize = sizeof(wc);
		wc.lpfnWndProc = &App::windowProc;
		wc.hInstance = instance;
		wc.hCursor = ::LoadCursor(nullptr, IDC_ARROW);
		wc.hIcon = ::LoadIcon(nullptr, IDI_APPLICATION);
		wc.lpszClassName = kClass;
		if (!::RegisterClassExW(&wc)) return false;

		const UINT guessedDpi = ::GetDpiForSystem();
		RECT r = clientRectForDpi(guessedDpi);
		hwnd_ = ::CreateWindowExW(0, kClass, L"PresetMaker", kStyle, CW_USEDEFAULT, CW_USEDEFAULT,
		                          r.right - r.left, r.bottom - r.top, nullptr, nullptr, instance, this);
		if (!hwnd_) return false;
		if (const UINT dpi = ::GetDpiForWindow(hwnd_); dpi != guessedDpi)
		{
			r = clientRectForDpi(dpi);
			::SetWindowPos(hwnd_, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
		}
		frame_.open(hwnd_);
		if (!frame_.getWindowHandle()) return false;
		frame_.attachClient(static_cast<gmpi::api::IEditor*>(form_.get()));
		return true;
	}

	int run()
	{
		::ShowWindow(hwnd_, SW_SHOW);
		::UpdateWindow(hwnd_);
		MSG msg{};
		while (::GetMessageW(&msg, nullptr, 0, 0) > 0)
		{
			::TranslateMessage(&msg);
			::DispatchMessageW(&msg);
		}
		return static_cast<int>(msg.wParam);
	}

private:
	static RECT clientRectForDpi(UINT dpi)
	{
		RECT r{ 0, 0, ::MulDiv(kWidthDips, static_cast<int>(dpi), 96), ::MulDiv(kHeightDips, static_cast<int>(dpi), 96) };
		::AdjustWindowRectExForDpi(&r, kStyle, FALSE, 0, dpi);
		return r;
	}

	// frame first: it must be the one to destroy its child window
	void close()
	{
		frame_.detachClient();
		frame_.close();
		if (hwnd_) ::DestroyWindow(hwnd_);
	}

	static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
	{
		if (message == WM_NCCREATE)
			::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams));
		auto* const self = reinterpret_cast<App*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
		if (!self) return ::DefWindowProcW(hwnd, message, wParam, lParam);
		if (message == WM_NCDESTROY)
		{
			::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
			self->hwnd_ = {};
			return ::DefWindowProcW(hwnd, message, wParam, lParam);
		}
		if (!self->hwnd_) self->hwnd_ = hwnd;
		return self->onMessage(hwnd, message, wParam, lParam);
	}

	LRESULT onMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
	{
		switch (message)
		{
		case WM_SIZE:
			if (wParam != SIZE_MINIMIZED && frame_.getWindowHandle())
			{
				RECT c{};
				::GetClientRect(hwnd, &c);
				frame_.reSize(0, 0, c.right, c.bottom);
			}
			return 0;
		case WM_DPICHANGED:
		{
			const auto* const s = reinterpret_cast<const RECT*>(lParam);
			::SetWindowPos(hwnd, nullptr, s->left, s->top, s->right - s->left, s->bottom - s->top, SWP_NOZORDER | SWP_NOACTIVATE);
			return 0;
		}
		case WM_SETFOCUS:
			if (const HWND child = frame_.getWindowHandle()) ::SetFocus(child);
			return 0;
		case WM_ERASEBKGND:
			return 1;
		case WM_CLOSE:
			::ShowWindow(hwnd, SW_HIDE);
			::PostQuitMessage(0);
			return 0;
		case WM_DESTROY:
			hwnd_ = {};
			::PostQuitMessage(0);
			return 0;
		}
		return ::DefWindowProcW(hwnd, message, wParam, lParam);
	}

	// member order is the contract: the form outlives the frame that references it
	ComApartment com_;
	gmpi::shared_ptr<FitForm> form_{ new FitForm() };
	gmpi::hosting::DrawingFrame frame_;
	HWND hwnd_{};
};
} // namespace

int APIENTRY wWinMain(_In_ HINSTANCE, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int)
{
	App app;
	if (!app.create())
	{
		::MessageBoxW(nullptr, L"Could not create the window.", L"PresetMaker", MB_OK | MB_ICONERROR);
		return 1;
	}
	return app.run();
}
