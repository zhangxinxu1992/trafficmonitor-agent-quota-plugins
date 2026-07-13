#include "PluginInterface.h"
#include "ClaudeQuotaCore.h"
#include "ClaudeQuotaFetch.h"
#include "PluginVersion.h"

#include <Windows.h>

#include <chrono>
#include <ctime>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace
{
enum class WindowKind
{
    FiveHour,
    Weekly,
    Monthly
};

constexpr int kQuotaRemainingRadio = 2203;
constexpr int kQuotaUsedRadio = 2204;
constexpr int kShowResetInfoCheckbox = 2205;
constexpr int kResetCountdownRadio = 2206;
constexpr int kResetTimeRadio = 2207;
constexpr int kSaveButton = 2208;
constexpr const wchar_t* kOptionsDialogClassName = L"TrafficMonitorClaudeQuotaOptions";

std::wstring WindowsErrorMessage(DWORD error_code)
{
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error_code,
        0,
        reinterpret_cast<LPWSTR>(&buffer),
        0,
        nullptr);
    if (length == 0 || buffer == nullptr)
    {
        return L"Windows error " + std::to_wstring(error_code);
    }
    std::wstring message(buffer, length);
    LocalFree(buffer);
    while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n' || message.back() == L'.'))
    {
        message.pop_back();
    }
    return message;
}

std::wstring GetEnvVar(const wchar_t* name)
{
    const DWORD length = GetEnvironmentVariableW(name, nullptr, 0);
    if (length == 0)
    {
        return {};
    }
    std::wstring value(length, L'\0');
    const DWORD written = GetEnvironmentVariableW(name, value.data(), length);
    value.resize(written);
    return value;
}

std::wstring JoinPath(std::wstring base, const wchar_t* child)
{
    if (!base.empty() && base.back() != L'\\' && base.back() != L'/')
    {
        base.push_back(L'\\');
    }
    base += child;
    return base;
}

std::wstring GetDefaultConfigPath()
{
    const auto appdata = GetEnvVar(L"APPDATA");
    if (!appdata.empty())
    {
        return JoinPath(JoinPath(appdata, L"TrafficMonitorClaudeQuota"), L"config.json");
    }
    return L"TrafficMonitorClaudeQuota\\config.json";
}

bool ReadFileUtf8AsWide(const std::wstring& path, std::wstring& content, std::wstring& error)
{
    const HANDLE file = CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        const auto code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND)
        {
            content.clear();
            return true;
        }
        error = L"Failed to open TrafficMonitor Claude quota config: " + WindowsErrorMessage(code);
        return false;
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 || size.QuadPart > 1024 * 1024)
    {
        CloseHandle(file);
        error = L"Invalid TrafficMonitor Claude quota config size.";
        return false;
    }
    std::string bytes(static_cast<size_t>(size.QuadPart), '\0');
    DWORD bytes_read = 0;
    const BOOL read = bytes.empty() || ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &bytes_read, nullptr);
    CloseHandle(file);
    if (!read)
    {
        error = L"Failed to read TrafficMonitor Claude quota config: " + WindowsErrorMessage(GetLastError());
        return false;
    }
    bytes.resize(bytes_read);
    if (bytes.empty())
    {
        content.clear();
        return true;
    }

    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    if (length <= 0)
    {
        error = L"Failed to decode TrafficMonitor Claude quota config as UTF-8.";
        return false;
    }
    content.assign(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), content.data(), length);
    return true;
}

bool WriteFileWideAsUtf8(const std::wstring& path, const std::wstring& content, std::wstring& error)
{
    const auto slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos)
    {
        const auto directory = path.substr(0, slash);
        if (!CreateDirectoryW(directory.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
        {
            error = L"Failed to create TrafficMonitor Claude quota config directory: " + WindowsErrorMessage(GetLastError());
            return false;
        }
    }

    const int length = WideCharToMultiByte(CP_UTF8, 0, content.data(), static_cast<int>(content.size()), nullptr, 0, nullptr, nullptr);
    std::string bytes(static_cast<size_t>(length), '\0');
    if (length > 0)
    {
        WideCharToMultiByte(CP_UTF8, 0, content.data(), static_cast<int>(content.size()), bytes.data(), length, nullptr, nullptr);
    }
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        error = L"Failed to open TrafficMonitor Claude quota config for writing: " + WindowsErrorMessage(GetLastError());
        return false;
    }
    DWORD written = 0;
    const BOOL saved = bytes.empty() || WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    CloseHandle(file);
    if (!saved || written != bytes.size())
    {
        error = L"Failed to write TrafficMonitor Claude quota config: " + WindowsErrorMessage(GetLastError());
        return false;
    }
    return true;
}

std::optional<claudequota::PluginConfig> LoadConfig(std::wstring& error)
{
    std::wstring json;
    if (!ReadFileUtf8AsWide(GetDefaultConfigPath(), json, error))
    {
        return std::nullopt;
    }
    return claudequota::ParseConfigJson(json, error);
}

bool SaveConfig(const claudequota::PluginConfig& config, std::wstring& error)
{
    return WriteFileWideAsUtf8(GetDefaultConfigPath(), claudequota::SerializeConfigJson(config), error);
}

bool SameDisplayOptions(const claudequota::DisplayOptions& lhs, const claudequota::DisplayOptions& rhs)
{
    return lhs.quota_display == rhs.quota_display
        && lhs.reset_display == rhs.reset_display
        && lhs.show_reset_info == rhs.show_reset_info;
}

std::optional<std::wstring> FindClaudeCodeExecutable()
{
    wchar_t path[32768]{};
    const DWORD length = SearchPathW(nullptr, L"claude.exe", nullptr, static_cast<DWORD>(_countof(path)), path, nullptr);
    if (length == 0 || length >= _countof(path))
    {
        return std::nullopt;
    }
    return std::wstring(path, length);
}

bool RunClaudeCodeLogin(std::wstring& error)
{
    const auto executable = FindClaudeCodeExecutable();
    if (!executable.has_value())
    {
        error = L"Claude Code was not found on PATH. Install Claude Code, then run claude auth login.";
        return false;
    }

    std::wstring command_line = L"\"" + *executable + L"\" auth login";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(
            executable->c_str(),
            command_line.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NEW_CONSOLE,
            nullptr,
            nullptr,
            &startup,
            &process))
    {
        error = L"Failed to start claude auth login: " + WindowsErrorMessage(GetLastError());
        return false;
    }

    CloseHandle(process.hThread);
    const DWORD wait_result = WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code = 1;
    const BOOL read_exit_code = GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hProcess);
    if (wait_result != WAIT_OBJECT_0 || !read_exit_code)
    {
        error = L"Failed while waiting for claude auth login: " + WindowsErrorMessage(GetLastError());
        return false;
    }
    if (exit_code != 0)
    {
        error = L"claude auth login exited with code " + std::to_wstring(exit_code) + L".";
        return false;
    }

    const auto credentials = claudequota::ReadClaudeCodeOAuthCredentials(error);
    if (!credentials.has_value())
    {
        if (error.empty())
        {
            error = L"Claude Code login finished, but OAuth credentials were not found.";
        }
        return false;
    }
    error.clear();
    return true;
}

std::optional<claudequota::OAuthCredentials> EnsureClaudeCodeLogin(HWND parent, bool& authenticated_now)
{
    authenticated_now = false;
    std::wstring error;
    if (const auto credentials = claudequota::ReadClaudeCodeOAuthCredentials(error))
    {
        return credentials;
    }

    const int choice = MessageBoxW(
        parent,
        L"Claude Code sign-in is required before opening these settings.\n\n"
        L"The plugin uses the same protected OAuth credentials as Claude Code.\n\n"
        L"Start 'claude auth login' now?",
        L"TrafficMonitor Claude Quota",
        MB_YESNO | MB_ICONINFORMATION);
    if (choice != IDYES)
    {
        return std::nullopt;
    }

    if (!RunClaudeCodeLogin(error))
    {
        MessageBoxW(parent, error.c_str(), L"TrafficMonitor Claude Quota", MB_OK | MB_ICONERROR);
        return std::nullopt;
    }

    const auto credentials = claudequota::ReadClaudeCodeOAuthCredentials(error);
    if (!credentials.has_value())
    {
        MessageBoxW(parent, error.c_str(), L"TrafficMonitor Claude Quota", MB_OK | MB_ICONERROR);
        return std::nullopt;
    }
    authenticated_now = true;
    return credentials;
}

int FallbackSystemDpi()
{
    HDC dc = GetDC(nullptr);
    if (dc == nullptr)
    {
        return 96;
    }
    const int dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(nullptr, dc);
    return dpi > 0 ? dpi : 96;
}

int DialogDpi(HWND parent)
{
    const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32 != nullptr && parent != nullptr && IsWindow(parent))
    {
        using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
        auto* get_dpi_for_window = reinterpret_cast<GetDpiForWindowFn>(GetProcAddress(user32, "GetDpiForWindow"));
        if (get_dpi_for_window != nullptr)
        {
            const UINT dpi = get_dpi_for_window(parent);
            if (dpi > 0)
            {
                return static_cast<int>(dpi);
            }
        }
    }
    return FallbackSystemDpi();
}

int ScaleForDpi(int value, int dpi)
{
    return MulDiv(value, dpi, 96);
}

HFONT CreateDialogFont(int point_size, int weight, int dpi)
{
    return CreateFontW(
        -MulDiv(point_size, dpi, 72),
        0,
        0,
        0,
        weight,
        FALSE,
        FALSE,
        FALSE,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE,
        L"Segoe UI");
}

void AdjustWindowRectForDpi(RECT* rect, DWORD style, DWORD ex_style, int dpi)
{
    const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32 != nullptr)
    {
        using AdjustWindowRectExForDpiFn = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
        auto* adjust_for_dpi = reinterpret_cast<AdjustWindowRectExForDpiFn>(
            GetProcAddress(user32, "AdjustWindowRectExForDpi"));
        if (adjust_for_dpi != nullptr)
        {
            adjust_for_dpi(rect, style, FALSE, ex_style, static_cast<UINT>(dpi));
            return;
        }
    }
    AdjustWindowRectEx(rect, style, FALSE, ex_style);
}

void CenterWindow(HWND window, HWND parent)
{
    RECT window_rect{};
    GetWindowRect(window, &window_rect);
    RECT area{};
    if (parent != nullptr && IsWindow(parent))
    {
        GetWindowRect(parent, &area);
    }
    else
    {
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &area, 0);
    }
    const int width = window_rect.right - window_rect.left;
    const int height = window_rect.bottom - window_rect.top;
    SetWindowPos(
        window,
        nullptr,
        area.left + (area.right - area.left - width) / 2,
        area.top + (area.bottom - area.top - height) / 2,
        0,
        0,
        SWP_NOZORDER | SWP_NOSIZE | SWP_NOACTIVATE);
}

struct OptionsDialogState
{
    claudequota::PluginConfig original_config;
    claudequota::PluginConfig config;
    std::wstring auth_status;
    bool accepted{};
    int dpi{96};
    HFONT title_font{};
    HFONT status_font{};
    HFONT body_font{};
    HWND title_hwnd{};
    HWND status_hwnd{};
};

void CaptureDisplayOptions(HWND window, OptionsDialogState& state)
{
    state.config.display.quota_display = IsDlgButtonChecked(window, kQuotaUsedRadio) == BST_CHECKED
        ? claudequota::QuotaDisplayMode::Used
        : claudequota::QuotaDisplayMode::Remaining;
    state.config.display.show_reset_info = IsDlgButtonChecked(window, kShowResetInfoCheckbox) == BST_CHECKED;
    state.config.display.reset_display = IsDlgButtonChecked(window, kResetTimeRadio) == BST_CHECKED
        ? claudequota::ResetDisplayMode::Time
        : claudequota::ResetDisplayMode::Countdown;
}

LRESULT CALLBACK OptionsDialogProc(HWND window, UINT message, WPARAM w_param, LPARAM l_param)
{
    auto* state = reinterpret_cast<OptionsDialogState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message)
    {
    case WM_CREATE:
    {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(l_param);
        state = static_cast<OptionsDialogState*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));

        RECT client{};
        GetClientRect(window, &client);
        const int margin = ScaleForDpi(18, state->dpi);
        const int content_width = client.right - client.left - margin * 2;
        int y = ScaleForDpi(16, state->dpi);

        state->title_hwnd = CreateWindowExW(
            0,
            L"STATIC",
            L"Claude quota display",
            WS_CHILD | WS_VISIBLE,
            margin,
            y,
            content_width,
            ScaleForDpi(26, state->dpi),
            window,
            nullptr,
            nullptr,
            nullptr);
        y += ScaleForDpi(31, state->dpi);
        state->status_hwnd = CreateWindowExW(
            0,
            L"STATIC",
            state->auth_status.c_str(),
            WS_CHILD | WS_VISIBLE,
            margin,
            y,
            content_width,
            ScaleForDpi(22, state->dpi),
            window,
            nullptr,
            nullptr,
            nullptr);
        y += ScaleForDpi(32, state->dpi);
        CreateWindowExW(
            0,
            L"STATIC",
            L"Authentication is managed by Claude Code. Run 'claude auth login' in a terminal to switch accounts.",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            margin,
            y,
            content_width,
            ScaleForDpi(36, state->dpi),
            window,
            nullptr,
            nullptr,
            nullptr);

        y += ScaleForDpi(52, state->dpi);
        CreateWindowExW(0, L"STATIC", L"Quota:", WS_CHILD | WS_VISIBLE,
            margin, y, ScaleForDpi(90, state->dpi), ScaleForDpi(22, state->dpi), window, nullptr, nullptr, nullptr);
        CreateWindowExW(0, L"BUTTON", L"Remaining", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON,
            margin + ScaleForDpi(94, state->dpi), y - ScaleForDpi(2, state->dpi), ScaleForDpi(120, state->dpi), ScaleForDpi(24, state->dpi),
            window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kQuotaRemainingRadio)), nullptr, nullptr);
        CreateWindowExW(0, L"BUTTON", L"Used", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON,
            margin + ScaleForDpi(224, state->dpi), y - ScaleForDpi(2, state->dpi), ScaleForDpi(90, state->dpi), ScaleForDpi(24, state->dpi),
            window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kQuotaUsedRadio)), nullptr, nullptr);
        y += ScaleForDpi(36, state->dpi);
        CreateWindowExW(0, L"STATIC", L"Reset:", WS_CHILD | WS_VISIBLE,
            margin, y, ScaleForDpi(90, state->dpi), ScaleForDpi(22, state->dpi), window, nullptr, nullptr, nullptr);
        CreateWindowExW(0, L"BUTTON", L"Show reset info", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
            margin + ScaleForDpi(94, state->dpi), y - ScaleForDpi(2, state->dpi), ScaleForDpi(170, state->dpi), ScaleForDpi(24, state->dpi),
            window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kShowResetInfoCheckbox)), nullptr, nullptr);
        y += ScaleForDpi(32, state->dpi);
        CreateWindowExW(0, L"BUTTON", L"Countdown", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON,
            margin + ScaleForDpi(94, state->dpi), y, ScaleForDpi(120, state->dpi), ScaleForDpi(24, state->dpi), window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kResetCountdownRadio)), nullptr, nullptr);
        CreateWindowExW(0, L"BUTTON", L"Reset time", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON,
            margin + ScaleForDpi(224, state->dpi), y, ScaleForDpi(120, state->dpi), ScaleForDpi(24, state->dpi), window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kResetTimeRadio)), nullptr, nullptr);

        const int button_height = ScaleForDpi(30, state->dpi);
        const int button_width = ScaleForDpi(92, state->dpi);
        const int button_gap = ScaleForDpi(10, state->dpi);
        const int button_y = client.bottom - margin - button_height;
        int button_x = client.right - margin - button_width;
        CreateWindowExW(0, L"BUTTON", L"Save", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            button_x - button_gap - button_width, button_y, button_width, button_height, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSaveButton)), nullptr, nullptr);
        CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            button_x, button_y, button_width, button_height, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDCANCEL)), nullptr, nullptr);

        EnumChildWindows(window, [](HWND child, LPARAM value) -> BOOL {
            SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(value), TRUE);
            return TRUE;
        }, reinterpret_cast<LPARAM>(state->body_font));
        SendMessageW(state->title_hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(state->title_font), TRUE);
        SendMessageW(state->status_hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(state->status_font), TRUE);
        CheckRadioButton(window, kQuotaRemainingRadio, kQuotaUsedRadio,
            state->config.display.quota_display == claudequota::QuotaDisplayMode::Used ? kQuotaUsedRadio : kQuotaRemainingRadio);
        CheckRadioButton(window, kResetCountdownRadio, kResetTimeRadio,
            state->config.display.reset_display == claudequota::ResetDisplayMode::Time ? kResetTimeRadio : kResetCountdownRadio);
        SendMessageW(GetDlgItem(window, kShowResetInfoCheckbox), BM_SETCHECK,
            state->config.display.show_reset_info ? BST_CHECKED : BST_UNCHECKED, 0);
        EnableWindow(GetDlgItem(window, kResetCountdownRadio), state->config.display.show_reset_info);
        EnableWindow(GetDlgItem(window, kResetTimeRadio), state->config.display.show_reset_info);
        SetFocus(GetDlgItem(window, kSaveButton));
        return 0;
    }
    case WM_COMMAND:
        if (state == nullptr)
        {
            break;
        }
        switch (LOWORD(w_param))
        {
        case kShowResetInfoCheckbox:
        {
            const bool enabled = IsDlgButtonChecked(window, kShowResetInfoCheckbox) == BST_CHECKED;
            EnableWindow(GetDlgItem(window, kResetCountdownRadio), enabled);
            EnableWindow(GetDlgItem(window, kResetTimeRadio), enabled);
            return 0;
        }
        case kSaveButton:
        {
            CaptureDisplayOptions(window, *state);
            state->accepted = true;
            DestroyWindow(window);
            return 0;
        }
        case IDCANCEL:
            DestroyWindow(window);
            return 0;
        default:
            break;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, w_param, l_param);
}

bool ShowOptionsDialog(HWND parent, OptionsDialogState& state)
{
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = OptionsDialogProc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    window_class.lpszClassName = kOptionsDialogClassName;
    if (!RegisterClassW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        return false;
    }

    state.dpi = DialogDpi(parent);
    state.title_font = CreateDialogFont(13, FW_NORMAL, state.dpi);
    state.status_font = CreateDialogFont(9, FW_BOLD, state.dpi);
    state.body_font = CreateDialogFont(9, FW_NORMAL, state.dpi);
    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    const DWORD ex_style = WS_EX_CONTROLPARENT | WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE;
    RECT window_rect{0, 0, ScaleForDpi(500, state.dpi), ScaleForDpi(284, state.dpi)};
    AdjustWindowRectForDpi(&window_rect, style, ex_style, state.dpi);

    HWND window = CreateWindowExW(
        ex_style,
        kOptionsDialogClassName,
        L"TrafficMonitor Claude Quota",
        style,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        window_rect.right - window_rect.left,
        window_rect.bottom - window_rect.top,
        parent,
        nullptr,
        instance,
        &state);
    if (window == nullptr)
    {
        DeleteObject(state.title_font);
        DeleteObject(state.status_font);
        DeleteObject(state.body_font);
        state.title_font = nullptr;
        state.status_font = nullptr;
        state.body_font = nullptr;
        return false;
    }

    CenterWindow(window, parent);
    const BOOL disable_parent = parent != nullptr && IsWindow(parent) && IsWindowEnabled(parent);
    if (disable_parent)
    {
        EnableWindow(parent, FALSE);
    }
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);
    MSG message{};
    while (IsWindow(window))
    {
        const BOOL got_message = GetMessageW(&message, nullptr, 0, 0);
        if (got_message <= 0)
        {
            break;
        }
        if (!IsDialogMessageW(window, &message))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    if (disable_parent)
    {
        EnableWindow(parent, TRUE);
        SetActiveWindow(parent);
    }
    DeleteObject(state.title_font);
    DeleteObject(state.status_font);
    DeleteObject(state.body_font);
    state.title_font = nullptr;
    state.status_font = nullptr;
    state.body_font = nullptr;
    return state.accepted;
}

std::wstring BuildSampleText(WindowKind kind, const claudequota::DisplayOptions& options)
{
    std::wstring sample = L" 100%";
    if (!options.show_reset_info)
    {
        return sample;
    }
    if (options.reset_display == claudequota::ResetDisplayMode::Time)
    {
        return sample + L" 12-31 23:59";
    }
    switch (kind)
    {
    case WindowKind::FiveHour: return sample + L" 4h 59m";
    case WindowKind::Weekly: return sample + L" 6d 23h";
    case WindowKind::Monthly: return sample + L" 4w 3d";
    }
    return sample;
}

class ClaudeQuotaPlugin;

class ClaudeQuotaItem final : public IPluginItem
{
public:
    explicit ClaudeQuotaItem(WindowKind kind) : m_kind(kind) {}

    const wchar_t* GetItemName() const override
    {
        switch (m_kind)
        {
        case WindowKind::FiveHour: return L"TrafficMonitor Claude 5h";
        case WindowKind::Weekly: return L"TrafficMonitor Claude Week";
        case WindowKind::Monthly: return L"TrafficMonitor Claude Month";
        }
        return L"TrafficMonitor Claude Quota";
    }

    const wchar_t* GetItemId() const override
    {
        switch (m_kind)
        {
        case WindowKind::FiveHour: return L"ClaudeQuota5h";
        case WindowKind::Weekly: return L"ClaudeQuotaWeek";
        case WindowKind::Monthly: return L"ClaudeQuotaMonth";
        }
        return L"ClaudeQuota";
    }

    const wchar_t* GetItemLableText() const override
    {
        switch (m_kind)
        {
        case WindowKind::FiveHour: return L"CL 5h:";
        case WindowKind::Weekly: return L"CL 7d:";
        case WindowKind::Monthly: return L"CL 1mo:";
        }
        return L"CL:";
    }

    const wchar_t* GetItemValueText() const override;
    const wchar_t* GetItemValueSampleText() const override;
    int IsDrawResourceUsageGraph() const override { return 1; }
    float GetResourceUsageGraphValue() const override;

private:
    WindowKind m_kind;
    mutable std::wstring m_value;
    mutable std::wstring m_sample;
};

class ClaudeQuotaPlugin final : public ITMPlugin
{
public:
    static ClaudeQuotaPlugin& Instance()
    {
        static ClaudeQuotaPlugin instance;
        return instance;
    }

    IPluginItem* GetItem(int index) override
    {
        switch (index)
        {
        case 0: return &m_five_hour_item;
        case 1: return &m_weekly_item;
        case 2: return &m_monthly_item;
        default: return nullptr;
        }
    }

    void DataRequired() override
    {
        const auto now = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_refreshing || now < m_next_refresh)
            {
                return;
            }
            m_refreshing = true;
        }
        if (m_worker.joinable())
        {
            m_worker.join();
        }
        m_worker = std::thread([] {
            ClaudeQuotaPlugin::Instance().ApplyFetchResult(claudequota::FetchUsageSnapshot());
        });
    }

    const wchar_t* GetInfo(PluginInfoIndex index) override
    {
        switch (index)
        {
        case TMI_NAME: return L"TrafficMonitor Claude Quota";
        case TMI_DESCRIPTION: return L"Displays remaining Claude 5-hour, weekly, and monthly spend-limit quota in TrafficMonitor.";
        case TMI_AUTHOR: return L"zhangxinxu";
        case TMI_COPYRIGHT: return L"MIT";
        case TMI_VERSION: return kTrafficMonitorQuotaPluginVersion;
        case TMI_URL: return L"https://github.com/zhangxinxu1992/trafficmonitor-agent-quota-plugins";
        default: return L"";
        }
    }

    const wchar_t* GetTooltipInfo() override
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_tooltip_return = BuildTooltipLocked();
        return m_tooltip_return.c_str();
    }

    OptionReturn ShowOptionsDialog(void* hParent) override
    {
        const auto parent = static_cast<HWND>(hParent);
        bool authenticated_now = false;
        const auto credentials = EnsureClaudeCodeLogin(parent, authenticated_now);
        if (!credentials.has_value())
        {
            return OR_OPTION_UNCHANGED;
        }

        std::wstring error;
        const auto config = LoadConfig(error);
        if (!config.has_value())
        {
            MessageBoxW(parent, error.c_str(), L"TrafficMonitor Claude Quota", MB_OK | MB_ICONERROR);
            return OR_OPTION_UNCHANGED;
        }

        OptionsDialogState state;
        state.original_config = *config;
        state.config = *config;
        state.auth_status = L"Status: signed in with Claude Code.";
        if (!::ShowOptionsDialog(parent, state))
        {
            if (authenticated_now)
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_next_refresh = {};
                m_has_usage = false;
                m_last_error.clear();
            }
            return authenticated_now ? OR_OPTION_CHANGED : OR_OPTION_UNCHANGED;
        }

        const bool config_changed = !SameDisplayOptions(state.config.display, state.original_config.display);
        if (config_changed && !SaveConfig(state.config, error))
        {
            MessageBoxW(parent, error.c_str(), L"TrafficMonitor Claude Quota", MB_OK | MB_ICONERROR);
            return OR_OPTION_UNCHANGED;
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_config = state.config;
            if (authenticated_now)
            {
                m_next_refresh = {};
                m_has_usage = false;
                m_last_error.clear();
            }
        }
        return config_changed || authenticated_now
            ? OR_OPTION_CHANGED
            : OR_OPTION_UNCHANGED;
    }

    std::wstring ValueText(WindowKind kind) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto* window = SelectWindow(kind);
        if (m_has_usage && window != nullptr && window->present)
        {
            return L" " + claudequota::FormatWindowText(window->used_percent, window->reset_at, std::time(nullptr), m_config.display);
        }
        if (m_has_usage)
        {
            return L" N/A";
        }
        return m_last_error.empty() ? L" ..." : L" ERR";
    }

    std::wstring SampleText(WindowKind kind) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return BuildSampleText(kind, m_config.display);
    }

    float ResourceGraphValue(WindowKind kind) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto* window = SelectWindow(kind);
        return m_has_usage && window != nullptr && window->present
            ? claudequota::FormatResourceGraphValue(window->used_percent, m_config.display)
            : 0.0f;
    }

private:
    ClaudeQuotaPlugin()
        : m_five_hour_item(WindowKind::FiveHour),
          m_weekly_item(WindowKind::Weekly),
          m_monthly_item(WindowKind::Monthly)
    {
        std::wstring error;
        if (const auto config = LoadConfig(error))
        {
            m_config = *config;
        }
    }

    ~ClaudeQuotaPlugin()
    {
        if (m_worker.joinable())
        {
            m_worker.join();
        }
    }

    const claudequota::RateWindow* SelectWindow(WindowKind kind) const
    {
        if (!m_has_usage)
        {
            return nullptr;
        }
        switch (kind)
        {
        case WindowKind::FiveHour: return &m_usage.primary;
        case WindowKind::Weekly: return &m_usage.secondary;
        case WindowKind::Monthly: return &m_usage.monthly;
        }
        return nullptr;
    }

    void ApplyFetchResult(const claudequota::FetchResult& result)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_refreshing = false;
        m_last_refresh = std::time(nullptr);
        if (result.success)
        {
            m_usage = result.usage;
            m_has_usage = true;
            m_last_error.clear();
            m_next_refresh = std::chrono::steady_clock::now() + std::chrono::minutes(5);
        }
        else
        {
            m_last_error = result.error.empty() ? L"Unknown Claude usage error." : result.error;
            m_next_refresh = std::chrono::steady_clock::now() + std::chrono::minutes(1);
        }
    }

    std::wstring WindowLineLocked(const wchar_t* title, const claudequota::RateWindow& window) const
    {
        if (!window.present)
        {
            return std::wstring(title) + L": unavailable";
        }
        std::wstring line = std::wstring(title) + L": "
            + claudequota::FormatWindowText(window.used_percent, 0, std::time(nullptr), m_config.display);
        line += m_config.display.quota_display == claudequota::QuotaDisplayMode::Used ? L" used" : L" remaining";
        if (window.reset_at > 0)
        {
            line += m_config.display.reset_display == claudequota::ResetDisplayMode::Time ? L", resets at " : L", resets in ";
            line += m_config.display.reset_display == claudequota::ResetDisplayMode::Time
                ? claudequota::FormatResetTime(window.reset_at, std::time(nullptr))
                : claudequota::FormatResetCountdown(window.reset_at, std::time(nullptr));
        }
        return line;
    }

    std::wstring BuildTooltipLocked() const
    {
        std::wstring tooltip = L"TrafficMonitor Claude quota";
        if (!m_usage.plan_type.empty())
        {
            tooltip += L" (" + m_usage.plan_type + L")";
        }
        tooltip += L"\n";
        if (m_has_usage)
        {
            tooltip += WindowLineLocked(L"5h", m_usage.primary) + L"\n";
            tooltip += WindowLineLocked(L"Week", m_usage.secondary) + L"\n";
            tooltip += WindowLineLocked(L"Month", m_usage.monthly);
        }
        else if (m_refreshing)
        {
            tooltip += L"Refreshing...";
        }
        else
        {
            tooltip += L"Waiting for first refresh.";
        }
        if (m_last_refresh > 0)
        {
            tooltip += m_last_error.empty() ? L"\nLast refresh: OK" : L"\nLast refresh: failed";
        }
        if (!m_last_error.empty())
        {
            tooltip += L"\nError: " + m_last_error;
        }
        return tooltip;
    }

    ClaudeQuotaItem m_five_hour_item;
    ClaudeQuotaItem m_weekly_item;
    ClaudeQuotaItem m_monthly_item;
    mutable std::mutex m_mutex;
    claudequota::UsageSnapshot m_usage;
    claudequota::PluginConfig m_config;
    bool m_has_usage{};
    bool m_refreshing{};
    std::time_t m_last_refresh{};
    std::chrono::steady_clock::time_point m_next_refresh{};
    std::thread m_worker;
    std::wstring m_last_error;
    std::wstring m_tooltip_return;
};

const wchar_t* ClaudeQuotaItem::GetItemValueText() const
{
    m_value = ClaudeQuotaPlugin::Instance().ValueText(m_kind);
    return m_value.c_str();
}

const wchar_t* ClaudeQuotaItem::GetItemValueSampleText() const
{
    m_sample = ClaudeQuotaPlugin::Instance().SampleText(m_kind);
    return m_sample.c_str();
}

float ClaudeQuotaItem::GetResourceUsageGraphValue() const
{
    return ClaudeQuotaPlugin::Instance().ResourceGraphValue(m_kind);
}
}

extern "C" __declspec(dllexport) ITMPlugin* TMPluginGetInstance()
{
    return &ClaudeQuotaPlugin::Instance();
}
