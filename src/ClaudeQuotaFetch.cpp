#include "ClaudeQuotaFetch.h"
#include "PluginVersion.h"

#include <Windows.h>
#include <wincred.h>
#include <winhttp.h>

#include <ctime>
#include <string>

namespace
{
constexpr const wchar_t* kClaudeHost = L"claude.ai";
constexpr const wchar_t* kOAuthHost = L"api.anthropic.com";
constexpr const wchar_t* kCredentialTarget = L"TrafficMonitorClaudeQuota:ClaudeWebSession";

struct HttpHandle
{
    HINTERNET value{};

    explicit HttpHandle(HINTERNET handle = nullptr) : value(handle) {}
    HttpHandle(const HttpHandle&) = delete;
    HttpHandle& operator=(const HttpHandle&) = delete;

    ~HttpHandle()
    {
        if (value != nullptr)
        {
            WinHttpCloseHandle(value);
        }
    }

    operator HINTERNET() const
    {
        return value;
    }
};

struct HttpResponse
{
    int status{};
    std::string body;
};

std::wstring WindowsErrorMessage(DWORD error_code)
{
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error_code,
        0,
        reinterpret_cast<LPWSTR>(&buffer),
        0,
        nullptr);
    if (length == 0 || buffer == nullptr)
    {
        return L"Windows error " + std::to_wstring(error_code);
    }

    std::wstring message(buffer, length);
    LocalFree(buffer);
    while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n' || message.back() == L'.'))
    {
        message.pop_back();
    }
    return message;
}

std::wstring GetEnvVar(const wchar_t* name)
{
    const DWORD length = GetEnvironmentVariableW(name, nullptr, 0);
    if (length == 0)
    {
        return {};
    }
    std::wstring value(length, L'\0');
    const DWORD written = GetEnvironmentVariableW(name, value.data(), length);
    if (written == 0)
    {
        return {};
    }
    value.resize(written);
    return value;
}

std::wstring GetEnvironmentProxy()
{
    for (const auto* name : {L"HTTPS_PROXY", L"HTTP_PROXY"})
    {
        if (const auto proxy = codexquota::NormalizeProxyUrl(GetEnvVar(name)))
        {
            return *proxy;
        }
    }
    return {};
}

std::wstring JoinPath(std::wstring base, const wchar_t* child)
{
    if (!base.empty() && base.back() != L'\\' && base.back() != L'/')
    {
        base.push_back(L'\\');
    }
    base += child;
    return base;
}

bool ReadFileBytes(const std::wstring& path, std::string& content, std::wstring& error)
{
    const HANDLE file = CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        error = L"Failed to open " + path + L": " + WindowsErrorMessage(GetLastError());
        return false;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 || size.QuadPart > 1024 * 1024)
    {
        CloseHandle(file);
        error = L"Invalid Claude credentials file size.";
        return false;
    }
    content.assign(static_cast<size_t>(size.QuadPart), '\0');
    DWORD bytes_read = 0;
    const BOOL read = content.empty()
        || ReadFile(file, content.data(), static_cast<DWORD>(content.size()), &bytes_read, nullptr);
    CloseHandle(file);
    if (!read)
    {
        error = L"Failed to read " + path + L": " + WindowsErrorMessage(GetLastError());
        return false;
    }
    content.resize(bytes_read);
    return true;
}

bool ReadResponseBody(HINTERNET request, std::string& body, std::wstring& error)
{
    body.clear();
    while (true)
    {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available))
        {
            error = L"Failed to query Claude response data: " + WindowsErrorMessage(GetLastError());
            return false;
        }
        if (available == 0)
        {
            return true;
        }
        std::string buffer(available, '\0');
        DWORD bytes_read = 0;
        if (!WinHttpReadData(request, buffer.data(), available, &bytes_read))
        {
            error = L"Failed to read Claude response data: " + WindowsErrorMessage(GetLastError());
            return false;
        }
        body.append(buffer.data(), bytes_read);
    }
}

bool QueryStatusCode(HINTERNET request, int& status_code, std::wstring& error)
{
    DWORD status = 0;
    DWORD size = sizeof(status);
    if (!WinHttpQueryHeaders(
            request,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &status,
            &size,
            WINHTTP_NO_HEADER_INDEX))
    {
        error = L"Failed to read Claude HTTP status: " + WindowsErrorMessage(GetLastError());
        return false;
    }
    status_code = static_cast<int>(status);
    return true;
}

bool SendGet(
    HINTERNET connect,
    const std::wstring& path,
    const std::wstring& session_key,
    HttpResponse& response,
    std::wstring& error)
{
    HttpHandle request(WinHttpOpenRequest(
        connect,
        L"GET",
        path.c_str(),
        nullptr,
        L"https://claude.ai/settings/usage",
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE));
    if (request.value == nullptr)
    {
        error = L"Failed to create Claude request: " + WindowsErrorMessage(GetLastError());
        return false;
    }

    WinHttpSetTimeouts(request, 10000, 10000, 10000, 30000);
    std::wstring headers = L"Accept: application/json\r\n";
    headers += L"Cookie: sessionKey=" + session_key + L"\r\n";
    headers += L"Origin: https://claude.ai\r\n";
    headers += L"anthropic-client-platform: web_claude_ai\r\n";
    if (!WinHttpAddRequestHeaders(request, headers.c_str(), static_cast<DWORD>(headers.size()), WINHTTP_ADDREQ_FLAG_ADD))
    {
        error = L"Failed to add Claude request headers: " + WindowsErrorMessage(GetLastError());
        return false;
    }
    if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
    {
        error = L"Failed to send Claude request: " + WindowsErrorMessage(GetLastError());
        return false;
    }
    if (!WinHttpReceiveResponse(request, nullptr))
    {
        error = L"Failed to receive Claude response: " + WindowsErrorMessage(GetLastError());
        return false;
    }
    if (!QueryStatusCode(request, response.status, error))
    {
        return false;
    }
    return ReadResponseBody(request, response.body, error);
}

bool SendOAuthGet(
    HINTERNET connect,
    const std::wstring& access_token,
    HttpResponse& response,
    std::wstring& error)
{
    HttpHandle request(WinHttpOpenRequest(
        connect,
        L"GET",
        L"/api/oauth/usage",
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE));
    if (request.value == nullptr)
    {
        error = L"Failed to create Claude OAuth request: " + WindowsErrorMessage(GetLastError());
        return false;
    }
    WinHttpSetTimeouts(request, 10000, 10000, 10000, 30000);
    std::wstring headers = L"Authorization: Bearer " + access_token + L"\r\n";
    headers += L"Accept: application/json\r\n";
    headers += L"anthropic-beta: oauth-2025-04-20\r\n";
    if (!WinHttpAddRequestHeaders(request, headers.c_str(), static_cast<DWORD>(headers.size()), WINHTTP_ADDREQ_FLAG_ADD))
    {
        error = L"Failed to add Claude OAuth headers: " + WindowsErrorMessage(GetLastError());
        return false;
    }
    if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
        || !WinHttpReceiveResponse(request, nullptr))
    {
        error = L"Failed to query Claude OAuth usage: " + WindowsErrorMessage(GetLastError());
        return false;
    }
    if (!QueryStatusCode(request, response.status, error))
    {
        return false;
    }
    return ReadResponseBody(request, response.body, error);
}

std::optional<std::wstring> ResolveSessionKey(std::wstring& error)
{
    for (const auto* name : {L"CLAUDE_AI_SESSION_KEY", L"CLAUDE_WEB_SESSION_KEY"})
    {
        const auto value = GetEnvVar(name);
        if (!value.empty())
        {
            return claudequota::NormalizeSessionKeyInput(value, error);
        }
    }
    return claudequota::ReadStoredSessionKey(error);
}

bool HasAnyWindow(const claudequota::UsageSnapshot& usage)
{
    return usage.primary.present || usage.secondary.present || usage.monthly.present;
}
}

namespace claudequota
{
std::wstring GetClaudeSessionCredentialTarget()
{
    return kCredentialTarget;
}

std::optional<std::wstring> ReadStoredSessionKey(std::wstring& error)
{
    error.clear();
    PCREDENTIALW credential = nullptr;
    if (!CredReadW(kCredentialTarget, CRED_TYPE_GENERIC, 0, &credential))
    {
        const auto code = GetLastError();
        if (code == ERROR_NOT_FOUND)
        {
            return std::nullopt;
        }
        error = L"Failed to read the stored Claude sessionKey: " + WindowsErrorMessage(code);
        return std::nullopt;
    }

    std::wstring value;
    if (credential->CredentialBlobSize % sizeof(wchar_t) == 0)
    {
        value.assign(
            reinterpret_cast<const wchar_t*>(credential->CredentialBlob),
            credential->CredentialBlobSize / sizeof(wchar_t));
    }
    else
    {
        error = L"Stored Claude sessionKey has an invalid size.";
    }
    CredFree(credential);
    if (!error.empty())
    {
        return std::nullopt;
    }
    return NormalizeSessionKeyInput(value, error);
}

bool WriteStoredSessionKey(const std::wstring& input, std::wstring& error)
{
    const auto normalized = NormalizeSessionKeyInput(input, error);
    if (!normalized.has_value())
    {
        return false;
    }
    const auto blob_size = normalized->size() * sizeof(wchar_t);
    if (blob_size > CRED_MAX_CREDENTIAL_BLOB_SIZE)
    {
        error = L"Claude sessionKey is too large for Windows Credential Manager.";
        return false;
    }

    CREDENTIALW credential{};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = const_cast<LPWSTR>(kCredentialTarget);
    credential.CredentialBlobSize = static_cast<DWORD>(blob_size);
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<wchar_t*>(normalized->data()));
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    credential.UserName = const_cast<LPWSTR>(L"Claude Web");
    if (!CredWriteW(&credential, 0))
    {
        error = L"Failed to store Claude sessionKey: " + WindowsErrorMessage(GetLastError());
        return false;
    }
    error.clear();
    return true;
}

bool DeleteStoredSessionKey(std::wstring& error)
{
    error.clear();
    if (CredDeleteW(kCredentialTarget, CRED_TYPE_GENERIC, 0))
    {
        return true;
    }
    const auto code = GetLastError();
    if (code == ERROR_NOT_FOUND)
    {
        return true;
    }
    error = L"Failed to delete the stored Claude sessionKey: " + WindowsErrorMessage(code);
    return false;
}

bool HasStoredSessionKey()
{
    std::wstring error;
    return ReadStoredSessionKey(error).has_value();
}

std::wstring GetDefaultOAuthCredentialsPath()
{
    const auto user_profile = GetEnvVar(L"USERPROFILE");
    if (!user_profile.empty())
    {
        return JoinPath(JoinPath(user_profile, L".claude"), L".credentials.json");
    }
    return L".claude\\.credentials.json";
}

FetchResult FetchWebUsageSnapshot(const std::wstring& session_key)
{
    FetchResult result;
    std::wstring error;

    const auto user_agent = L"TrafficMonitorClaudeQuota/" + std::wstring(kTrafficMonitorQuotaPluginVersion);
    const auto proxy = GetEnvironmentProxy();
    HttpHandle session(WinHttpOpen(
        user_agent.c_str(),
        proxy.empty() ? WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY : WINHTTP_ACCESS_TYPE_NAMED_PROXY,
        proxy.empty() ? WINHTTP_NO_PROXY_NAME : proxy.c_str(),
        WINHTTP_NO_PROXY_BYPASS,
        0));
    if (session.value == nullptr)
    {
        result.error = L"Failed to open WinHTTP session: " + WindowsErrorMessage(GetLastError());
        return result;
    }

    HttpHandle connect(WinHttpConnect(session, kClaudeHost, INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (connect.value == nullptr)
    {
        result.error = L"Failed to connect to claude.ai: " + WindowsErrorMessage(GetLastError());
        return result;
    }

    HttpResponse account_response;
    if (!SendGet(connect, L"/api/account", session_key, account_response, result.error))
    {
        return result;
    }
    result.http_status = account_response.status;
    if (account_response.status == 401 || account_response.status == 403)
    {
        result.error = L"Claude authentication failed. Update the sessionKey in the plugin options.";
        return result;
    }
    if (account_response.status < 200 || account_response.status >= 300)
    {
        result.error = L"Claude account API returned HTTP " + std::to_wstring(account_response.status) + L".";
        return result;
    }

    const auto account = ParseAccountJson(account_response.body, result.error);
    if (!account.has_value())
    {
        return result;
    }

    UsageSnapshot usage;
    usage.plan_type = account->plan_type;
    const std::wstring base_path = L"/api/organizations/" + account->organization_id;

    HttpResponse usage_response;
    if (SendGet(connect, base_path + L"/usage", session_key, usage_response, error)
        && usage_response.status >= 200 && usage_response.status < 300)
    {
        if (const auto parsed = ParseUsageJson(usage_response.body, error))
        {
            usage.primary = parsed->primary;
            usage.secondary = parsed->secondary;
            ApplySpendLimitJson(usage_response.body, std::time(nullptr), usage, error);
        }
    }

    HttpResponse spend_response;
    if (SendGet(connect, base_path + L"/overage_spend_limit", session_key, spend_response, error)
        && spend_response.status >= 200 && spend_response.status < 300)
    {
        ApplySpendLimitJson(spend_response.body, std::time(nullptr), usage, error);
    }

    if (!HasAnyWindow(usage))
    {
        result.http_status = spend_response.status != 0 ? spend_response.status : usage_response.status;
        result.error = L"Claude did not return a supported 5-hour, 7-day, or monthly spend-limit window.";
        return result;
    }

    result.usage = usage;
    result.success = true;
    result.error.clear();
    result.http_status = 200;
    return result;
}

FetchResult FetchOAuthUsageSnapshot()
{
    FetchResult result;
    std::string credentials_json;
    if (!ReadFileBytes(GetDefaultOAuthCredentialsPath(), credentials_json, result.error))
    {
        result.error = L"Claude sessionKey is not configured, and Claude Code OAuth credentials were not found. " + result.error;
        return result;
    }

    const auto credentials = ParseOAuthCredentialsJson(credentials_json, result.error);
    if (!credentials.has_value())
    {
        return result;
    }

    const auto user_agent = L"TrafficMonitorClaudeQuota/" + std::wstring(kTrafficMonitorQuotaPluginVersion);
    const auto proxy = GetEnvironmentProxy();
    HttpHandle session(WinHttpOpen(
        user_agent.c_str(),
        proxy.empty() ? WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY : WINHTTP_ACCESS_TYPE_NAMED_PROXY,
        proxy.empty() ? WINHTTP_NO_PROXY_NAME : proxy.c_str(),
        WINHTTP_NO_PROXY_BYPASS,
        0));
    if (session.value == nullptr)
    {
        result.error = L"Failed to open WinHTTP session: " + WindowsErrorMessage(GetLastError());
        return result;
    }
    HttpHandle connect(WinHttpConnect(session, kOAuthHost, INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (connect.value == nullptr)
    {
        result.error = L"Failed to connect to api.anthropic.com: " + WindowsErrorMessage(GetLastError());
        return result;
    }

    HttpResponse response;
    if (!SendOAuthGet(connect, credentials->access_token, response, result.error))
    {
        return result;
    }
    result.http_status = response.status;
    if (response.status == 401 || response.status == 403)
    {
        result.error = L"Claude Code OAuth authentication failed. Run claude login or configure a web sessionKey.";
        return result;
    }
    if (response.status < 200 || response.status >= 300)
    {
        result.error = L"Claude OAuth usage API returned HTTP " + std::to_wstring(response.status) + L".";
        return result;
    }

    const auto usage = ParseUsageJson(response.body, result.error);
    if (!usage.has_value())
    {
        return result;
    }
    result.usage = *usage;
    result.usage.plan_type = credentials->rate_limit_tier.empty() ? L"Claude OAuth" : credentials->rate_limit_tier;
    ApplySpendLimitJson(response.body, std::time(nullptr), result.usage, result.error);
    if (!HasAnyWindow(result.usage))
    {
        result.error = L"Claude OAuth did not return a supported quota window.";
        return result;
    }
    result.success = true;
    result.error.clear();
    return result;
}

FetchResult FetchUsageSnapshot()
{
    std::wstring error;
    const auto session_key = ResolveSessionKey(error);
    if (session_key.has_value())
    {
        return FetchWebUsageSnapshot(*session_key);
    }
    if (!error.empty())
    {
        FetchResult result;
        result.error = error;
        return result;
    }
    return FetchOAuthUsageSnapshot();
}
}
