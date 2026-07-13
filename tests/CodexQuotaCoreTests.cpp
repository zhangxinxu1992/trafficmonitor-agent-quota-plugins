#include "CodexQuotaCore.h"
#include "CodexQuotaFetch.h"

#include <iostream>
#include <string>
#include <Windows.h>

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

void TestParsesTokenCredentials()
{
    std::wstring error;
    const auto credentials = codexquota::ParseCredentialsJson(
        LR"({
            "tokens": {
                "access_token": "access-token",
                "refresh_token": "ignored-refresh-token",
                "account_id": "account-id"
            }
        })",
        error);

    Check(credentials.has_value(), "tokens credentials should parse");
    Check(credentials->access_token == L"access-token", "access token should parse");
    Check(credentials->account_id == L"account-id", "account id should parse");
    Check(error.empty(), "successful credential parse should not set error");
}

void TestPrefersOpenAiApiKey()
{
    std::wstring error;
    const auto credentials = codexquota::ParseCredentialsJson(
        LR"({
            "OPENAI_API_KEY": "root-key",
            "tokens": {
                "access_token": "access-token",
                "account_id": "account-id"
            }
        })",
        error);

    Check(credentials.has_value(), "OPENAI_API_KEY credentials should parse");
    Check(credentials->access_token == L"root-key", "OPENAI_API_KEY should be preferred");
    Check(credentials->account_id.empty(), "OPENAI_API_KEY should not reuse account id");
}

void TestRejectsMissingCredentials()
{
    std::wstring error;
    const auto credentials = codexquota::ParseCredentialsJson(LR"({"tokens": {}})", error);

    Check(!credentials.has_value(), "missing access token should fail");
    Check(!error.empty(), "missing access token should explain failure");
}

void TestNormalizesEnvironmentProxy()
{
    Check(codexquota::NormalizeProxyUrl(L" http://proxy.example:3128/ ") == L"proxy.example:3128",
        "HTTP proxy URL should normalize to a WinHTTP proxy name");
    Check(codexquota::NormalizeProxyUrl(L"HTTPS://proxy.example:8443") == L"proxy.example:8443",
        "HTTPS proxy URL normalization should be case insensitive");
    Check(codexquota::NormalizeProxyUrl(L"proxy.example:3128") == L"proxy.example:3128",
        "bare proxy name should be preserved");
    Check(!codexquota::NormalizeProxyUrl(L"socks5://proxy.example:1080").has_value(),
        "unsupported proxy scheme should be rejected");
    Check(!codexquota::NormalizeProxyUrl(L"http://user:password@proxy.example:3128").has_value(),
        "proxy credentials should not be accepted in environment URLs");
}

void TestParsesUsageWindows()
{
    std::wstring error;
    const auto usage = codexquota::ParseUsageJson(
        R"({
            "plan_type": "pro",
            "rate_limit": {
                "primary_window": {
                    "used_percent": 24,
                    "limit_window_seconds": 18000,
                    "reset_at": 1781798102
                },
                "secondary_window": {
                    "used_percent": "10",
                    "limit_window_seconds": 604800,
                    "reset_at": 1782363780
                }
            },
            "additional_rate_limits": [
                {
                    "limit_name": "GPT-5.3-Codex-Spark",
                    "metered_feature": "codex_bengalfox"
                }
            ]
        })",
        error);

    Check(usage.has_value(), "usage JSON should parse");
    Check(usage->plan_type == L"pro", "plan_type should parse");
    Check(usage->primary.present, "primary window should be present");
    Check(usage->primary.used_percent == 24.0, "primary used percent should parse");
    Check(usage->primary.limit_window_seconds == 18000, "primary window seconds should parse");
    Check(usage->primary.reset_at == 1781798102, "primary reset_at should parse");
    Check(usage->secondary.present, "secondary window should be present");
    Check(usage->secondary.used_percent == 10.0, "secondary string percent should parse");
    Check(usage->secondary.limit_window_seconds == 604800, "secondary window seconds should parse");
    Check(usage->secondary.reset_at == 1782363780, "secondary reset_at should parse");
    Check(error.empty(), "successful usage parse should not set error");
}

void TestClassifiesPrimaryOnlyWeeklyWindowByDuration()
{
    std::wstring error;
    const auto usage = codexquota::ParseUsageJson(
        R"({
            "plan_type": "pro",
            "rate_limit": {
                "primary_window": {
                    "used_percent": 12,
                    "limit_window_seconds": 604800,
                    "reset_at": 1784504645
                },
                "secondary_window": null
            }
        })",
        error);

    Check(usage.has_value(), "primary-only weekly usage JSON should parse");
    Check(!usage->primary.present, "a seven-day primary_window should not be exposed as the 5h window");
    Check(usage->secondary.present, "a seven-day primary_window should be exposed as the weekly window");
    Check(usage->secondary.used_percent == 12.0, "reclassified weekly used percent should be preserved");
    Check(usage->secondary.limit_window_seconds == 604800, "reclassified weekly duration should be preserved");
    Check(usage->secondary.reset_at == 1784504645, "reclassified weekly reset_at should be preserved");
    Check(error.empty(), "successful weekly reclassification should not set error");
}

void TestClassifiesSwappedRateWindowsByDuration()
{
    std::wstring error;
    const auto usage = codexquota::ParseUsageJson(
        R"({
            "plan_type": "pro",
            "rate_limit": {
                "primary_window": {
                    "used_percent": 20,
                    "limit_window_seconds": 604800,
                    "reset_at": 1784504645
                },
                "secondary_window": {
                    "used_percent": 30,
                    "limit_window_seconds": 18000,
                    "reset_at": 1784000000
                }
            }
        })",
        error);

    Check(usage.has_value(), "swapped rate-window usage JSON should parse");
    Check(usage->primary.present, "the five-hour duration should populate the 5h window regardless of source field");
    Check(usage->primary.used_percent == 30.0, "reclassified 5h used percent should be preserved");
    Check(usage->primary.limit_window_seconds == 18000, "reclassified 5h duration should be preserved");
    Check(usage->secondary.present, "the seven-day duration should populate the weekly window regardless of source field");
    Check(usage->secondary.used_percent == 20.0, "reclassified weekly used percent should be preserved after swapping");
    Check(usage->secondary.limit_window_seconds == 604800, "reclassified weekly duration should be preserved after swapping");
    Check(error.empty(), "successful swapped-window reclassification should not set error");
}

void TestFallsBackToRateWindowFieldNamesWhenDurationIsMissing()
{
    std::wstring error;
    const auto usage = codexquota::ParseUsageJson(
        R"({
            "rate_limit": {
                "primary_window": {
                    "used_percent": 40,
                    "reset_at": 1784000000
                },
                "secondary_window": {
                    "used_percent": 50,
                    "reset_at": 1784504645
                }
            }
        })",
        error);

    Check(usage.has_value(), "rate windows without durations should retain positional compatibility");
    Check(usage->primary.present, "duration-less primary_window should fall back to the 5h slot");
    Check(usage->primary.used_percent == 40.0, "duration-less primary used percent should be preserved");
    Check(usage->secondary.present, "duration-less secondary_window should fall back to the weekly slot");
    Check(usage->secondary.used_percent == 50.0, "duration-less secondary used percent should be preserved");
    Check(error.empty(), "successful positional fallback should not set error");
}

void TestDoesNotMislabelUnknownPositiveRateWindowDuration()
{
    std::wstring error;
    const auto usage = codexquota::ParseUsageJson(
        R"({
            "rate_limit": {
                "primary_window": {
                    "used_percent": 60,
                    "limit_window_seconds": 86400,
                    "reset_at": 1784000000
                }
            }
        })",
        error);

    Check(!usage.has_value(), "an unsupported positive duration should not be mislabeled as 5h or weekly");
    Check(error.find(L"supported quota window") != std::wstring::npos,
        "an unsupported positive duration should report that no supported window was found");
}

void TestParsesBusinessMonthlySpendControl()
{
    std::wstring error;
    const auto usage = codexquota::ParseUsageJson(
        R"({
            "plan_type": "business",
            "rate_limit": null,
            "additional_rate_limits": null,
            "credits": {
                "has_credits": true,
                "unlimited": false,
                "balance": null
            },
            "spend_control": {
                "reached": false,
                "individual_limit": {
                    "source": "workspace_spend_controls",
                    "limit": "12500",
                    "used": "375",
                    "remaining": "12125",
                    "used_percent": 3,
                    "remaining_percent": 97,
                    "reset_after_seconds": 1610856,
                    "reset_at": 1785542400
                }
            }
        })",
        error);

    Check(usage.has_value(), "business monthly usage JSON should parse");
    Check(usage->plan_type == L"business", "business plan_type should parse");
    Check(!usage->primary.present, "business response should not invent a 5h window");
    Check(!usage->secondary.present, "business response should not invent a weekly window");
    Check(usage->monthly.present, "business spend control should create a monthly window");
    Check(usage->monthly.used_percent == 3.0, "business monthly used percent should parse");
    Check(usage->monthly.reset_at == 1785542400, "business monthly reset_at should parse");
    Check(error.empty(), "successful business monthly parse should not set error");
}

void TestDerivesBusinessMonthlyPercent()
{
    std::wstring error;
    const auto from_remaining = codexquota::ParseUsageJson(
        R"({
            "plan_type": "enterprise",
            "spend_control": {
                "individual_limit": {
                    "remaining_percent": "75",
                    "reset_at": 1785542400
                }
            }
        })",
        error);

    Check(from_remaining.has_value(), "business monthly usage should derive percent from remaining percent");
    Check(from_remaining->monthly.used_percent == 25.0,
        "business monthly used percent should derive from remaining percent");

    error.clear();
    const auto from_amounts = codexquota::ParseUsageJson(
        R"({
            "plan_type": "enterprise",
            "spend_control": {
                "individual_limit": {
                    "limit": "2000",
                    "used": "500",
                    "reset_at": 1785542400
                }
            }
        })",
        error);

    Check(from_amounts.has_value(), "business monthly usage should derive percent from amounts");
    Check(from_amounts->monthly.used_percent == 25.0,
        "business monthly percent should derive from used and limit");
}

void TestFormatsUsedPercent()
{
    Check(codexquota::FormatPercent(24.4) == L"24%", "percent should round down below .5");
    Check(codexquota::FormatPercent(24.5) == L"25%", "percent should round half up");
    Check(codexquota::FormatPercent(-1.0) == L"0%", "negative percent should clamp");
    Check(codexquota::FormatPercent(125.0) == L"125%", "over-budget percent should be preserved");
}

void TestFormatsRemainingPercent()
{
    Check(codexquota::FormatRemainingPercent(24.4) == L"76%", "remaining percent should subtract from 100 and round");
    Check(codexquota::FormatRemainingPercent(24.5) == L"76%", "remaining percent should round half up");
    Check(codexquota::FormatRemainingPercent(-1.0) == L"100%", "negative used percent should leave full remaining quota");
    Check(codexquota::FormatRemainingPercent(125.0) == L"0%", "over-budget remaining percent should clamp to zero");
}

void TestFormatsRemainingWindowText()
{
    Check(codexquota::FormatRemainingWindowText(24.4, 1700000300, 1700000000) == L"76% 5m", "remaining window text should include reset countdown");
    Check(codexquota::FormatRemainingWindowText(31.0, 1700016200, 1700000000) == L"69% 4h 30m", "taskbar hour countdown should keep minutes");
    Check(codexquota::FormatRemainingWindowText(12.0, 1700522000, 1700000000) == L"88% 6d 1h", "taskbar day countdown should keep hours");
    Check(codexquota::FormatRemainingWindowText(10.0, 1700604800, 1700000000) == L"90% 1w", "weekly countdown should stay compact");
    Check(codexquota::FormatRemainingWindowText(24.4, 0, 1700000000) == L"76%", "missing reset time should omit countdown");
}

long long LocalTimestamp(int year, int month, int day, int hour, int minute)
{
    std::tm local{};
    local.tm_year = year - 1900;
    local.tm_mon = month - 1;
    local.tm_mday = day;
    local.tm_hour = hour;
    local.tm_min = minute;
    local.tm_isdst = -1;
    return static_cast<long long>(std::mktime(&local));
}

void TestParsesDisplayConfig()
{
    std::wstring error;
    const auto config = codexquota::ParseConfigJson(
        LR"({
            "quota_display": "used",
            "reset_display": "time",
            "show_reset_info": false
        })",
        error);

    Check(config.has_value(), "Codex display config should parse");
    Check(config->display.quota_display == codexquota::QuotaDisplayMode::Used, "Codex quota display should parse as used");
    Check(config->display.reset_display == codexquota::ResetDisplayMode::Time, "Codex reset display should parse as time");
    Check(!config->display.show_reset_info, "Codex reset info display flag should parse");
    Check(error.empty(), "successful Codex display config parse should not set error");

    error.clear();
    const auto default_config = codexquota::ParseConfigJson(L"{}", error);
    Check(default_config.has_value(), "empty Codex display config should parse");
    Check(default_config->display.show_reset_info, "Codex reset info should default to visible");
    Check(codexquota::SerializeConfigJson(*config).find(L"\"show_reset_info\": false") != std::wstring::npos,
        "Codex config serialization should persist hidden reset info");
}

void TestFormatsWindowTextWithDisplayOptions()
{
    codexquota::DisplayOptions options;
    const auto now = LocalTimestamp(2026, 6, 22, 10, 0);
    const auto same_day_reset = LocalTimestamp(2026, 6, 22, 18, 30);
    const auto next_day_reset = LocalTimestamp(2026, 6, 23, 8, 5);

    options.quota_display = codexquota::QuotaDisplayMode::Used;
    Check(codexquota::FormatWindowText(24.4, same_day_reset, now, options) == L"24% 8h 30m",
        "used display should show used percent with countdown");

    options.quota_display = codexquota::QuotaDisplayMode::Remaining;
    options.reset_display = codexquota::ResetDisplayMode::Time;
    Check(codexquota::FormatWindowText(24.4, same_day_reset, now, options) == L"76% 18:30",
        "same-day reset time should show local HH:MM");
    Check(codexquota::FormatWindowText(24.4, next_day_reset, now, options) == L"76% 06-23 08:05",
        "cross-day reset time should show local month-day and time");

    options.show_reset_info = false;
    Check(codexquota::FormatWindowText(24.4, next_day_reset, now, options) == L"76%",
        "hidden Codex reset info should leave only the quota percent");
}

void TestFormatsResourceGraphValueWithDisplayOptions()
{
    codexquota::DisplayOptions options;
    Check(codexquota::FormatResourceGraphValue(24.0, options) == 0.24f,
        "remaining display should graph used Codex quota");
    Check(codexquota::FormatResourceGraphValue(96.0, options) == 0.96f,
        "4 percent remaining Codex quota should graph 96 percent used");

    options.quota_display = codexquota::QuotaDisplayMode::Used;
    Check(codexquota::FormatResourceGraphValue(24.0, options) == 0.24f,
        "used display should graph used Codex quota");
    Check(codexquota::FormatResourceGraphValue(-5.0, options) == 0.0f,
        "negative Codex graph values should clamp to zero");
    Check(codexquota::FormatResourceGraphValue(125.0, options) == 1.0f,
        "over-budget Codex graph values should clamp to one");
}

void TestFormatsCountdown()
{
    Check(codexquota::FormatResetCountdown(1700000000, 1700000000) == L"now", "past reset should be now");
    Check(codexquota::FormatResetCountdown(1700000300, 1700000000) == L"5m", "minutes should format");
    Check(codexquota::FormatResetCountdown(1700007200, 1700000000) == L"2h", "whole hours should omit minutes");
    Check(codexquota::FormatResetCountdown(1700090000, 1700000000) == L"1d 1h", "days should format with remaining hours");
}

void TestLiveFetchWhenRequested()
{
    wchar_t flag[8]{};
    if (GetEnvironmentVariableW(L"TRAFFICMONITOR_CODEX_QUOTA_RUN_LIVE_TEST", flag, 8) == 0
        && GetEnvironmentVariableW(L"CODEX_QUOTA_RUN_LIVE_TEST", flag, 8) == 0)
    {
        return;
    }

    const auto result = codexquota::FetchUsageSnapshot();
    if (!result.success)
    {
        std::wcerr << L"LIVE ERROR: " << result.error << L" status=" << result.http_status << L'\n';
    }
    Check(result.success, "live Codex usage fetch should succeed when requested");
    Check(result.usage.primary.present || result.usage.secondary.present || result.usage.monthly.present,
        "live Codex usage should include a supported rate or monthly window");
    if (result.usage.primary.present && result.usage.primary.limit_window_seconds > 0)
    {
        Check(result.usage.primary.limit_window_seconds == 18000,
            "live Codex 5h slot should contain a five-hour window");
    }
    if (result.usage.secondary.present && result.usage.secondary.limit_window_seconds > 0)
    {
        Check(result.usage.secondary.limit_window_seconds == 604800,
            "live Codex weekly slot should contain a seven-day window");
    }
    if (result.usage.plan_type == L"business" || result.usage.plan_type == L"enterprise")
    {
        Check(result.usage.monthly.present, "live company Codex usage should include monthly spend control");
    }
}
}

int main()
{
    TestParsesTokenCredentials();
    TestPrefersOpenAiApiKey();
    TestRejectsMissingCredentials();
    TestNormalizesEnvironmentProxy();
    TestParsesUsageWindows();
    TestClassifiesPrimaryOnlyWeeklyWindowByDuration();
    TestClassifiesSwappedRateWindowsByDuration();
    TestFallsBackToRateWindowFieldNamesWhenDurationIsMissing();
    TestDoesNotMislabelUnknownPositiveRateWindowDuration();
    TestParsesBusinessMonthlySpendControl();
    TestDerivesBusinessMonthlyPercent();
    TestFormatsUsedPercent();
    TestFormatsRemainingPercent();
    TestFormatsRemainingWindowText();
    TestParsesDisplayConfig();
    TestFormatsWindowTextWithDisplayOptions();
    TestFormatsResourceGraphValueWithDisplayOptions();
    TestFormatsCountdown();
    TestLiveFetchWhenRequested();

    if (failures != 0)
    {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }

    std::cout << "All tests passed\n";
    return 0;
}
