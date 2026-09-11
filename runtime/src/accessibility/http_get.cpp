#include "accessibility/http_get.h"

#include <windows.h>
#include <winhttp.h>

namespace a11y::net {
namespace {

// Every stage is bounded so a dead, captive or filtered network cannot keep the worker alive:
// the check is a courtesy at startup, never something the player waits for. Chosen to be a few
// seconds each, which is the whole budget the announcement is worth.
constexpr int kResolveTimeoutMs = 4000;
constexpr int kConnectTimeoutMs = 4000;
constexpr int kSendTimeoutMs = 4000;
constexpr int kReceiveTimeoutMs = 6000;

// GitHub's REST API answers 403 to a request without one.
constexpr const wchar_t* kUserAgent = L"MKWiiAccessibility";

// A version file is a few bytes and a release object a few kilobytes; the cap stops a wrong URL
// from pulling an arbitrary download into memory.
constexpr size_t kMaxBodyBytes = 256 * 1024;

constexpr DWORD kReadChunkBytes = 4096;

class Handle {
public:
    explicit Handle(HINTERNET handle) : mHandle(handle) {}
    ~Handle() {
        if (mHandle) {
            WinHttpCloseHandle(mHandle);
        }
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    operator HINTERNET() const { return mHandle; }
    explicit operator bool() const { return mHandle != nullptr; }

private:
    HINTERNET mHandle;
};

std::wstring Widen(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                           nullptr, 0);
    std::wstring wide(static_cast<size_t>(length > 0 ? length : 0), L'\0');
    if (length > 0) {
        MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), wide.data(),
                            length);
    }
    return wide;
}

}  // namespace

bool HttpGet(const std::string& url, std::string& body) {
    body.clear();
    const std::wstring wideUrl = Widen(url);
    if (wideUrl.empty()) {
        return false;
    }

    URL_COMPONENTS parts = {};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wideUrl.c_str(), static_cast<DWORD>(wideUrl.size()), 0, &parts)) {
        return false;
    }
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);

    const Handle session(WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        return false;
    }
    WinHttpSetTimeouts(session, kResolveTimeoutMs, kConnectTimeoutMs, kSendTimeoutMs,
                       kReceiveTimeoutMs);

    const Handle connection(WinHttpConnect(session, host.c_str(), parts.nPort, 0));
    if (!connection) {
        return false;
    }
    const DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    const Handle request(WinHttpOpenRequest(connection, L"GET", path.c_str(), nullptr,
                                            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                            flags));
    if (!request) {
        return false;
    }
    if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0,
                            0, 0) ||
        !WinHttpReceiveResponse(request, nullptr)) {
        return false;
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                             WINHTTP_NO_HEADER_INDEX) ||
        status != HTTP_STATUS_OK) {
        return false;
    }

    char chunk[kReadChunkBytes];
    for (;;) {
        DWORD read = 0;
        if (!WinHttpReadData(request, chunk, kReadChunkBytes, &read)) {
            body.clear();
            return false;
        }
        if (read == 0) {
            break;
        }
        if (body.size() + read > kMaxBodyBytes) {
            body.clear();
            return false;
        }
        body.append(chunk, read);
    }
    return !body.empty();
}

}  // namespace a11y::net
