#include "single_instance.hpp"

#include <format>

#include <windows.h>

#include "app_window.hpp"

namespace safebox::daemon {

namespace {

// Окна этого класса могут принадлежать экземплярам на других портах - сверяем порт из свойства
// окна.
[[nodiscard]] HWND findWindow(int port) {
    HWND window = nullptr;
    while ((window = FindWindowExW(nullptr, window, kAppWindowClass, nullptr)) != nullptr) {
        if (reinterpret_cast<INT_PTR>(GetPropW(window, kAppWindowPortProp)) == port) {
            return window;
        }
    }
    return nullptr;
}

} // namespace

SingleInstance::SingleInstance(int port) : port_(port) {
    const auto name = std::format(L"Local\\SafeBoxV2-{}", port);
    mutex_ = CreateMutexW(nullptr, FALSE, name.c_str());
    // не вышло создать мьютекс - не мешаем запуску
    first_ = mutex_ == nullptr || GetLastError() != ERROR_ALREADY_EXISTS;
}

SingleInstance::~SingleInstance() {
    if (mutex_ != nullptr) {
        CloseHandle(mutex_);
    }
}

bool SingleInstance::activateExisting() const {
    for (int attempt = 0; attempt < 30; ++attempt) {
        if (const HWND window = findWindow(port_)) {
            if (IsIconic(window)) {
                ShowWindow(window, SW_RESTORE); // развернутое окно SW_RESTORE свернул бы в обычное
            }
            SetForegroundWindow(window);
            return true;
        }
        Sleep(100);
    }
    return false;
}

} // namespace safebox::daemon
