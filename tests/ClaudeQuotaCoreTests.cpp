#include "ClaudeQuotaCore.h"
#include "ClaudeQuotaFetch.h"

#include <Windows.h>

#include <cmath>
#include <ctime>
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

void TestSessionKeyNormalization()
{
    std::wstring error;
    auto value = claudequota::NormalizeSessionKeyInput(L"  sk-test-value  ", error);
    Check(value == L"sk-test-value", "raw sessionKey should be trimmed");

    value = claudequota::NormalizeSessionKeyInput(L"other=x; sessionKey=sk-cookie-value; last=y", error);
    Check(value == L"sk-cookie-value", "Cookie header should yield sessionKey only");

    value = claudequota::NormalizeSessionKeyInput(L"other=x; last=y", error);
    Check(!value.has_value() && error.find(L"sessionKey") != std::wstring::npos, "unrelated Cookie header should fail");
}

void TestAccountParsing()
{
    const std::string json = R"({
        "email_address":"user@example.com",
        "rate_limit_tier":"enterprise",
        "memberships":[{"organization":{"uuid":"org-123"}}]
    })";

    std::wstring error;
    const auto account = claudequota::ParseAccountJson(json, error);
    Check(account.has_value(), "account response should parse");
    Check(account && account->organization_id == L"org-123", "organization uuid should parse");
    Check(account && account->plan_type == L"enterprise", "plan type should parse");
}

void TestOAuthCredentialParsing()
{
    std::wstring error;
    const auto credentials = claudequota::ParseOAuthCredentialsJson(
        R"({"claudeAiOauth":{"accessToken":"oauth-token","rateLimitTier":"enterprise"}})",
        error);
    Check(credentials.has_value(), "Claude Code OAuth credentials should parse");
    Check(credentials && credentials->access_token == L"oauth-token", "OAuth access token should parse");
    Check(credentials && credentials->rate_limit_tier == L"enterprise", "OAuth rate limit tier should parse");
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
    TestSessionKeyNormalization();
    TestAccountParsing();
    TestOAuthCredentialParsing();
    TestUsageParsing();
    TestSpendLimitParsing();
    TestEmbeddedOAuthAliases();
    TestConfigParsing();
    RunLiveTestIfRequested();

    if (failures != 0)
    {
        std::cerr << failures << " Claude quota test(s) failed.\n";
        return 1;
    }
    std::cout << "Claude quota tests passed.\n";
    return 0;
}
