#include "ClaudeQuotaFetch.h"
#include "PluginVersion.h"

#include <Windows.h>
#include <winhttp.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <ctime>
#include <string>

namespace
{
constexpr const wchar_t* kUsageHost = L"api.anthropic.com";
constexpr const wchar_t* kTokenHost = L"platform.claude.com";
constexpr const wchar_t* kDefaultClientId = L"9d1c250a-e61b-44d9-88ed-5944d1962f5e";
constexpr const wchar_t* kDefaultScopes =
    L"user:profile user:inference user:sessions:claude_code user:mcp_servers user:file_upload";
constexpr long long kRefreshSkewMs = 5LL * 60 * 1000;

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

struct RealHttpContext
{
    HttpHandle session;
};

struct CredentialStoreContext
{
    std::wstring path;
    std::string expected_json;
};

struct JsonSpan
{
    size_t begin{};
    size_t end{};
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

std::string WideToUtf8(const std::wstring& value)
{
    if (value.empty())
    {
        return {};
    }
    const int length = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (length <= 0)
    {
        return {};
    }
    std::string result(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        result.data(),
        length,
        nullptr,
        nullptr);
    return result;
}

std::string JsonString(const std::wstring& value)
{
    const auto utf8 = WideToUtf8(value);
    std::string result = "\"";
    for (const auto ch : utf8)
    {
        switch (ch)
        {
        case '\"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\b': result += "\\b"; break;
        case '\f': result += "\\f"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20)
            {
                constexpr char digits[] = "0123456789abcdef";
                result += "\\u00";
                result.push_back(digits[(static_cast<unsigned char>(ch) >> 4) & 0x0f]);
                result.push_back(digits[static_cast<unsigned char>(ch) & 0x0f]);
            }
            else
            {
                result.push_back(ch);
            }
        }
    }
    result.push_back('\"');
    return result;
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
    const auto read_error = read ? ERROR_SUCCESS : GetLastError();
    CloseHandle(file);
    if (!read)
    {
        error = L"Failed to read " + path + L": " + WindowsErrorMessage(read_error);
        return false;
    }
    content.resize(bytes_read);
    return true;
}

void SkipWhitespace(const std::string& json, size_t& position)
{
    while (position < json.size()
        && (json[position] == ' ' || json[position] == '\t' || json[position] == '\r' || json[position] == '\n'))
    {
        ++position;
    }
}

bool ParseJsonStringToken(const std::string& json, size_t& position, std::string* decoded)
{
    if (position >= json.size() || json[position] != '\"')
    {
        return false;
    }
    ++position;
    if (decoded != nullptr)
    {
        decoded->clear();
    }
    bool escaped = false;
    while (position < json.size())
    {
        const char ch = json[position++];
        if (escaped)
        {
            if (decoded != nullptr)
            {
                decoded->push_back(ch);
            }
            escaped = false;
        }
        else if (ch == '\\')
        {
            escaped = true;
        }
        else if (ch == '\"')
        {
            return true;
        }
        else if (decoded != nullptr)
        {
            decoded->push_back(ch);
        }
    }
    return false;
}

bool SkipJsonValue(const std::string& json, size_t& position)
{
    SkipWhitespace(json, position);
    if (position >= json.size())
    {
        return false;
    }
    if (json[position] == '\"')
    {
        return ParseJsonStringToken(json, position, nullptr);
    }
    if (json[position] == '{')
    {
        ++position;
        SkipWhitespace(json, position);
        if (position < json.size() && json[position] == '}')
        {
            ++position;
            return true;
        }
        while (position < json.size())
        {
            if (!ParseJsonStringToken(json, position, nullptr))
            {
                return false;
            }
            SkipWhitespace(json, position);
            if (position >= json.size() || json[position++] != ':')
            {
                return false;
            }
            if (!SkipJsonValue(json, position))
            {
                return false;
            }
            SkipWhitespace(json, position);
            if (position < json.size() && json[position] == '}')
            {
                ++position;
                return true;
            }
            if (position >= json.size() || json[position++] != ',')
            {
                return false;
            }
            SkipWhitespace(json, position);
        }
        return false;
    }
    if (json[position] == '[')
    {
        ++position;
        SkipWhitespace(json, position);
        if (position < json.size() && json[position] == ']')
        {
            ++position;
            return true;
        }
        while (position < json.size())
        {
            if (!SkipJsonValue(json, position))
            {
                return false;
            }
            SkipWhitespace(json, position);
            if (position < json.size() && json[position] == ']')
            {
                ++position;
                return true;
            }
            if (position >= json.size() || json[position++] != ',')
            {
                return false;
            }
        }
        return false;
    }

    const auto start = position;
    while (position < json.size()
        && json[position] != ',' && json[position] != '}' && json[position] != ']'
        && json[position] != ' ' && json[position] != '\t' && json[position] != '\r' && json[position] != '\n')
    {
        ++position;
    }
    return position > start;
}

bool FindObjectMemberValue(const std::string& json, const JsonSpan& object, const std::string& name, JsonSpan& value)
{
    if (object.end <= object.begin + 1 || object.end > json.size() || json[object.begin] != '{')
    {
        return false;
    }
    size_t position = object.begin + 1;
    SkipWhitespace(json, position);
    while (position < object.end - 1)
    {
        std::string key;
        if (!ParseJsonStringToken(json, position, &key))
        {
            return false;
        }
        SkipWhitespace(json, position);
        if (position >= object.end || json[position++] != ':')
        {
            return false;
        }
        SkipWhitespace(json, position);
        const auto value_start = position;
        if (!SkipJsonValue(json, position))
        {
            return false;
        }
        if (key == name)
        {
            value = {value_start, position};
            return true;
        }
        SkipWhitespace(json, position);
        if (position < object.end && json[position] == ',')
        {
            ++position;
            SkipWhitespace(json, position);
        }
        else
        {
            break;
        }
    }
    return false;
}

bool GetRootObject(const std::string& json, JsonSpan& root)
{
    size_t start = json.size() >= 3
        && static_cast<unsigned char>(json[0]) == 0xef
        && static_cast<unsigned char>(json[1]) == 0xbb
        && static_cast<unsigned char>(json[2]) == 0xbf
        ? 3
        : 0;
    SkipWhitespace(json, start);
    if (start >= json.size() || json[start] != '{')
    {
        return false;
    }
    auto end = start;
    if (!SkipJsonValue(json, end))
    {
        return false;
    }
    root = {start, end};
    return true;
}

bool SetClaudeOAuthMember(std::string& json, const std::string& name, const std::string& encoded_value)
{
    JsonSpan root;
    JsonSpan oauth;
    if (!GetRootObject(json, root) || !FindObjectMemberValue(json, root, "claudeAiOauth", oauth)
        || oauth.begin >= json.size() || json[oauth.begin] != '{')
    {
        return false;
    }

    JsonSpan member;
    if (FindObjectMemberValue(json, oauth, name, member))
    {
        json.replace(member.begin, member.end - member.begin, encoded_value);
        return true;
    }

    size_t first = oauth.begin + 1;
    SkipWhitespace(json, first);
    const bool empty = first < oauth.end && json[first] == '}';
    const auto insertion = (empty ? "" : ",") + JsonString(std::wstring(name.begin(), name.end())) + ":" + encoded_value;
    json.insert(oauth.end - 1, insertion);
    return true;
}

bool BuildUpdatedCredentialsJson(
    const std::string& original,
    const claudequota::OAuthCredentials& credentials,
    std::string& updated,
    std::wstring& error)
{
    updated = original;
    if (!SetClaudeOAuthMember(updated, "accessToken", JsonString(credentials.access_token))
        || !SetClaudeOAuthMember(updated, "refreshToken", JsonString(credentials.refresh_token))
        || !SetClaudeOAuthMember(updated, "expiresAt", std::to_string(credentials.expires_at_ms)))
    {
        error = L"Failed to update Claude OAuth credentials JSON.";
        return false;
    }
    if (credentials.refresh_token_expires_at_ms > 0
        && !SetClaudeOAuthMember(
            updated,
            "refreshTokenExpiresAt",
            std::to_string(credentials.refresh_token_expires_at_ms)))
    {
        error = L"Failed to update Claude OAuth refresh-token expiry.";
        return false;
    }
    return true;
}

bool WriteCredentialsAtomically(const std::string& updated_json, std::wstring& error, void* raw_context)
{
    auto* context = static_cast<CredentialStoreContext*>(raw_context);
    if (context == nullptr)
    {
        error = L"Claude OAuth credential store was not configured.";
        return false;
    }

    std::string current_json;
    if (!ReadFileBytes(context->path, current_json, error))
    {
        return false;
    }
    if (current_json != context->expected_json)
    {
        error = L"Claude credentials changed while refreshing; retrying is safe.";
        return false;
    }

    std::wstring temporary_path;
    HANDLE file = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 10 && file == INVALID_HANDLE_VALUE; ++attempt)
    {
        temporary_path = context->path + L".tmp." + std::to_wstring(GetCurrentProcessId()) + L"."
            + std::to_wstring(GetTickCount64()) + L"." + std::to_wstring(attempt);
        file = CreateFileW(
            temporary_path.c_str(),
            GENERIC_WRITE,
            0,
            nullptr,
            CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
    }
    if (file == INVALID_HANDLE_VALUE)
    {
        error = L"Failed to create a temporary Claude credentials file: " + WindowsErrorMessage(GetLastError());
        return false;
    }

    DWORD bytes_written = 0;
    const BOOL written = updated_json.empty()
        || WriteFile(file, updated_json.data(), static_cast<DWORD>(updated_json.size()), &bytes_written, nullptr);
    const BOOL flushed = written && bytes_written == updated_json.size() && FlushFileBuffers(file);
    const auto write_error = flushed ? ERROR_SUCCESS : GetLastError();
    CloseHandle(file);
    if (!flushed)
    {
        DeleteFileW(temporary_path.c_str());
        error = L"Failed to write Claude OAuth credentials: " + WindowsErrorMessage(write_error);
        return false;
    }

    if (!ReplaceFileW(
            context->path.c_str(),
            temporary_path.c_str(),
            nullptr,
            REPLACEFILE_WRITE_THROUGH,
            nullptr,
            nullptr))
    {
        const auto replace_error = GetLastError();
        DeleteFileW(temporary_path.c_str());
        error = L"Failed to replace Claude OAuth credentials: " + WindowsErrorMessage(replace_error);
        return false;
    }
    context->expected_json = updated_json;
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

int QueryRetryAfterSeconds(HINTERNET request)
{
    DWORD size = 0;
    if (WinHttpQueryHeaders(
            request,
            WINHTTP_QUERY_CUSTOM,
            L"Retry-After",
            WINHTTP_NO_OUTPUT_BUFFER,
            &size,
            WINHTTP_NO_HEADER_INDEX)
        || GetLastError() != ERROR_INSUFFICIENT_BUFFER)
    {
        return 0;
    }

    std::wstring value(size / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(
            request,
            WINHTTP_QUERY_CUSTOM,
            L"Retry-After",
            value.data(),
            &size,
            WINHTTP_NO_HEADER_INDEX))
    {
        return 0;
    }
    value.resize(size / sizeof(wchar_t));
    while (!value.empty() && value.back() == L'\0')
    {
        value.pop_back();
    }

    wchar_t* end = nullptr;
    const auto seconds = std::wcstoll(value.c_str(), &end, 10);
    if (end != value.c_str() && *end == L'\0' && seconds > 0)
    {
        return static_cast<int>(std::min<long long>(seconds, INT_MAX));
    }

    SYSTEMTIME retry_time{};
    SYSTEMTIME current_time{};
    FILETIME retry_file_time{};
    FILETIME current_file_time{};
    if (!WinHttpTimeToSystemTime(value.c_str(), &retry_time))
    {
        return 0;
    }
    GetSystemTime(&current_time);
    if (!SystemTimeToFileTime(&retry_time, &retry_file_time)
        || !SystemTimeToFileTime(&current_time, &current_file_time))
    {
        return 0;
    }
    ULARGE_INTEGER retry{};
    ULARGE_INTEGER current{};
    retry.LowPart = retry_file_time.dwLowDateTime;
    retry.HighPart = retry_file_time.dwHighDateTime;
    current.LowPart = current_file_time.dwLowDateTime;
    current.HighPart = current_file_time.dwHighDateTime;
    if (retry.QuadPart <= current.QuadPart)
    {
        return 0;
    }
    const auto delta = (retry.QuadPart - current.QuadPart + 9999999ULL) / 10000000ULL;
    return static_cast<int>(std::min<unsigned long long>(delta, INT_MAX));
}

bool SendRealRequest(
    const claudequota::ClaudeHttpRequest& request_data,
    claudequota::ClaudeHttpResponse& response,
    std::wstring& error,
    void* raw_context)
{
    auto* context = static_cast<RealHttpContext*>(raw_context);
    if (context == nullptr || context->session.value == nullptr)
    {
        error = L"Claude HTTP session was not initialized.";
        return false;
    }

    HttpHandle connect(WinHttpConnect(context->session, request_data.host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (connect.value == nullptr)
    {
        error = L"Failed to connect to " + request_data.host + L": " + WindowsErrorMessage(GetLastError());
        return false;
    }
    HttpHandle request(WinHttpOpenRequest(
        connect,
        request_data.method.c_str(),
        request_data.path.c_str(),
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

    std::wstring headers;
    if (!request_data.authorization.empty())
    {
        headers += L"Authorization: " + request_data.authorization + L"\r\n";
    }
    if (!request_data.accept.empty())
    {
        headers += L"Accept: " + request_data.accept + L"\r\n";
    }
    if (!request_data.anthropic_beta.empty())
    {
        headers += L"anthropic-beta: " + request_data.anthropic_beta + L"\r\n";
    }
    if (!request_data.content_type.empty())
    {
        headers += L"Content-Type: " + request_data.content_type + L"\r\n";
    }
    if (!headers.empty()
        && !WinHttpAddRequestHeaders(request, headers.c_str(), static_cast<DWORD>(headers.size()), WINHTTP_ADDREQ_FLAG_ADD))
    {
        error = L"Failed to add Claude OAuth headers: " + WindowsErrorMessage(GetLastError());
        return false;
    }

    auto* request_body = request_data.body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(request_data.body.data());
    const auto request_body_size = static_cast<DWORD>(request_data.body.size());
    if (!WinHttpSendRequest(
            request,
            WINHTTP_NO_ADDITIONAL_HEADERS,
            0,
            request_body,
            request_body_size,
            request_body_size,
            0)
        || !WinHttpReceiveResponse(request, nullptr))
    {
        error = L"Failed to query Claude OAuth service: " + WindowsErrorMessage(GetLastError());
        return false;
    }
    if (!QueryStatusCode(request, response.http_status, error))
    {
        return false;
    }
    response.retry_after_seconds = QueryRetryAfterSeconds(request);
    return ReadResponseBody(request, response.body, error);
}

bool HasAnyWindow(const claudequota::UsageSnapshot& usage)
{
    return usage.primary.present || usage.secondary.present || usage.monthly.present;
}

std::wstring JoinScopes(const std::vector<std::wstring>& scopes)
{
    std::wstring result;
    for (const auto& scope : scopes)
    {
        if (!scope.empty())
        {
            if (!result.empty())
            {
                result.push_back(L' ');
            }
            result += scope;
        }
    }
    return result.empty() ? kDefaultScopes : result;
}

claudequota::ClaudeHttpRequest BuildUsageRequest(const claudequota::OAuthCredentials& credentials)
{
    claudequota::ClaudeHttpRequest request;
    request.host = kUsageHost;
    request.method = L"GET";
    request.path = L"/api/oauth/usage";
    request.authorization = L"Bearer " + credentials.access_token;
    request.accept = L"application/json";
    request.anthropic_beta = L"oauth-2025-04-20";
    return request;
}

claudequota::ClaudeHttpRequest BuildRefreshRequest(const claudequota::OAuthCredentials& credentials)
{
    const auto client_id = credentials.client_id.empty() ? kDefaultClientId : credentials.client_id;
    claudequota::ClaudeHttpRequest request;
    request.host = kTokenHost;
    request.method = L"POST";
    request.path = L"/v1/oauth/token";
    request.accept = L"application/json";
    request.content_type = L"application/json";
    request.body = "{\"grant_type\":\"refresh_token\",\"refresh_token\":" + JsonString(credentials.refresh_token)
        + ",\"client_id\":" + JsonString(client_id) + ",\"scope\":" + JsonString(JoinScopes(credentials.scopes)) + "}";
    return request;
}

bool RefreshCredentials(
    const std::string& original_json,
    long long now_ms,
    claudequota::OAuthCredentials& credentials,
    claudequota::FetchResult& result,
    claudequota::ClaudeHttpRequestCallback request_callback,
    void* request_context,
    claudequota::OAuthCredentialsStoreCallback store_callback,
    void* store_context)
{
    if (credentials.refresh_token.empty())
    {
        result.error = L"Claude OAuth token expired and no refresh token was found. Run claude auth login.";
        return false;
    }

    claudequota::ClaudeHttpResponse response;
    if (!request_callback(BuildRefreshRequest(credentials), response, result.error, request_context))
    {
        return false;
    }
    result.http_status = response.http_status;
    result.retry_after_seconds = response.retry_after_seconds;
    if (response.http_status < 200 || response.http_status >= 300)
    {
        if (response.http_status == 400 || response.http_status == 401 || response.http_status == 403)
        {
            result.error = L"Claude Code OAuth refresh failed. Run claude auth login.";
        }
        else
        {
            result.error = L"Claude OAuth token endpoint returned HTTP " + std::to_wstring(response.http_status) + L".";
        }
        return false;
    }

    const auto token = claudequota::ParseOAuthTokenResponseJson(response.body, result.error);
    if (!token.has_value())
    {
        return false;
    }
    if (token->expires_in_seconds > (LLONG_MAX - now_ms) / 1000)
    {
        result.error = L"Claude OAuth token expiry was invalid.";
        return false;
    }

    auto updated_credentials = credentials;
    updated_credentials.access_token = token->access_token;
    if (!token->refresh_token.empty())
    {
        updated_credentials.refresh_token = token->refresh_token;
    }
    updated_credentials.expires_at_ms = now_ms + token->expires_in_seconds * 1000;
    if (token->refresh_token_expires_in_seconds > 0)
    {
        if (token->refresh_token_expires_in_seconds > (LLONG_MAX - now_ms) / 1000)
        {
            result.error = L"Claude OAuth refresh-token expiry was invalid.";
            return false;
        }
        updated_credentials.refresh_token_expires_at_ms = now_ms + token->refresh_token_expires_in_seconds * 1000;
    }

    std::string updated_json;
    if (!BuildUpdatedCredentialsJson(original_json, updated_credentials, updated_json, result.error))
    {
        return false;
    }
    if (store_callback == nullptr || !store_callback(updated_json, result.error, store_context))
    {
        if (result.error.empty())
        {
            result.error = L"Failed to persist refreshed Claude OAuth credentials.";
        }
        return false;
    }
    credentials = std::move(updated_credentials);
    return true;
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

FetchResult FetchUsageSnapshotFromCredentialsJson(
    const std::string& credentials_json,
    long long now_ms,
    ClaudeHttpRequestCallback request_callback,
    void* request_context,
    OAuthCredentialsStoreCallback store_callback,
    void* store_context)
{
    FetchResult result;
    if (request_callback == nullptr)
    {
        result.error = L"Claude HTTP transport was not configured.";
        return result;
    }

    auto credentials = ParseOAuthCredentialsJson(credentials_json, result.error);
    if (!credentials.has_value())
    {
        return result;
    }

    bool refreshed = false;
    if (credentials->expires_at_ms > 0 && credentials->expires_at_ms <= now_ms + kRefreshSkewMs)
    {
        if (!RefreshCredentials(
                credentials_json,
                now_ms,
                *credentials,
                result,
                request_callback,
                request_context,
                store_callback,
                store_context))
        {
            return result;
        }
        refreshed = true;
    }

    ClaudeHttpResponse response;
    if (!request_callback(BuildUsageRequest(*credentials), response, result.error, request_context))
    {
        return result;
    }
    if (response.http_status == 401 && !refreshed)
    {
        if (!RefreshCredentials(
                credentials_json,
                now_ms,
                *credentials,
                result,
                request_callback,
                request_context,
                store_callback,
                store_context))
        {
            return result;
        }
        refreshed = true;
        response = {};
        if (!request_callback(BuildUsageRequest(*credentials), response, result.error, request_context))
        {
            return result;
        }
    }

    result.http_status = response.http_status;
    result.retry_after_seconds = response.retry_after_seconds;
    if (response.http_status == 401 || response.http_status == 403)
    {
        result.error = L"Claude Code OAuth authentication failed. Run claude auth login.";
        return result;
    }
    if (response.http_status == 429)
    {
        result.error = L"Claude OAuth usage API is rate limited";
        if (response.retry_after_seconds > 0)
        {
            result.error += L"; retry after " + std::to_wstring(response.retry_after_seconds) + L" seconds";
        }
        result.error += L".";
        return result;
    }
    if (response.http_status < 200 || response.http_status >= 300)
    {
        result.error = L"Claude OAuth usage API returned HTTP " + std::to_wstring(response.http_status) + L".";
        return result;
    }

    const auto usage = ParseUsageJson(response.body, result.error);
    if (!usage.has_value())
    {
        return result;
    }
    result.usage = *usage;
    result.usage.plan_type = credentials->rate_limit_tier.empty() ? L"Claude OAuth" : credentials->rate_limit_tier;
    ApplySpendLimitJson(response.body, now_ms / 1000, result.usage, result.error);
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
    FetchResult result;
    const auto credentials_path = GetDefaultOAuthCredentialsPath();
    std::string credentials_json;
    if (!ReadFileBytes(credentials_path, credentials_json, result.error))
    {
        return result;
    }

    const auto user_agent = L"TrafficMonitorClaudeQuota/" + std::wstring(kTrafficMonitorQuotaPluginVersion);
    const auto proxy = GetEnvironmentProxy();
    RealHttpContext http_context;
    http_context.session.value = WinHttpOpen(
        user_agent.c_str(),
        proxy.empty() ? WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY : WINHTTP_ACCESS_TYPE_NAMED_PROXY,
        proxy.empty() ? WINHTTP_NO_PROXY_NAME : proxy.c_str(),
        WINHTTP_NO_PROXY_BYPASS,
        0);
    if (http_context.session.value == nullptr)
    {
        result.error = L"Failed to open WinHTTP session: " + WindowsErrorMessage(GetLastError());
        return result;
    }

    CredentialStoreContext store_context{credentials_path, credentials_json};
    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return FetchUsageSnapshotFromCredentialsJson(
        credentials_json,
        now_ms,
        SendRealRequest,
        &http_context,
        WriteCredentialsAtomically,
        &store_context);
}
}
