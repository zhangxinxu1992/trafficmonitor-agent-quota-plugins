#include "ClaudeQuotaCore.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <ctime>

namespace
{
std::string Trim(const std::string& value)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
    {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::wstring Trim(const std::wstring& value)
{
    const auto first = value.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos)
    {
        return {};
    }
    const auto last = value.find_last_not_of(L" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::wstring Utf8ToWide(const std::string& value)
{
    if (value.empty())
    {
        return {};
    }
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0)
    {
        return std::wstring(value.begin(), value.end());
    }
    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), length);
    return result;
}

std::optional<std::string> FindJsonStringValue(const std::string& json, const std::string& key)
{
    const auto key_pos = json.find("\"" + key + "\"");
    if (key_pos == std::string::npos)
    {
        return std::nullopt;
    }
    const auto colon_pos = json.find(':', key_pos + key.size() + 2);
    if (colon_pos == std::string::npos)
    {
        return std::nullopt;
    }
    auto value_pos = json.find_first_not_of(" \t\r\n", colon_pos + 1);
    if (value_pos == std::string::npos || json[value_pos] != '"')
    {
        return std::nullopt;
    }
    ++value_pos;

    std::string value;
    bool escaped = false;
    for (auto index = value_pos; index < json.size(); ++index)
    {
        const char ch = json[index];
        if (escaped)
        {
            switch (ch)
            {
            case 'n': value.push_back('\n'); break;
            case 'r': value.push_back('\r'); break;
            case 't': value.push_back('\t'); break;
            default: value.push_back(ch); break;
            }
            escaped = false;
        }
        else if (ch == '\\')
        {
            escaped = true;
        }
        else if (ch == '"')
        {
            return value;
        }
        else
        {
            value.push_back(ch);
        }
    }
    return std::nullopt;
}

std::optional<std::string> FindJsonObject(const std::string& json, const std::string& key)
{
    const auto key_pos = json.find("\"" + key + "\"");
    if (key_pos == std::string::npos)
    {
        return std::nullopt;
    }
    const auto colon_pos = json.find(':', key_pos + key.size() + 2);
    if (colon_pos == std::string::npos)
    {
        return std::nullopt;
    }
    const auto start = json.find_first_not_of(" \t\r\n", colon_pos + 1);
    if (start == std::string::npos || json[start] != '{')
    {
        return std::nullopt;
    }

    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (auto index = start; index < json.size(); ++index)
    {
        const char ch = json[index];
        if (in_string)
        {
            if (escaped)
            {
                escaped = false;
            }
            else if (ch == '\\')
            {
                escaped = true;
            }
            else if (ch == '"')
            {
                in_string = false;
            }
            continue;
        }
        if (ch == '"')
        {
            in_string = true;
        }
        else if (ch == '{')
        {
            ++depth;
        }
        else if (ch == '}' && --depth == 0)
        {
            return json.substr(start, index - start + 1);
        }
    }
    return std::nullopt;
}

std::optional<std::string> FindJsonScalarText(const std::string& json, const std::string& key)
{
    const auto key_pos = json.find("\"" + key + "\"");
    if (key_pos == std::string::npos)
    {
        return std::nullopt;
    }
    const auto colon_pos = json.find(':', key_pos + key.size() + 2);
    if (colon_pos == std::string::npos)
    {
        return std::nullopt;
    }
    const auto value_pos = json.find_first_not_of(" \t\r\n", colon_pos + 1);
    if (value_pos == std::string::npos)
    {
        return std::nullopt;
    }
    const auto end = json.find_first_of(",}\r\n\t ", value_pos);
    return Trim(json.substr(value_pos, end == std::string::npos ? std::string::npos : end - value_pos));
}

std::optional<double> FindJsonDouble(const std::string& json, const std::string& key)
{
    const auto value = FindJsonScalarText(json, key);
    if (!value.has_value())
    {
        return std::nullopt;
    }
    try
    {
        return std::stod(*value);
    }
    catch (...)
    {
        return std::nullopt;
    }
}

std::optional<bool> FindJsonBool(const std::string& json, const std::string& key)
{
    const auto value = FindJsonScalarText(json, key);
    if (value == "true")
    {
        return true;
    }
    if (value == "false")
    {
        return false;
    }
    return std::nullopt;
}

std::optional<long long> ParseIso8601(const std::string& value)
{
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (sscanf_s(value.c_str(), "%d-%d-%dT%d:%d:%d", &year, &month, &day, &hour, &minute, &second) != 6)
    {
        return std::nullopt;
    }

    std::tm utc{};
    utc.tm_year = year - 1900;
    utc.tm_mon = month - 1;
    utc.tm_mday = day;
    utc.tm_hour = hour;
    utc.tm_min = minute;
    utc.tm_sec = second;
    auto result = static_cast<long long>(_mkgmtime64(&utc));
    if (result < 0)
    {
        return std::nullopt;
    }

    const auto time_zone_pos = value.find_first_of("Z+-", 19);
    if (time_zone_pos != std::string::npos && value[time_zone_pos] != 'Z')
    {
        int offset_hour = 0;
        int offset_minute = 0;
        if (sscanf_s(value.c_str() + time_zone_pos + 1, "%d:%d", &offset_hour, &offset_minute) == 2)
        {
            const long long offset = static_cast<long long>(offset_hour * 60 + offset_minute) * 60;
            result += value[time_zone_pos] == '+' ? -offset : offset;
        }
    }
    return result;
}

double NormalizeUtilization(double utilization)
{
    if (utilization > 0.0 && utilization <= 1.0)
    {
        utilization *= 100.0;
    }
    if (!std::isfinite(utilization))
    {
        return 0.0;
    }
    return std::clamp(utilization, 0.0, 100.0);
}

std::optional<claudequota::RateWindow> ParseUsageWindow(const std::string& json, const std::string& key, int window_seconds)
{
    const auto object = FindJsonObject(json, key);
    if (!object.has_value())
    {
        return std::nullopt;
    }
    const auto utilization = FindJsonDouble(*object, "utilization");
    if (!utilization.has_value())
    {
        return std::nullopt;
    }

    claudequota::RateWindow window;
    window.present = true;
    window.used_percent = NormalizeUtilization(*utilization);
    window.limit_window_seconds = window_seconds;
    if (const auto reset = FindJsonStringValue(*object, "resets_at"))
    {
        window.reset_at = ParseIso8601(*reset).value_or(0);
    }
    else if (const auto camel_reset = FindJsonStringValue(*object, "resetsAt"))
    {
        window.reset_at = ParseIso8601(*camel_reset).value_or(0);
    }
    return window;
}

void ReplaceAll(std::wstring& value, const std::wstring& old_text, const std::wstring& new_text)
{
    size_t position = 0;
    while ((position = value.find(old_text, position)) != std::wstring::npos)
    {
        value.replace(position, old_text.size(), new_text);
        position += new_text.size();
    }
}
}

namespace claudequota
{
std::optional<OAuthCredentials> ParseOAuthCredentialsJson(const std::string& json, std::wstring& error)
{
    error.clear();
    const auto access_token = FindJsonStringValue(json, "accessToken");
    if (!access_token.has_value() || Trim(*access_token).empty())
    {
        error = L"Claude OAuth credentials were not found. Run claude login or configure a web sessionKey.";
        return std::nullopt;
    }

    OAuthCredentials credentials;
    credentials.access_token = Utf8ToWide(Trim(*access_token));
    if (const auto tier = FindJsonStringValue(json, "rateLimitTier"))
    {
        credentials.rate_limit_tier = Utf8ToWide(Trim(*tier));
    }
    return credentials;
}

std::optional<AccountInfo> ParseAccountJson(const std::string& json, std::wstring& error)
{
    error.clear();
    AccountInfo account;

    if (const auto organization = FindJsonObject(json, "organization"))
    {
        if (const auto uuid = FindJsonStringValue(*organization, "uuid"))
        {
            account.organization_id = Utf8ToWide(Trim(*uuid));
        }
    }
    if (account.organization_id.empty())
    {
        if (const auto uuid = FindJsonStringValue(json, "uuid"))
        {
            account.organization_id = Utf8ToWide(Trim(*uuid));
        }
    }
    if (const auto plan = FindJsonStringValue(json, "rate_limit_tier"))
    {
        account.plan_type = Utf8ToWide(*plan);
    }
    if (account.plan_type.empty())
    {
        if (const auto plan = FindJsonStringValue(json, "subscription_type"))
        {
            account.plan_type = Utf8ToWide(*plan);
        }
    }

    if (account.organization_id.empty())
    {
        error = L"Claude account response does not contain an organization id.";
        return std::nullopt;
    }
    return account;
}

std::optional<UsageSnapshot> ParseUsageJson(const std::string& json, std::wstring& error)
{
    error.clear();
    UsageSnapshot snapshot;
    if (const auto primary = ParseUsageWindow(json, "five_hour", 5 * 60 * 60))
    {
        snapshot.primary = *primary;
    }
    else if (const auto camel_primary = ParseUsageWindow(json, "fiveHour", 5 * 60 * 60))
    {
        snapshot.primary = *camel_primary;
    }
    if (const auto weekly = ParseUsageWindow(json, "seven_day", 7 * 24 * 60 * 60))
    {
        snapshot.secondary = *weekly;
    }
    else if (const auto camel_weekly = ParseUsageWindow(json, "sevenDay", 7 * 24 * 60 * 60))
    {
        snapshot.secondary = *camel_weekly;
    }
    return snapshot;
}

bool ApplySpendLimitJson(const std::string& json, long long now, UsageSnapshot& snapshot, std::wstring& error)
{
    error.clear();
    if (const auto enabled = FindJsonBool(json, "is_enabled"); enabled.has_value() && !*enabled)
    {
        return false;
    }

    auto limit = FindJsonDouble(json, "monthly_credit_limit");
    if (!limit.has_value())
    {
        limit = FindJsonDouble(json, "monthly_limit");
    }
    if (!limit.has_value())
    {
        limit = FindJsonDouble(json, "limit");
    }

    auto used = FindJsonDouble(json, "used_credits");
    if (!used.has_value())
    {
        used = FindJsonDouble(json, "used");
    }

    if (!limit.has_value() || !used.has_value() || *limit <= 0.0)
    {
        return false;
    }

    snapshot.monthly.present = true;
    snapshot.monthly.used_percent = std::clamp(*used / *limit * 100.0, 0.0, 100.0);
    snapshot.monthly.limit_window_seconds = 0;
    if (const auto reset = FindJsonStringValue(json, "resets_at"))
    {
        snapshot.monthly.reset_at = ParseIso8601(*reset).value_or(NextUtcMonthStart(now));
    }
    else if (const auto camel_reset = FindJsonStringValue(json, "resetsAt"))
    {
        snapshot.monthly.reset_at = ParseIso8601(*camel_reset).value_or(NextUtcMonthStart(now));
    }
    else
    {
        snapshot.monthly.reset_at = NextUtcMonthStart(now);
    }
    return true;
}

std::optional<std::wstring> NormalizeSessionKeyInput(const std::wstring& input, std::wstring& error)
{
    error.clear();
    auto value = Trim(input);
    if (value.empty())
    {
        error = L"Claude sessionKey is empty.";
        return std::nullopt;
    }

    const auto cookie_pos = value.find(L"sessionKey=");
    if (cookie_pos != std::wstring::npos)
    {
        const auto start = cookie_pos + 11;
        const auto end = value.find(L';', start);
        value = Trim(value.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start));
    }
    else if (value.find(L'=') != std::wstring::npos || value.find(L';') != std::wstring::npos)
    {
        error = L"Cookie input does not contain sessionKey.";
        return std::nullopt;
    }

    if (value.size() >= 2 && value.front() == L'"' && value.back() == L'"')
    {
        value = value.substr(1, value.size() - 2);
    }
    if (value.empty() || value.find_first_of(L"\r\n\0") != std::wstring::npos)
    {
        error = L"Claude sessionKey has an invalid value.";
        return std::nullopt;
    }
    return value;
}

long long NextUtcMonthStart(long long now)
{
    const auto current = static_cast<__time64_t>(now);
    std::tm utc{};
    if (_gmtime64_s(&utc, &current) != 0)
    {
        return 0;
    }
    utc.tm_mday = 1;
    utc.tm_hour = 0;
    utc.tm_min = 0;
    utc.tm_sec = 0;
    ++utc.tm_mon;
    return static_cast<long long>(_mkgmtime64(&utc));
}

std::optional<PluginConfig> ParseConfigJson(const std::wstring& json, std::wstring& error)
{
    auto config = codexquota::ParseConfigJson(json, error);
    ReplaceAll(error, L"Codex", L"Claude");
    return config;
}

std::wstring SerializeConfigJson(const PluginConfig& config)
{
    return codexquota::SerializeConfigJson(config);
}

std::wstring FormatWindowText(double used_percent, long long reset_at, long long now, const DisplayOptions& options)
{
    return codexquota::FormatWindowText(used_percent, reset_at, now, options);
}

float FormatResourceGraphValue(double used_percent, const DisplayOptions& options)
{
    return codexquota::FormatResourceGraphValue(used_percent, options);
}

std::wstring FormatResetCountdown(long long reset_at, long long now)
{
    return codexquota::FormatResetCountdown(reset_at, now);
}

std::wstring FormatResetTime(long long reset_at, long long now)
{
    return codexquota::FormatResetTime(reset_at, now);
}
}
