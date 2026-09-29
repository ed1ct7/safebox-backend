#include "app_window.hpp"

#include <windows.h>

#include <dwmapi.h>
#include <ole2.h>
#include <shellapi.h>
#include <wrl.h>

#include <WebView2.h>

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#include "config.hpp"
#include "safebox/domain/io.hpp"

namespace safebox::daemon {

namespace {

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

constexpr wchar_t kTitle[] = L"SafeBox";
constexpr int kIconResource = 1; // safeboxd.rc
constexpr COLORREF kBackground = RGB(0x0f, 0x11, 0x15);
constexpr UINT_PTR kStopTimer = 1;
constexpr UINT kStopPollMs = 200;
// размеры в пикселях при 96 dpi
constexpr int kDefaultWidth = 1280;
constexpr int kDefaultHeight = 800;
constexpr int kMinWidth = 720;
constexpr int kMinHeight = 480;

#if defined(NDEBUG)
constexpr bool kDevTools = false;
#else
constexpr bool kDevTools = true;
#endif

struct CoStringDeleter {
    void operator()(wchar_t* text) const noexcept { CoTaskMemFree(text); }
};
using CoString = std::unique_ptr<wchar_t, CoStringDeleter>;

[[nodiscard]] std::wstring toWide(std::string_view utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int size =
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), size);
    return wide;
}

[[nodiscard]] bool startsWithNoCase(std::wstring_view text, std::wstring_view prefix) {
    return text.size() >= prefix.size() &&
           CompareStringOrdinal(text.data(), static_cast<int>(prefix.size()), prefix.data(),
                                static_cast<int>(prefix.size()), TRUE) == CSTR_EQUAL;
}

// В системный браузер уходят только веб-адреса: file:, javascript: и прочее не запускаем.
void openExternal(std::wstring_view url) {
    if (!startsWithNoCase(url, L"http://") && !startsWithNoCase(url, L"https://")) {
        return;
    }
    const std::wstring address(url);
    ShellExecuteW(nullptr, L"open", address.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

[[nodiscard]] std::filesystem::path envDirectory(std::string_view name) {
    const auto value = systemEnv(name);
    return value && !value->empty() ? domain::pathFromUtf8(*value) : std::filesystem::path{};
}

// Положение окна: %APPDATA%\SafeBox\window.ini, строки key=value.
struct Placement {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    bool maximized = false;
};

[[nodiscard]] std::filesystem::path placementFile() {
    const auto dir = envDirectory("APPDATA");
    return dir.empty() ? dir : dir / "SafeBox" / "window.ini";
}

[[nodiscard]] std::optional<Placement> loadPlacement() {
    const auto file = placementFile();
    if (file.empty()) {
        return std::nullopt;
    }
    std::ifstream in(file);
    Placement placement;
    int found = 0;
    std::string line;
    while (std::getline(in, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const std::string_view key = std::string_view(line).substr(0, eq);
        const std::string_view text = std::string_view(line).substr(eq + 1);
        int value = 0;
        if (std::from_chars(text.data(), text.data() + text.size(), value).ec != std::errc{}) {
            continue;
        }
        if (key == "x") {
            placement.x = value;
        } else if (key == "y") {
            placement.y = value;
        } else if (key == "w") {
            placement.width = value;
        } else if (key == "h") {
            placement.height = value;
        } else if (key == "maximized") {
            placement.maximized = value != 0;
        } else {
            continue;
        }
        ++found;
    }
    if (found < 5 || placement.width < kMinWidth || placement.height < kMinHeight) {
        return std::nullopt;
    }
    return placement;
}

// Пишем нормальное (не развернутое) положение: после разворачивания окно вернется туда же.
void savePlacement(HWND window) {
    WINDOWPLACEMENT wp{};
    wp.length = sizeof(wp);
    const auto file = placementFile();
    if (file.empty() || !GetWindowPlacement(window, &wp)) {
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::trunc);
    const bool maximized = wp.showCmd == SW_SHOWMAXIMIZED || (wp.flags & WPF_RESTORETOMAXIMIZED);
    out << std::format("x={}\ny={}\nw={}\nh={}\nmaximized={}\n", wp.rcNormalPosition.left,
                       wp.rcNormalPosition.top,
                       wp.rcNormalPosition.right - wp.rcNormalPosition.left,
                       wp.rcNormalPosition.bottom - wp.rcNormalPosition.top, maximized ? 1 : 0);
}

[[nodiscard]] RECT centeredDefaultRect() {
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int dpi = static_cast<int>(GetDpiForSystem());
    const int width =
        std::min(MulDiv(kDefaultWidth, dpi, 96), static_cast<int>(work.right - work.left));
    const int height =
        std::min(MulDiv(kDefaultHeight, dpi, 96), static_cast<int>(work.bottom - work.top));
    const int left = work.left + (work.right - work.left - width) / 2;
    const int top = work.top + (work.bottom - work.top - height) / 2;
    return {left, top, left + width, top + height};
}

class AppWindow {
public:
    explicit AppWindow(const AppWindowOptions& options)
        : options_(options), url_(toWide(options.url)) {
        // origin - до третьего "/": http://127.0.0.1:8900
        const auto schemeEnd = url_.find(L"://");
        const auto pathStart =
            schemeEnd == std::wstring::npos ? schemeEnd : url_.find(L'/', schemeEnd + 3);
        origin_ = url_.substr(0, pathStart);
    }

    ~AppWindow() {
        if (background_ != nullptr) {
            DeleteObject(background_);
        }
    }
    AppWindow(const AppWindow&) = delete;
    AppWindow& operator=(const AppWindow&) = delete;

    [[nodiscard]] std::optional<std::string> run() {
        wchar_t* rawVersion = nullptr;
        const HRESULT found = GetAvailableCoreWebView2BrowserVersionString(nullptr, &rawVersion);
        const CoString version(rawVersion);
        if (FAILED(found) || !version) {
            return "Не найден Microsoft Edge WebView2 Runtime";
        }

        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        if (!create(instance)) {
            return "Не удалось создать окно";
        }
        startWebView();

        MSG message;
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        UnregisterClassW(kAppWindowClass, instance);
        return problem_;
    }

private:
    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        AppWindow* self = nullptr;
        if (message == WM_NCCREATE) {
            self =
                static_cast<AppWindow*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            self->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<AppWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        }
        return self != nullptr ? self->handle(message, wParam, lParam)
                               : DefWindowProcW(window, message, wParam, lParam);
    }

    [[nodiscard]] LRESULT handle(UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
        case WM_SIZE:
            fitToClient(wParam == SIZE_MINIMIZED);
            return 0;
        case WM_SETFOCUS:
            if (controller_) {
                controller_->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
            }
            return 0;
        case WM_GETMINMAXINFO: {
            const int dpi = static_cast<int>(GetDpiForWindow(window_));
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            info->ptMinTrackSize.x = MulDiv(kMinWidth, dpi, 96);
            info->ptMinTrackSize.y = MulDiv(kMinHeight, dpi, 96);
            return 0;
        }
        case WM_DPICHANGED: {
            const auto* rect = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(window_, nullptr, rect->left, rect->top, rect->right - rect->left,
                         rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_TIMER:
            if (wParam == kStopTimer && options_.stopRequested && options_.stopRequested()) {
                close();
            }
            return 0;
        case WM_CLOSE:
            close();
            return 0;
        case WM_DESTROY:
            KillTimer(window_, kStopTimer);
            RemovePropW(window_, kAppWindowPortProp);
            if (controller_) {
                controller_->Close();
                controller_.Reset();
            }
            webview_.Reset();
            PostQuitMessage(0);
            return 0;
        case WM_NCDESTROY: {
            const HWND window = window_;
            window_ = nullptr;
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            return DefWindowProcW(window, message, wParam, lParam);
        }
        default:
            return DefWindowProcW(window_, message, wParam, lParam);
        }
    }

    [[nodiscard]] bool create(HINSTANCE instance) {
        background_ = CreateSolidBrush(kBackground);
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.style = CS_HREDRAW | CS_VREDRAW;
        windowClass.lpfnWndProc = &AppWindow::windowProc;
        windowClass.hInstance = instance;
        windowClass.hIcon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(kIconResource),
                                                          IMAGE_ICON, 0, 0, LR_DEFAULTSIZE));
        windowClass.hIconSm = static_cast<HICON>(
            LoadImageW(instance, MAKEINTRESOURCEW(kIconResource), IMAGE_ICON,
                       GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
        windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW
        windowClass.hbrBackground = background_;
        windowClass.lpszClassName = kAppWindowClass;
        if (!RegisterClassExW(&windowClass)) {
            return false;
        }

        // сохраненное положение годится, если окно попадает на какой-нибудь из экранов
        auto saved = loadPlacement();
        WINDOWPLACEMENT placement{};
        placement.length = sizeof(placement);
        if (saved) {
            placement.rcNormalPosition = {saved->x, saved->y, saved->x + saved->width,
                                          saved->y + saved->height};
            placement.showCmd = saved->maximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
            if (MonitorFromRect(&placement.rcNormalPosition, MONITOR_DEFAULTTONULL) == nullptr) {
                saved.reset();
            }
        }
        const RECT rect = centeredDefaultRect();
        constexpr DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
        window_ = CreateWindowExW(
            0, kAppWindowClass, kTitle, style, saved ? CW_USEDEFAULT : rect.left,
            saved ? CW_USEDEFAULT : rect.top, saved ? CW_USEDEFAULT : rect.right - rect.left,
            saved ? CW_USEDEFAULT : rect.bottom - rect.top, nullptr, nullptr, instance, this);
        if (window_ == nullptr) {
            return false;
        }
        SetPropW(window_, kAppWindowPortProp,
                 reinterpret_cast<HANDLE>(static_cast<INT_PTR>(options_.port)));
        const BOOL dark = TRUE;
        DwmSetWindowAttribute(window_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
        SetTimer(window_, kStopTimer, kStopPollMs, nullptr);

        if (saved) {
            SetWindowPlacement(window_, &placement);
        } else {
            ShowWindow(window_, SW_SHOWNORMAL);
        }
        UpdateWindow(window_);
        return true;
    }

    void startWebView() {
        const auto local = envDirectory("LOCALAPPDATA");
        const auto dataDir =
            local.empty() ? std::filesystem::path{} : local / "SafeBox" / "WebView2";
        std::error_code ec;
        std::filesystem::create_directories(dataDir, ec);
        const HRESULT started = CreateCoreWebView2EnvironmentWithOptions(
            nullptr, dataDir.empty() ? nullptr : dataDir.c_str(), nullptr,
            Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
                [this](HRESULT result, ICoreWebView2Environment* environment) -> HRESULT {
                    onEnvironment(result, environment);
                    return S_OK;
                })
                .Get());
        if (FAILED(started)) {
            fail(startFailure(started));
        }
    }

    void onEnvironment(HRESULT result, ICoreWebView2Environment* environment) {
        if (window_ == nullptr) {
            return; // окно уже закрыли
        }
        if (FAILED(result) || environment == nullptr) {
            fail(startFailure(result));
            return;
        }
        const HRESULT created = environment->CreateCoreWebView2Controller(
            window_,
            Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                [this](HRESULT controllerResult, ICoreWebView2Controller* controller) -> HRESULT {
                    onController(controllerResult, controller);
                    return S_OK;
                })
                .Get());
        if (FAILED(created)) {
            fail(startFailure(created));
        }
    }

    void onController(HRESULT result, ICoreWebView2Controller* controller) {
        if (window_ == nullptr) {
            return;
        }
        if (FAILED(result) || controller == nullptr) {
            fail(startFailure(result));
            return;
        }
        controller_ = controller;
        controller_->get_CoreWebView2(&webview_);
        if (!webview_) {
            fail(startFailure(E_FAIL));
            return;
        }

        // тот же цвет, что у окна: без белой вспышки, пока страница не отрисована
        ComPtr<ICoreWebView2Controller2> colored;
        if (SUCCEEDED(controller_.As(&colored))) {
            colored->put_DefaultBackgroundColor(COREWEBVIEW2_COLOR{255, 0x0f, 0x11, 0x15});
        }
        configureSettings();
        routeExternalLinks();
        fitToClient(false);
        webview_->Navigate(url_.c_str());
        controller_->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
    }

    void configureSettings() {
        ComPtr<ICoreWebView2Settings> settings;
        if (FAILED(webview_->get_Settings(&settings))) {
            return;
        }
        settings->put_AreDevToolsEnabled(kDevTools ? TRUE : FALSE);
        // пароль сейфа и пути не должны оседать в профиле WebView2
        ComPtr<ICoreWebView2Settings4> privacy;
        if (SUCCEEDED(settings.As(&privacy))) {
            privacy->put_IsPasswordAutosaveEnabled(FALSE);
            privacy->put_IsGeneralAutofillEnabled(FALSE);
        }
    }

    // Окно показывает только интерфейс SafeBox; всё остальное - в системный браузер.
    void routeExternalLinks() {
        EventRegistrationToken token{};
        webview_->add_NavigationStarting(
            Callback<ICoreWebView2NavigationStartingEventHandler>(
                [this](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                    LPWSTR rawUri = nullptr;
                    args->get_Uri(&rawUri);
                    const CoString uri(rawUri);
                    if (uri && !isOwnPage(uri.get())) {
                        args->put_Cancel(TRUE);
                        openExternal(uri.get());
                    }
                    return S_OK;
                })
                .Get(),
            &token);
        webview_->add_NewWindowRequested(
            Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                    args->put_Handled(TRUE);
                    LPWSTR rawUri = nullptr;
                    args->get_Uri(&rawUri);
                    const CoString uri(rawUri);
                    if (uri) {
                        openExternal(uri.get());
                    }
                    return S_OK;
                })
                .Get(),
            &token);
    }

    // Адрес внутри origin интерфейса; префикс "http://127.0.0.1:8900" не должен пропускать
    // "http://127.0.0.1:89001" или "http://127.0.0.1:8900.example.com".
    [[nodiscard]] bool isOwnPage(std::wstring_view uri) const {
        if (!startsWithNoCase(uri, origin_)) {
            return false;
        }
        return uri.size() == origin_.size() ||
               std::wstring_view(L"/?#").find(uri[origin_.size()]) != std::wstring_view::npos;
    }

    void fitToClient(bool minimized) {
        if (!controller_) {
            return;
        }
        controller_->put_IsVisible(minimized ? FALSE : TRUE);
        if (!minimized) {
            RECT bounds{};
            GetClientRect(window_, &bounds);
            controller_->put_Bounds(bounds);
        }
    }

    // Обычное закрытие: запоминаем положение, дальше все как по Ctrl+C.
    void close() {
        if (window_ != nullptr) {
            savePlacement(window_);
            DestroyWindow(window_);
        }
    }

    // WebView2 не запустился: окно не нужно, дальше решает вызывающий (запасной путь).
    void fail(std::string problem) {
        if (!problem_) {
            problem_ = std::move(problem);
        }
        if (window_ != nullptr) {
            DestroyWindow(window_);
        }
    }

    [[nodiscard]] static std::string startFailure(HRESULT result) {
        return std::format("Не удалось запустить WebView2 (код 0x{:08X})",
                           static_cast<unsigned>(result));
    }

    const AppWindowOptions& options_;
    std::wstring url_;
    std::wstring origin_;
    HBRUSH background_ = nullptr;
    HWND window_ = nullptr;
    ComPtr<ICoreWebView2Controller> controller_;
    ComPtr<ICoreWebView2> webview_;
    std::optional<std::string> problem_;
};

} // namespace

std::optional<std::string> runAppWindow(const AppWindowOptions& options) {
    if (FAILED(OleInitialize(nullptr))) {
        return "Не удалось инициализировать COM";
    }
    std::optional<std::string> problem;
    {
        AppWindow window(options); // COM-объекты WebView2 освобождаются до OleUninitialize
        problem = window.run();
    }
    OleUninitialize();
    return problem;
}

void openInBrowser(std::string_view url) {
    const std::wstring address = toWide(url);
    const std::wstring arguments = L"--app=" + address;
    const auto edge = reinterpret_cast<INT_PTR>(
        ShellExecuteW(nullptr, L"open", L"msedge.exe", arguments.c_str(), nullptr, SW_SHOWNORMAL));
    if (edge <= 32) { // ShellExecute: значения до 32 - коды ошибок
        openExternal(address);
    }
}

void showMessage(std::string_view text, bool error) {
    MessageBoxW(nullptr, toWide(text).c_str(), kTitle,
                MB_OK | MB_SETFOREGROUND | (error ? MB_ICONERROR : MB_ICONINFORMATION));
}

} // namespace safebox::daemon
