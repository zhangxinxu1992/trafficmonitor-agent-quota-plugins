#pragma once

#include "CodexQuotaCore.h"

#include <optional>
#include <string>

namespace claudequota
{
using DisplayOptions = codexquota::DisplayOptions;
using PluginConfig = codexquota::PluginConfig;
using QuotaDisplayMode = codexquota::QuotaDisplayMode;
using RateWindow = codexquota::RateWindow;
using ResetDisplayMode = codexquota::ResetDisplayMode;
using UsageSnapshot = codexquota::UsageSnapshot;

struct OAuthCredentials
{
    std::wstring access_token;
    std::wstring rate_limit_tier;
};

std::optional<OAuthCredentials> ParseOAuthCredentialsJson(const std::string& json, std::wstring& error);
std::optional<UsageSnapshot> ParseUsageJson(const std::string& json, std::wstring& error);
bool ApplySpendLimitJson(const std::string& json, long long now, UsageSnapshot& snapshot, std::wstring& error);
long long NextUtcMonthStart(long long now);

std::optional<PluginConfig> ParseConfigJson(const std::wstring& json, std::wstring& error);
std::wstring SerializeConfigJson(const PluginConfig& config);
std::wstring FormatWindowText(double used_percent, long long reset_at, long long now, const DisplayOptions& options);
float FormatResourceGraphValue(double used_percent, const DisplayOptions& options);
std::wstring FormatResetCountdown(long long reset_at, long long now);
std::wstring FormatResetTime(long long reset_at, long long now);
}
