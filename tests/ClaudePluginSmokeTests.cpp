#include "PluginInterface.h"

#include <Windows.h>

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
}

int wmain()
{
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
