#pragma once

#include "GitHubCopilotQuotaCore.h"

#include <string>

namespace githubcopilotquota
{
struct QuotaSnapshot
{
    PluginConfig config;
    Allowance allowance;
    UsagePeriod period;
    UsageReport usage;
    Quota quota;
    std::wstring username;
};

struct FetchResult
{
    bool success{};
    QuotaSnapshot snapshot;
    std::wstring error;
    int http_status{};
};

std::wstring GetDefaultConfigPath();
FetchResult FetchQuotaSnapshot();
}
