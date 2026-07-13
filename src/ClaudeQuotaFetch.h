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

std::wstring GetClaudeSessionCredentialTarget();
std::optional<std::wstring> ReadStoredSessionKey(std::wstring& error);
bool WriteStoredSessionKey(const std::wstring& input, std::wstring& error);
bool DeleteStoredSessionKey(std::wstring& error);
bool HasStoredSessionKey();
std::wstring GetDefaultOAuthCredentialsPath();
FetchResult FetchUsageSnapshot();
}
