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
    int retry_after_seconds{};
};

struct ClaudeHttpRequest
{
    std::wstring host;
    std::wstring method;
    std::wstring path;
    std::wstring authorization;
    std::wstring accept;
    std::wstring anthropic_beta;
    std::wstring content_type;
    std::string body;
};

struct ClaudeHttpResponse
{
    int http_status{};
    int retry_after_seconds{};
    std::string body;
};

using ClaudeHttpRequestCallback = bool (*)(
    const ClaudeHttpRequest& request,
    ClaudeHttpResponse& response,
    std::wstring& error,
    void* context);
using OAuthCredentialsStoreCallback = bool (*)(
    const std::string& updated_json,
    std::wstring& error,
    void* context);

std::wstring GetDefaultOAuthCredentialsPath();
std::optional<OAuthCredentials> ReadClaudeCodeOAuthCredentials(std::wstring& error);
FetchResult FetchUsageSnapshotFromCredentialsJson(
    const std::string& credentials_json,
    long long now_ms,
    ClaudeHttpRequestCallback request_callback,
    void* request_context,
    OAuthCredentialsStoreCallback store_callback,
    void* store_context);
FetchResult FetchUsageSnapshot();
}
