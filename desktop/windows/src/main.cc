#include "win32_app.h"

#include "log.h"

#include <Windows.h>
#include <winrt/base.h>

#include <cstdint>
#include <exception>
#include <string>

namespace {

constexpr wchar_t kSingleInstanceMutexName[] = L"Local\\TenClass.VoiceStick.SingleInstance";

class ScopedHandle {
public:
    explicit ScopedHandle(HANDLE handle) : handle_(handle) {}
    ~ScopedHandle() {
        if (handle_) CloseHandle(handle_);
    }

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    HANDLE get() const { return handle_; }

private:
    HANDLE handle_ = nullptr;
};

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    try {
        ScopedHandle single_instance_mutex(CreateMutexW(nullptr, TRUE, kSingleInstanceMutexName));
        if (!single_instance_mutex.get()) {
            voicestick::LogApp("CreateMutexW failed");
            return 1;
        }
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            voicestick::LogApp("Another VoiceStick instance is already running");
            return 0;
        }

        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        voicestick::Win32App app(instance);
        return app.Run();
    } catch (const winrt::hresult_error& error) {
        voicestick::LogApp("Fatal startup WinRT error: hr=" +
                           std::to_string(static_cast<std::int32_t>(error.code())) +
                           " message=" + winrt::to_string(error.message()));
    } catch (const std::exception& error) {
        voicestick::LogApp(std::string("Fatal startup exception: ") + error.what());
    } catch (...) {
        voicestick::LogApp("Fatal startup unknown exception");
    }
    return 1;
}
