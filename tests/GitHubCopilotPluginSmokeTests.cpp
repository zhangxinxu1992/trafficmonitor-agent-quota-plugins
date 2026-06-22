#include "PluginInterface.h"

#include <Windows.h>
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

bool StartsWith(const std::wstring& value, const wchar_t* prefix)
{
    const std::wstring prefix_value(prefix);
    return value.size() >= prefix_value.size()
        && value.compare(0, prefix_value.size(), prefix_value) == 0;
}
}

int main()
{
    const auto dll_path = CurrentExeDir() + L"\\TrafficMonitorGitHubCopilotQuota.dll";
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
        Check(std::wstring(plugin->GetInfo(ITMPlugin::TMI_NAME)) == L"GitHub Copilot Quota", "plugin name should match");
        Check(std::wstring(plugin->GetInfo(ITMPlugin::TMI_DESCRIPTION)) == L"Displays remaining GitHub Copilot monthly AI Credits.", "plugin description should match");

        IPluginItem* item = plugin->GetItem(0);
        Check(item != nullptr, "first item should exist");
        Check(plugin->GetItem(1) == nullptr, "second item should not exist");

        if (item != nullptr)
        {
            Check(std::wstring(item->GetItemId()) == L"GitHubCopilotQuotaAI", "item id should match");
            Check(std::wstring(item->GetItemName()) == L"GitHub Copilot AI Credits", "item name should match");
            Check(std::wstring(item->GetItemLableText()) == L"GC:", "label should avoid trim-prone whitespace");
            Check(std::wstring(item->GetItemValueSampleText()) == L" 100% 20.0kcr 31d", "sample should reserve value-leading spacing and full countdown width");

            const std::wstring initial_value(item->GetItemValueText());
            Check(initial_value == L" ...", "initial value should include visible spacing before loading");
            Check(StartsWith(initial_value, L" "), "initial value should start with visible spacing");
        }
    }

    FreeLibrary(module);

    if (failures != 0)
    {
        std::cerr << failures << " GitHub Copilot plugin smoke test(s) failed\n";
        return 1;
    }

    std::cout << "GitHub Copilot plugin smoke tests passed\n";
    return 0;
}
