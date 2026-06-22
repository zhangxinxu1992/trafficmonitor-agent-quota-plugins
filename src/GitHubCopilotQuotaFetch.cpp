#include "GitHubCopilotQuotaFetch.h"

namespace githubcopilotquota
{
std::wstring GetDefaultConfigPath()
{
    return L"TrafficMonitorGitHubCopilotQuota\\config.json";
}

FetchResult FetchQuotaSnapshot()
{
    FetchResult result;
    result.success = false;
    result.error = L"GitHub Copilot quota fetch is not implemented.";
    return result;
}
}
