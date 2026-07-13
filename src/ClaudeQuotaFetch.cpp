#include "ClaudeQuotaFetch.h"
#include "PluginVersion.h"

#include <Windows.h>
#include <winhttp.h>

#include <ctime>
#include <string>

namespace
{
constexpr const wchar_t* kOAuthHost = L"api.anthropic.com";

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

bool HasAnyWindow(const claudequota::UsageSnapshot& usage)
{
    return usage.primary.present || usage.secondary.present || usage.monthly.present;
}
}

namespace claudequota
{
std::wstring GetDefaultOAuthCredentialsPath()
{
    const auto user_profile = GetEnvVar(L"USERPROFILE");
    if (!user_profile.empty())
    {
        return JoinPath(JoinPath(user_profile, L".claude"), L".credentials.json");
    }
    return L".claude\\.credentials.json";
}

std::optional<OAuthCredentials> ReadClaudeCodeOAuthCredentials(std::wstring& error)
{
    std::string credentials_json;
    if (!ReadFileBytes(GetDefaultOAuthCredentialsPath(), credentials_json, error))
    {
        return std::nullopt;
    }
    return ParseOAuthCredentialsJson(credentials_json, error);
}

FetchResult FetchOAuthUsageSnapshot()
{
    FetchResult result;
    const auto credentials = ReadClaudeCodeOAuthCredentials(result.error);
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
        result.error = L"Claude Code OAuth authentication failed. Run claude auth login.";
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
    return FetchOAuthUsageSnapshot();
}
}
