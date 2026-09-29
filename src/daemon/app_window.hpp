// Окно приложения: обычное окно Windows с WebView2 внутри вместо вкладки браузера (только Windows)
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace safebox::daemon {

// Класс окна и свойство с портом сервера: по ним повторный запуск находит открытое окно.
inline constexpr wchar_t kAppWindowClass[] = L"SafeBoxV2Window";
inline constexpr wchar_t kAppWindowPortProp[] = L"SafeBoxV2Port";

struct AppWindowOptions {
    std::string url; // адрес интерфейса (UTF-8), чужие адреса откроются в системном браузере
    int port = 0;
    // Опрашивается по таймеру в потоке окна: true - закрыть окно (Ctrl+C, сервер остановился).
    std::function<bool()> stopRequested;
};

// Блокирует поток (цикл сообщений) до закрытия окна. nullopt - окно отработало и закрыто,
// иначе причина (по-русски), почему окна нет: нет WebView2 Runtime или он не запустился.
[[nodiscard]] std::optional<std::string> runAppWindow(const AppWindowOptions& options);

// Открывает интерфейс без своего окна: Edge в режиме приложения, иначе браузер по умолчанию.
void openInBrowser(std::string_view url);

// Окно с сообщением, когда консоли нет.
void showMessage(std::string_view text, bool error);

} // namespace safebox::daemon
