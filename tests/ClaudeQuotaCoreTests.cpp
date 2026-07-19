#include "ClaudeQuotaCore.h"
#include "ClaudeQuotaFetch.h"

#include <Windows.h>

#include <cmath>
#include <ctime>
#include <iostream>
#include <string>
#include <vector>

namespace
{
int failures = 0;

struct FakeClaudeTransport
{
    std::vector<claudequota::ClaudeHttpRequest> requests;
    std::vector<claudequota::ClaudeHttpResponse> responses;
    size_t next_response{};
};

struct FakeCredentialStore
{
    bool succeed{true};
    std::vector<std::string> writes;
};

bool SendFakeClaudeRequest(
    const claudequota::ClaudeHttpRequest& request,
    claudequota::ClaudeHttpResponse& response,
    std::wstring& error,
    void* raw_context)
{
    auto* context = static_cast<FakeClaudeTransport*>(raw_context);
    context->requests.push_back(request);
    if (context->next_response >= context->responses.size())
    {
        error = L"Unexpected fake Claude request.";
        return false;
    }
    response = context->responses[context->next_response++];
    return true;
}

bool StoreFakeCredentials(const std::string& updated_json, std::wstring& error, void* raw_context)
{
    auto* context = static_cast<FakeCredentialStore*>(raw_context);
    context->writes.push_back(updated_json);
    if (!context->succeed)
    {
        error = L"Fake credential store failure.";
        return false;
    }
    return true;
}

void Check(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

long long UtcTime(int year, int month, int day, int hour = 0, int minute = 0)
{
    std::tm value{};
    value.tm_year = year - 1900;
    value.tm_mon = month - 1;
    value.tm_mday = day;
    value.tm_hour = hour;
    value.tm_min = minute;
    return static_cast<long long>(_mkgmtime64(&value));
}

void TestOAuthCredentialParsing()
{
    std::wstring error;
    const auto credentials = claudequota::ParseOAuthCredentialsJson(
        R"({"claudeAiOauth":{"accessToken":"oauth-token","refreshToken":"refresh-token","expiresAt":123456,"refreshTokenExpiresAt":654321,"clientId":"client-id","scopes":["user:profile","user:inference"],"rateLimitTier":"enterprise"}})",
        error);
    Check(credentials.has_value(), "Claude Code OAuth credentials should parse");
    Check(credentials && credentials->access_token == L"oauth-token", "OAuth access token should parse");
    Check(credentials && credentials->refresh_token == L"refresh-token", "OAuth refresh token should parse");
    Check(credentials && credentials->expires_at_ms == 123456, "OAuth access token expiry should parse");
    Check(credentials && credentials->refresh_token_expires_at_ms == 654321, "OAuth refresh token expiry should parse");
    Check(credentials && credentials->client_id == L"client-id", "OAuth client ID should parse");
    Check(credentials && credentials->scopes.size() == 2 && credentials->scopes[1] == L"user:inference",
        "OAuth scopes should parse");
    Check(credentials && credentials->rate_limit_tier == L"enterprise", "OAuth rate limit tier should parse");
}

void TestOAuthCredentialParsingIgnoresMcpTokens()
{
    std::wstring error;
    const auto credentials = claudequota::ParseOAuthCredentialsJson(
        R"({
            "mcpOAuth": {
                "ediprod|test": {"accessToken":"", "refreshToken":""},
                "github|test": {"accessToken":"mcp-token", "refreshToken":"mcp-refresh"}
            },
            "claudeAiOauth": {
                "accessToken":"claude-oauth-token",
                "rateLimitTier":"enterprise"
            }
        })",
        error);

    Check(credentials.has_value(), "Claude OAuth credentials should parse after unrelated MCP OAuth entries");
    Check(credentials && credentials->access_token == L"claude-oauth-token",
        "Claude OAuth parsing should use claudeAiOauth instead of the first accessToken in the file");
    Check(credentials && credentials->rate_limit_tier == L"enterprise",
        "Claude OAuth parsing should use the tier inside claudeAiOauth");
}

void TestOAuthCredentialParsingRejectsMcpOnlyCredentials()
{
    std::wstring error;
    const auto credentials = claudequota::ParseOAuthCredentialsJson(
        R"({"mcpOAuth":{"github|test":{"accessToken":"mcp-token"}}})",
        error);

    Check(!credentials.has_value(), "MCP OAuth credentials alone should not authenticate Claude usage");
    Check(error.find(L"claude auth login") != std::wstring::npos,
        "missing Claude OAuth credentials should recommend the current Claude Code login command");
}

void TestUsageParsing()
{
    const std::string json = R"({
        "five_hour":{"utilization":0.08,"resets_at":"2026-07-13T12:30:00Z"},
        "seven_day":{"utilization":42,"resets_at":"2026-07-18T00:00:00Z"}
    })";

    std::wstring error;
    const auto usage = claudequota::ParseUsageJson(json, error);
    Check(usage.has_value(), "usage response should parse");
    Check(usage && usage->primary.present, "5-hour window should be present");
    Check(usage && std::abs(usage->primary.used_percent - 8.0) < 0.001, "fractional utilization should normalize to percent");
    Check(usage && usage->primary.reset_at == UtcTime(2026, 7, 13, 12, 30), "5-hour reset should parse as UTC");
    Check(usage && usage->secondary.present && usage->secondary.used_percent == 42.0, "weekly percent should parse");
}

void TestSpendLimitParsing()
{
    claudequota::UsageSnapshot usage;
    std::wstring error;
    const auto now = UtcTime(2026, 7, 13, 10, 0);
    const bool parsed = claudequota::ApplySpendLimitJson(
        R"({"monthly_credit_limit":50000,"used_credits":4241,"currency":"USD","is_enabled":true})",
        now,
        usage,
        error);

    Check(parsed, "Enterprise spend limit should parse");
    Check(usage.monthly.present, "monthly window should be present");
    Check(std::abs(usage.monthly.used_percent - 8.482) < 0.001, "monthly used percent should derive from spend and limit");
    Check(usage.monthly.reset_at == UtcTime(2026, 8, 1), "monthly reset should be next UTC month boundary");

    claudequota::DisplayOptions options;
    options.show_reset_info = false;
    Check(claudequota::FormatWindowText(usage.monthly.used_percent, usage.monthly.reset_at, now, options) == L"92%", "remaining monthly text should round consistently");
}

void TestEmbeddedOAuthAliases()
{
    claudequota::UsageSnapshot usage;
    std::wstring error;
    const bool parsed = claudequota::ApplySpendLimitJson(
        R"({"monthly_limit":1000,"used_credits":250,"is_enabled":true})",
        UtcTime(2026, 7, 13),
        usage,
        error);
    Check(parsed && usage.monthly.used_percent == 25.0, "OAuth extra_usage aliases should parse");
}

void TestConfigParsing()
{
    std::wstring error;
    const auto config = claudequota::ParseConfigJson(
        L"{\"quota_display\":\"used\",\"reset_display\":\"time\",\"show_reset_info\":false}",
        error);
    Check(config.has_value(), "Claude display config should parse");
    Check(config && config->display.quota_display == claudequota::QuotaDisplayMode::Used, "used mode should parse");
    Check(config && !config->display.show_reset_info, "show_reset_info should parse");
}

std::string UsageResponse()
{
    return R"({"five_hour":{"utilization":25,"resets_at":"2026-07-17T10:00:00Z"}})";
}

void TestExpiredOAuthTokenRefreshesBeforeUsageRequest()
{
    const std::string credentials_json = R"({
        "mcpOAuth":{"github|test":{"accessToken":"mcp-token","refreshToken":"mcp-refresh"}},
        "claudeAiOauth":{
            "accessToken":"expired-access",
            "refreshToken":"old-refresh",
            "expiresAt":1000000,
            "clientId":"test-client",
            "scopes":["user:profile","user:inference"],
            "rateLimitTier":"enterprise"
        }
    })";
    FakeClaudeTransport transport;
    transport.responses = {
        {200, 0, R"({"access_token":"new-access","refresh_token":"new-refresh","expires_in":3600,"refresh_token_expires_in":7200})"},
        {200, 0, UsageResponse()}
    };
    FakeCredentialStore store;

    const auto result = claudequota::FetchUsageSnapshotFromCredentialsJson(
        credentials_json,
        2000000,
        SendFakeClaudeRequest,
        &transport,
        StoreFakeCredentials,
        &store);

    Check(result.success, "expired OAuth token should refresh and fetch usage");
    Check(transport.requests.size() == 2, "expired OAuth token should make one refresh and one usage request");
    Check(transport.requests.size() >= 1 && transport.requests[0].host == L"platform.claude.com"
        && transport.requests[0].method == L"POST" && transport.requests[0].path == L"/v1/oauth/token",
        "OAuth refresh should use the Claude Code token endpoint");
    Check(transport.requests.size() >= 1 && transport.requests[0].body.find("old-refresh") != std::string::npos
        && transport.requests[0].body.find("test-client") != std::string::npos
        && transport.requests[0].body.find("user:profile user:inference") != std::string::npos,
        "OAuth refresh request should contain the saved refresh token, client ID, and scopes");
    Check(transport.requests.size() >= 2 && transport.requests[1].authorization == L"Bearer new-access",
        "usage request should use the refreshed access token");
    Check(store.writes.size() == 1, "refreshed OAuth credentials should be persisted once");
    Check(store.writes.size() == 1 && store.writes[0].find("mcp-token") != std::string::npos
        && store.writes[0].find("mcp-refresh") != std::string::npos,
        "credential refresh should preserve unrelated MCP OAuth entries");

    std::wstring parse_error;
    const auto updated = store.writes.empty()
        ? std::optional<claudequota::OAuthCredentials>{}
        : claudequota::ParseOAuthCredentialsJson(store.writes[0], parse_error);
    Check(updated && updated->access_token == L"new-access" && updated->refresh_token == L"new-refresh",
        "credential refresh should persist rotated access and refresh tokens");
    Check(updated && updated->expires_at_ms == 5600000 && updated->refresh_token_expires_at_ms == 9200000,
        "credential refresh should persist absolute token expiries");
}

void TestUnauthorizedUsageRefreshesOnceAndRetries()
{
    const std::string credentials_json = R"({"claudeAiOauth":{
        "accessToken":"rejected-access","refreshToken":"refresh-token","expiresAt":10000000
    }})";
    FakeClaudeTransport transport;
    transport.responses = {
        {401, 0, ""},
        {200, 0, R"({"access_token":"replacement-access","expires_in":3600})"},
        {200, 0, UsageResponse()}
    };
    FakeCredentialStore store;

    const auto result = claudequota::FetchUsageSnapshotFromCredentialsJson(
        credentials_json,
        2000000,
        SendFakeClaudeRequest,
        &transport,
        StoreFakeCredentials,
        &store);

    Check(result.success, "HTTP 401 should refresh OAuth credentials and retry usage once");
    Check(transport.requests.size() == 3 && transport.requests[0].path == L"/api/oauth/usage"
        && transport.requests[1].path == L"/v1/oauth/token" && transport.requests[2].path == L"/api/oauth/usage",
        "HTTP 401 recovery should issue usage, refresh, then one usage retry");
    Check(transport.requests.size() == 3 && transport.requests[2].authorization == L"Bearer replacement-access",
        "HTTP 401 retry should use the replacement access token");
    Check(transport.requests.size() == 3
        && transport.requests[1].body.find("9d1c250a-e61b-44d9-88ed-5944d1962f5e") != std::string::npos
        && transport.requests[1].body.find("user:sessions:claude_code") != std::string::npos,
        "OAuth refresh should use Claude Code defaults when client ID or scopes are absent");
    Check(store.writes.size() == 1, "HTTP 401 recovery should persist refreshed credentials once");
}

void TestRateLimitPropagatesRetryAfterWithoutRefreshing()
{
    const std::string credentials_json = R"({"claudeAiOauth":{
        "accessToken":"valid-access","refreshToken":"refresh-token","expiresAt":10000000
    }})";
    FakeClaudeTransport transport;
    transport.responses = {{429, 457, R"({"type":"error","error":{"type":"rate_limit_error"}})"}};
    FakeCredentialStore store;

    const auto result = claudequota::FetchUsageSnapshotFromCredentialsJson(
        credentials_json,
        2000000,
        SendFakeClaudeRequest,
        &transport,
        StoreFakeCredentials,
        &store);

    Check(!result.success && result.http_status == 429, "HTTP 429 should remain a rate-limit failure");
    Check(result.retry_after_seconds == 457, "HTTP 429 should propagate Retry-After seconds to the plugin scheduler");
    Check(result.error.find(L"457 seconds") != std::wstring::npos, "HTTP 429 error should explain the server retry delay");
    Check(transport.requests.size() == 1 && transport.requests[0].path == L"/api/oauth/usage",
        "HTTP 429 should not trigger an OAuth token refresh");
    Check(store.writes.empty(), "HTTP 429 should not rewrite OAuth credentials");
}

void TestCredentialStoreFailureStopsBeforeUsageRequest()
{
    const std::string credentials_json = R"({"claudeAiOauth":{
        "accessToken":"expired-access","refreshToken":"refresh-token","expiresAt":1000000
    }})";
    FakeClaudeTransport transport;
    transport.responses = {{200, 0, R"({"access_token":"new-access","expires_in":3600})"}};
    FakeCredentialStore store;
    store.succeed = false;

    const auto result = claudequota::FetchUsageSnapshotFromCredentialsJson(
        credentials_json,
        2000000,
        SendFakeClaudeRequest,
        &transport,
        StoreFakeCredentials,
        &store);

    Check(!result.success && result.error == L"Fake credential store failure.",
        "credential store failure should be returned without using an unpersisted token");
    Check(transport.requests.size() == 1, "credential store failure should stop before the usage request");
}

void RunLiveTestIfRequested()
{
    wchar_t value[8]{};
    if (GetEnvironmentVariableW(L"TRAFFICMONITOR_CLAUDE_QUOTA_RUN_LIVE_TEST", value, 8) == 0
        || std::wstring(value) != L"1")
    {
        return;
    }

    const auto result = claudequota::FetchUsageSnapshot();
    if (!result.success)
    {
        std::wcerr << L"FAIL: live Claude fetch: " << result.error << L'\n';
        ++failures;
        return;
    }
    Check(result.usage.primary.present || result.usage.secondary.present || result.usage.monthly.present,
        "live Claude fetch should return at least one window");
}
}

int wmain()
{
    TestOAuthCredentialParsing();
    TestOAuthCredentialParsingIgnoresMcpTokens();
    TestOAuthCredentialParsingRejectsMcpOnlyCredentials();
    TestUsageParsing();
    TestSpendLimitParsing();
    TestEmbeddedOAuthAliases();
    TestConfigParsing();
    TestExpiredOAuthTokenRefreshesBeforeUsageRequest();
    TestUnauthorizedUsageRefreshesOnceAndRetries();
    TestRateLimitPropagatesRetryAfterWithoutRefreshing();
    TestCredentialStoreFailureStopsBeforeUsageRequest();
    RunLiveTestIfRequested();

    if (failures != 0)
    {
        std::cerr << failures << " Claude quota test(s) failed.\n";
        return 1;
    }
    std::cout << "Claude quota tests passed.\n";
    return 0;
}
