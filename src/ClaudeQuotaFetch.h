#pragma once

#include "ClaudeQuotaCore.h"

#include <optional>
#include <string>

namespace claudequota
{
struct FetchResult
{
    bool success{};
    UsageSnapshot usage;
    std::wstring error;
    int http_status{};
};

std::wstring GetDefaultOAuthCredentialsPath();
std::optional<OAuthCredentials> ReadClaudeCodeOAuthCredentials(std::wstring& error);
FetchResult FetchUsageSnapshot();
}
