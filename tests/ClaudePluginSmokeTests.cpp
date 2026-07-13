#include "PluginInterface.h"

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

namespace
{
int failures = 0;

void Check(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::wstring CurrentExeDir()
{
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    path.resize(length);
    const auto slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : path.substr(0, slash);
}

bool LiveTestRequested()
{
    wchar_t value[8]{};
    return GetEnvironmentVariableW(L"TRAFFICMONITOR_CLAUDE_QUOTA_RUN_LIVE_TEST", value, 8) > 0
        && std::wstring(value) == L"1";
}

struct FindOwnWindowContext
{
    const wchar_t* class_name{};
    const wchar_t* title{};
    DWORD process_id{};
    HWND window{};
};

BOOL CALLBACK FindOwnWindow(HWND window, LPARAM parameter)
{
    auto* context = reinterpret_cast<FindOwnWindowContext*>(parameter);
    DWORD process_id = 0;
    GetWindowThreadProcessId(window, &process_id);
    if (process_id != context->process_id)
    {
        return TRUE;
    }

    wchar_t class_name[128]{};
    wchar_t title[256]{};
    GetClassNameW(window, class_name, static_cast<int>(_countof(class_name)));
    GetWindowTextW(window, title, static_cast<int>(_countof(title)));
    if (std::wstring(class_name) == context->class_name && std::wstring(title) == context->title)
    {
        context->window = window;
        return FALSE;
    }
    return TRUE;
}

HWND FindOwnWindowByClassAndTitle(const wchar_t* class_name, const wchar_t* title)
{
    FindOwnWindowContext context{class_name, title, GetCurrentProcessId(), nullptr};
    EnumWindows(FindOwnWindow, reinterpret_cast<LPARAM>(&context));
    return context.window;
}

int SystemDpi()
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

int ScaleForDpi(int value, int dpi)
{
    return MulDiv(value, dpi, 96);
}

struct IsolatedClaudeProfile
{
    std::wstring original_user_profile;
    bool had_original_user_profile{};
    std::wstring profile_path;
    std::wstring claude_path;
    std::wstring credentials_path;

    explicit IsolatedClaudeProfile(bool write_credentials = true)
    {
        wchar_t original[32768]{};
        const DWORD original_length = GetEnvironmentVariableW(
            L"USERPROFILE",
            original,
            static_cast<DWORD>(_countof(original)));
        if (original_length > 0 && original_length < _countof(original))
        {
            original_user_profile.assign(original, original_length);
            had_original_user_profile = true;
        }

        wchar_t temp[32768]{};
        const DWORD temp_length = GetTempPathW(static_cast<DWORD>(_countof(temp)), temp);
        profile_path.assign(temp, temp_length);
        profile_path += L"TrafficMonitorClaudeQuotaSmoke-" + std::to_wstring(GetCurrentProcessId());
        claude_path = profile_path + L"\\.claude";
        credentials_path = claude_path + L"\\.credentials.json";
        CreateDirectoryW(profile_path.c_str(), nullptr);
        CreateDirectoryW(claude_path.c_str(), nullptr);

        if (write_credentials)
        {
            static constexpr char kCredentials[] =
                R"({"claudeAiOauth":{"accessToken":"smoke-token","rateLimitTier":"test"}})";
            const HANDLE file = CreateFileW(
                credentials_path.c_str(),
                GENERIC_WRITE,
                0,
                nullptr,
                CREATE_ALWAYS,
                FILE_ATTRIBUTE_NORMAL,
                nullptr);
            if (file != INVALID_HANDLE_VALUE)
            {
                DWORD written = 0;
                WriteFile(file, kCredentials, static_cast<DWORD>(sizeof(kCredentials) - 1), &written, nullptr);
                CloseHandle(file);
            }
        }
        SetEnvironmentVariableW(L"USERPROFILE", profile_path.c_str());
    }

    ~IsolatedClaudeProfile()
    {
        if (had_original_user_profile)
        {
            SetEnvironmentVariableW(L"USERPROFILE", original_user_profile.c_str());
        }
        else
        {
            SetEnvironmentVariableW(L"USERPROFILE", nullptr);
        }
        DeleteFileW(credentials_path.c_str());
        RemoveDirectoryW(claude_path.c_str());
        RemoveDirectoryW(profile_path.c_str());
    }
};

void VerifyOptionsRequiresClaudeCodeLogin(ITMPlugin* plugin)
{
    IsolatedClaudeProfile profile(false);
    std::atomic<bool> finished{false};
    std::atomic<DWORD> dialog_thread_id{0};
    ITMPlugin::OptionReturn result = ITMPlugin::OR_OPTION_CHANGED;
    std::thread dialog_thread([&] {
        dialog_thread_id = GetCurrentThreadId();
        result = plugin->ShowOptionsDialog(nullptr);
        finished = true;
    });

    HWND prompt = nullptr;
    for (int attempt = 0; attempt < 50 && !finished; ++attempt)
    {
        prompt = FindOwnWindowByClassAndTitle(L"#32770", L"TrafficMonitor Claude Quota");
        if (prompt != nullptr)
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    Check(prompt != nullptr, "Claude options should require sign-in when Claude Code credentials are missing");
    if (prompt != nullptr)
    {
        const HWND no_button = GetDlgItem(prompt, IDNO);
        Check(no_button != nullptr, "Claude sign-in prompt should let the user decline login");
        PostMessageW(prompt, WM_COMMAND, MAKEWPARAM(IDNO, BN_CLICKED), reinterpret_cast<LPARAM>(no_button));
    }
    else if (dialog_thread_id != 0)
    {
        PostThreadMessageW(dialog_thread_id, WM_QUIT, 0, 0);
    }

    for (int attempt = 0; attempt < 50 && !finished; ++attempt)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (dialog_thread.joinable())
    {
        if (finished)
        {
            dialog_thread.join();
            Check(result == ITMPlugin::OR_OPTION_UNCHANGED,
                "declining Claude Code login should leave options unchanged");
            Check(FindOwnWindowByClassAndTitle(
                    L"TrafficMonitorClaudeQuotaOptions",
                    L"TrafficMonitor Claude Quota") == nullptr,
                "Claude display settings should not open before login succeeds");
        }
        else
        {
            Check(false, "Claude sign-in gate smoke test should finish");
            dialog_thread.detach();
        }
    }
}

bool ChildFitsInsideClient(HWND dialog, int control_id, const RECT& client)
{
    const HWND child = GetDlgItem(dialog, control_id);
    if (child == nullptr)
    {
        return false;
    }
    RECT child_rect{};
    GetWindowRect(child, &child_rect);
    MapWindowPoints(nullptr, dialog, reinterpret_cast<POINT*>(&child_rect), 2);
    return child_rect.left >= client.left
        && child_rect.top >= client.top
        && child_rect.right <= client.right
        && child_rect.bottom <= client.bottom;
}

void VerifyOptionsDialogUsesDpiScaledLayout(ITMPlugin* plugin)
{
    IsolatedClaudeProfile profile;
    std::atomic<bool> finished{false};
    std::atomic<DWORD> dialog_thread_id{0};
    ITMPlugin::OptionReturn result = ITMPlugin::OR_OPTION_CHANGED;
    std::thread dialog_thread([&] {
        dialog_thread_id = GetCurrentThreadId();
        result = plugin->ShowOptionsDialog(nullptr);
        finished = true;
    });

    HWND dialog = nullptr;
    for (int attempt = 0; attempt < 50 && !finished; ++attempt)
    {
        dialog = FindOwnWindowByClassAndTitle(
            L"TrafficMonitorClaudeQuotaOptions",
            L"TrafficMonitor Claude Quota");
        if (dialog != nullptr)
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    Check(dialog != nullptr, "Claude options dialog should open for DPI layout smoke test");
    if (dialog != nullptr)
    {
        const int dpi = SystemDpi();
        RECT client{};
        GetClientRect(dialog, &client);
        Check(client.right - client.left == ScaleForDpi(500, dpi),
            "Claude options client width should scale with the current DPI");
        Check(client.bottom - client.top == ScaleForDpi(284, dpi),
            "Claude options client height should scale with the current DPI");

        for (const int id : {2203, 2204, 2205, 2206, 2207, 2208, IDCANCEL})
        {
            Check(ChildFitsInsideClient(dialog, id, client),
                "every interactive Claude options control should fit inside the client area");
        }

        const HWND title = FindWindowExW(dialog, nullptr, L"STATIC", L"Claude quota display");
        Check(title != nullptr, "Claude options title control should exist");
        if (title != nullptr)
        {
            const auto font = reinterpret_cast<HFONT>(SendMessageW(title, WM_GETFONT, 0, 0));
            LOGFONTW log_font{};
            Check(font != nullptr && GetObjectW(font, sizeof(log_font), &log_font) == sizeof(log_font),
                "Claude options title font should be inspectable");
            Check(-log_font.lfHeight == MulDiv(13, dpi, 72),
                "Claude options title font should scale with the current DPI");
        }
        PostMessageW(dialog, WM_CLOSE, 0, 0);
    }
    else if (dialog_thread_id != 0)
    {
        PostThreadMessageW(dialog_thread_id, WM_QUIT, 0, 0);
    }

    for (int attempt = 0; attempt < 50 && !finished; ++attempt)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (dialog_thread.joinable())
    {
        if (finished)
        {
            dialog_thread.join();
            Check(result == ITMPlugin::OR_OPTION_UNCHANGED,
                "closing Claude options should leave options unchanged");
        }
        else
        {
            Check(false, "Claude options DPI layout smoke test should finish");
            dialog_thread.detach();
        }
    }
}
}

int wmain()
{
    SetProcessDPIAware();

    const auto dll_path = CurrentExeDir() + L"\\TrafficMonitorClaudeQuota.dll";
    const HMODULE module = LoadLibraryW(dll_path.c_str());
    Check(module != nullptr, "Claude plugin DLL should load");
    if (module == nullptr)
    {
        return 1;
    }

    using GetPluginFn = ITMPlugin* (*)();
    const auto get_plugin = reinterpret_cast<GetPluginFn>(GetProcAddress(module, "TMPluginGetInstance"));
    Check(get_plugin != nullptr, "TMPluginGetInstance should be exported");
    ITMPlugin* plugin = get_plugin == nullptr ? nullptr : get_plugin();
    Check(plugin != nullptr, "Claude plugin instance should be returned");
    if (plugin != nullptr)
    {
        Check(std::wstring(plugin->GetInfo(ITMPlugin::TMI_NAME)) == L"TrafficMonitor Claude Quota", "plugin name should match");
        Check(plugin->GetItem(3) == nullptr, "plugin should expose exactly three items");
        const wchar_t* expected_ids[] = {L"ClaudeQuota5h", L"ClaudeQuotaWeek", L"ClaudeQuotaMonth"};
        const wchar_t* expected_labels[] = {L"CL 5h:", L"CL 7d:", L"CL 1mo:"};
        for (int index = 0; index < 3; ++index)
        {
            IPluginItem* item = plugin->GetItem(index);
            Check(item != nullptr, "Claude item should exist");
            if (item == nullptr)
            {
                continue;
            }
            Check(std::wstring(item->GetItemId()) == expected_ids[index], "Claude item id should match");
            Check(std::wstring(item->GetItemLableText()) == expected_labels[index], "Claude item label should match");
            const std::wstring sample = item->GetItemValueSampleText();
            Check(!sample.empty() && sample.front() == L' ', "sample text should begin with visible spacing");
            Check(item->IsDrawResourceUsageGraph() == 1, "Claude item should enable graph support");
            Check(item->GetResourceUsageGraphValue() == 0.0f, "graph should be empty before refresh");
        }

        VerifyOptionsRequiresClaudeCodeLogin(plugin);
        VerifyOptionsDialogUsesDpiScaledLayout(plugin);

        if (LiveTestRequested())
        {
            plugin->DataRequired();
            for (int attempt = 0; attempt < 120; ++attempt)
            {
                bool completed = true;
                for (int index = 0; index < 3; ++index)
                {
                    const std::wstring value = plugin->GetItem(index)->GetItemValueText();
                    completed = completed && value != L" ...";
                }
                if (completed)
                {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
            const std::wstring tooltip = plugin->GetTooltipInfo();
            Check(tooltip.find(L"Last refresh: OK") != std::wstring::npos, "live Claude smoke test should refresh successfully");
        }
    }

    FreeLibrary(module);
    if (failures != 0)
    {
        std::cerr << failures << " Claude plugin smoke test(s) failed.\n";
        return 1;
    }
    std::cout << "Claude plugin smoke tests passed.\n";
    return 0;
}
