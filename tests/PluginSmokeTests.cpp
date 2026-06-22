#include "PluginInterface.h"
#include "../src/PluginVersion.h"

#include <Windows.h>
#include <cstring>
#include <iostream>
#include <string>

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
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring value(path);
    const auto slash = value.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : value.substr(0, slash);
}

std::wstring CreateIsolatedAppDataDir()
{
    wchar_t temp_path[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp_path);

    wchar_t temp_name[MAX_PATH]{};
    GetTempFileNameW(temp_path, L"cxq", 0, temp_name);
    DeleteFileW(temp_name);
    CreateDirectoryW(temp_name, nullptr);
    return temp_name;
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

void WriteAsciiFile(const std::wstring& path, const char* content)
{
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(file != INVALID_HANDLE_VALUE, "test config file should be writable");
    if (file == INVALID_HANDLE_VALUE)
    {
        return;
    }

    DWORD written = 0;
    const auto size = static_cast<DWORD>(std::strlen(content));
    const BOOL ok = WriteFile(file, content, size, &written, nullptr);
    CloseHandle(file);
    Check(ok && written == size, "test config file should be fully written");
}

void PrepareDisplayConfig(const std::wstring& appdata)
{
    const auto dir = JoinPath(appdata, L"TrafficMonitorCodexQuota");
    CreateDirectoryW(dir.c_str(), nullptr);
    WriteAsciiFile(
        JoinPath(dir, L"config.json"),
        "{\n"
        "  \"quota_display\": \"remaining\",\n"
        "  \"reset_display\": \"countdown\"\n"
        "}\n");
}

std::wstring ReadEnvironmentVariable(const wchar_t* name)
{
    const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (size == 0)
    {
        return {};
    }

    std::wstring value(size, L'\0');
    const DWORD written = GetEnvironmentVariableW(name, value.data(), size);
    value.resize(written);
    return value;
}

class EnvironmentVariableGuard
{
public:
    EnvironmentVariableGuard(const wchar_t* name, const wchar_t* value)
        : m_name(name),
          m_had_value(GetEnvironmentVariableW(name, nullptr, 0) != 0),
          m_original(ReadEnvironmentVariable(name))
    {
        SetEnvironmentVariableW(m_name.c_str(), value);
    }

    ~EnvironmentVariableGuard()
    {
        SetEnvironmentVariableW(m_name.c_str(), m_had_value ? m_original.c_str() : nullptr);
    }

private:
    std::wstring m_name;
    bool m_had_value{};
    std::wstring m_original;
};

bool Contains(const std::wstring& value, const wchar_t* text)
{
    return value.find(text) != std::wstring::npos;
}

bool StartsWith(const std::wstring& value, const wchar_t* prefix)
{
    const std::wstring prefix_value(prefix);
    return value.size() >= prefix_value.size()
        && value.compare(0, prefix_value.size(), prefix_value) == 0;
}

bool ContainsResetIndicator(const std::wstring& value)
{
    return Contains(value, L"m")
        || Contains(value, L"h")
        || Contains(value, L"d")
        || Contains(value, L"w")
        || Contains(value, L"now")
        || Contains(value, L":");
}

bool RunLiveRefresh()
{
    wchar_t flag[8]{};
    return GetEnvironmentVariableW(L"TRAFFICMONITOR_CODEX_QUOTA_RUN_LIVE_TEST", flag, 8) != 0
        || GetEnvironmentVariableW(L"CODEX_QUOTA_RUN_LIVE_TEST", flag, 8) != 0;
}
}

int main()
{
    const auto appdata = CreateIsolatedAppDataDir();
    PrepareDisplayConfig(appdata);
    EnvironmentVariableGuard appdata_guard(L"APPDATA", appdata.c_str());

    const auto dll_path = CurrentExeDir() + L"\\TrafficMonitorCodexQuota.dll";
    HMODULE module = LoadLibraryW(dll_path.c_str());
    if (module == nullptr)
    {
        std::wcerr << L"FAIL: LoadLibrary failed for " << dll_path << L" error " << GetLastError() << L'\n';
        return 1;
    }

    auto get_instance = reinterpret_cast<ITMPlugin* (*)()>(GetProcAddress(module, "TMPluginGetInstance"));
    Check(get_instance != nullptr, "TMPluginGetInstance should be exported");
    ITMPlugin* plugin = get_instance == nullptr ? nullptr : get_instance();
    Check(plugin != nullptr, "TMPluginGetInstance should return a plugin");

    if (plugin != nullptr)
    {
        Check(plugin->GetAPIVersion() >= 7, "plugin API version should support OnInitialize");
        Check(std::wstring(plugin->GetInfo(ITMPlugin::TMI_NAME)) == L"TrafficMonitor Codex Quota", "plugin name should match");
        Check(std::wstring(plugin->GetInfo(ITMPlugin::TMI_VERSION)) == kTrafficMonitorQuotaPluginVersion, "plugin version should match the unified release version");
        {
            EnvironmentVariableGuard options_guard(L"TRAFFICMONITOR_CODEX_QUOTA_OPTIONS_SMOKE_TEST", L"1");
            Check(plugin->ShowOptionsDialog(nullptr) == ITMPlugin::OR_OPTION_UNCHANGED,
                "Codex plugin options dialog should be provided");
        }

        IPluginItem* five_hour = plugin->GetItem(0);
        IPluginItem* weekly = plugin->GetItem(1);
        Check(five_hour != nullptr, "first item should exist");
        Check(weekly != nullptr, "second item should exist");
        Check(plugin->GetItem(2) == nullptr, "third item should not exist");

        if (five_hour != nullptr)
        {
            Check(std::wstring(five_hour->GetItemId()) == L"CodexQuota5h", "5h item id should match");
            Check(std::wstring(five_hour->GetItemName()) == L"TrafficMonitor Codex 5h", "5h item name should match");
            Check(std::wstring(five_hour->GetItemLableText()) == L"5h:", "5h label should avoid trim-prone whitespace");
            Check(std::wstring(five_hour->GetItemValueSampleText()) == L" 100% 4h 59m", "5h sample should follow countdown display config");
            Check(std::wstring(five_hour->GetItemValueText()) == L" ...", "5h initial value should include visible spacing before loading");
        }
        if (weekly != nullptr)
        {
            Check(std::wstring(weekly->GetItemId()) == L"CodexQuotaWeek", "weekly item id should match");
            Check(std::wstring(weekly->GetItemName()) == L"TrafficMonitor Codex Week", "weekly item name should match");
            Check(std::wstring(weekly->GetItemLableText()) == L"7d:", "weekly label should avoid trim-prone whitespace");
            Check(std::wstring(weekly->GetItemValueSampleText()) == L" 100% 6d 23h", "weekly sample should follow countdown display config");
            Check(std::wstring(weekly->GetItemValueText()) == L" ...", "weekly initial value should include visible spacing before loading");
        }

        if (RunLiveRefresh() && five_hour != nullptr && weekly != nullptr)
        {
            plugin->DataRequired();
            std::wstring five_hour_value;
            std::wstring weekly_value;
            for (int attempt = 0; attempt < 100; ++attempt)
            {
                Sleep(100);
                five_hour_value = five_hour->GetItemValueText();
                weekly_value = weekly->GetItemValueText();
                if (Contains(five_hour_value, L"%") && Contains(weekly_value, L"%"))
                {
                    break;
                }
            }

            Check(Contains(five_hour_value, L"%"), "live 5h plugin value should contain a percent");
            Check(StartsWith(five_hour_value, L" "), "live 5h plugin value should include visible spacing before the percent");
            Check(ContainsResetIndicator(five_hour_value), "live 5h plugin value should contain a reset countdown or reset time");
            Check(Contains(weekly_value, L"%"), "live weekly plugin value should contain a percent");
            Check(StartsWith(weekly_value, L" "), "live weekly plugin value should include visible spacing before the percent");
            Check(ContainsResetIndicator(weekly_value), "live weekly plugin value should contain a reset countdown or reset time");
        }
    }

    FreeLibrary(module);

    if (failures != 0)
    {
        std::cerr << failures << " smoke test(s) failed\n";
        return 1;
    }

    std::cout << "Plugin smoke tests passed\n";
    return 0;
}
