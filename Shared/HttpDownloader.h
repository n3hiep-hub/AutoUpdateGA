#pragma once

// HttpDownloader.h — High-speed HTTP/HTTPS download via WinHTTP.
// Tuned for large game files: 1 MB read chunks, large socket buffers,
// generous timeouts. No third-party dependencies.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <string>
#include <vector>
#include <functional>

#pragma comment(lib, "winhttp.lib")

namespace AutoUpdate
{
    struct MemoryDownload
    {
        bool        success   = false;
        std::string content;
        DWORD       errorCode = 0;
    };

    // ------------------------------------------------------------------ //
    //  Internal helpers
    // ------------------------------------------------------------------ //
    namespace detail
    {
        struct Handles
        {
            HINTERNET hSession = nullptr;
            HINTERNET hConnect = nullptr;
            HINTERNET hRequest = nullptr;

            ~Handles()
            {
                if (hRequest) WinHttpCloseHandle(hRequest);
                if (hConnect) WinHttpCloseHandle(hConnect);
                if (hSession) WinHttpCloseHandle(hSession);
            }
        };

        // Apply performance and timeout options to a session handle.
        inline void ApplySessionOptions(HINTERNET hSession)
        {
            // Follow redirects automatically
            DWORD redir = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
            WinHttpSetOption(hSession, WINHTTP_OPTION_REDIRECT_POLICY,
                             &redir, sizeof(redir));

            // Connection timeout: 15 s
            DWORD connectTimeout = 15000;
            WinHttpSetOption(hSession, WINHTTP_OPTION_CONNECT_TIMEOUT,
                             &connectTimeout, sizeof(connectTimeout));

            // Receive timeout: 5 min (large files on slow links)
            DWORD recvTimeout = 300000;
            WinHttpSetOption(hSession, WINHTTP_OPTION_RECEIVE_TIMEOUT,
                             &recvTimeout, sizeof(recvTimeout));
            WinHttpSetOption(hSession, WINHTTP_OPTION_SEND_TIMEOUT,
                             &recvTimeout, sizeof(recvTimeout));

            // Receive-response header timeout: 30 s
            DWORD responseTimeout = 30000;
            WinHttpSetOption(hSession, WINHTTP_OPTION_RECEIVE_RESPONSE_TIMEOUT,
                             &responseTimeout, sizeof(responseTimeout));
        }

        // Crack URL and open Session + Connect + Request.
        // Returns false on any failure (RAII handles clean up automatically).
        inline bool OpenHandles(const std::wstring& url, Handles& h)
        {
            URL_COMPONENTS uc   = {};
            uc.dwStructSize     = sizeof(uc);
            wchar_t scheme[32]  = {};
            wchar_t host[512]   = {};
            wchar_t path[4096]  = {};
            uc.lpszScheme   = scheme; uc.dwSchemeLength   = _countof(scheme);
            uc.lpszHostName = host;   uc.dwHostNameLength = _countof(host);
            uc.lpszUrlPath  = path;   uc.dwUrlPathLength  = _countof(path);

            if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc))
                return false;

            h.hSession = WinHttpOpen(
                L"WinHTTP/1.0",
                WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                WINHTTP_NO_PROXY_NAME,
                WINHTTP_NO_PROXY_BYPASS, 0);
            if (!h.hSession) return false;

            ApplySessionOptions(h.hSession);

            h.hConnect = WinHttpConnect(h.hSession, host, uc.nPort, 0);
            if (!h.hConnect) return false;

            DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
            h.hRequest  = WinHttpOpenRequest(
                h.hConnect, L"GET", path,
                nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
            if (!h.hRequest) return false;

            if (!WinHttpSendRequest(h.hRequest,
                WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
                return false;

            if (!WinHttpReceiveResponse(h.hRequest, nullptr))
                return false;

            DWORD status = 0, sz = sizeof(status);
            WinHttpQueryHeaders(h.hRequest,
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                nullptr, &status, &sz, nullptr);
            return (status == 200);
        }

        inline DWORD64 GetContentLength(HINTERNET hRequest)
        {
            wchar_t buf[32] = {};
            DWORD   sz      = sizeof(buf);
            if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_CONTENT_LENGTH,
                nullptr, buf, &sz, nullptr))
                return _wcstoui64(buf, nullptr, 10);
            return 0;
        }

        // Create all missing directories in the path to filePath.
        inline void EnsureDirectories(const std::wstring& filePath)
        {
            auto pos = filePath.rfind(L'\\');
            if (pos == std::wstring::npos) return;
            std::wstring dir = filePath.substr(0, pos);
            for (size_t i = 0; i <= dir.size(); ++i)
            {
                if (i == dir.size() || dir[i] == L'\\')
                {
                    if (i > 0)
                        CreateDirectoryW(dir.substr(0, i).c_str(), nullptr);
                }
            }
        }
    } // namespace detail

    // ------------------------------------------------------------------ //
    //  Public API
    // ------------------------------------------------------------------ //

    // Download URL to memory — suitable for small manifest files.
    inline MemoryDownload DownloadToMemory(const std::wstring& url)
    {
        MemoryDownload result;
        detail::Handles h;

        if (!detail::OpenHandles(url, h))
        {
            result.errorCode = GetLastError();
            return result;
        }

        // Small buffer is fine here (manifest is a few KB at most)
        char    buf[16384];
        DWORD   read = 0;
        while (WinHttpReadData(h.hRequest, buf, sizeof(buf), &read) && read > 0)
            result.content.append(buf, read);

        result.success = true;
        return result;
    }

    // Download URL to a local file — optimised for large files.
    //
    // progressCb(bytesReceived, totalBytes):
    //   Called after every chunk. totalBytes may be 0 if server omits Content-Length.
    //   Safe to call PostMessage / atomics here — it runs on the caller's thread.
    //
    // Writes to destPath.tmp first, then renames — safe on interruption.
    inline bool DownloadToFile(
        const std::wstring& url,
        const std::wstring& destPath,
        std::function<void(DWORD64 recv, DWORD64 total)> progressCb = nullptr)
    {
        detail::Handles h;
        if (!detail::OpenHandles(url, h))
            return false;

        DWORD64 contentLength = detail::GetContentLength(h.hRequest);
        detail::EnsureDirectories(destPath);

        std::wstring tmpPath = destPath + L".tmp";
        HANDLE hFile = CreateFileW(
            tmpPath.c_str(), GENERIC_WRITE, 0,
            nullptr, CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, // hint for OS read-ahead
            nullptr);
        if (hFile == INVALID_HANDLE_VALUE)
            return false;

        // 1 MB heap buffer — avoids thousands of tiny WinHTTP calls and
        // maximises disk write throughput (aligns well with NTFS cluster sizes).
        constexpr DWORD kChunkSize = 1024u * 1024u; // 1 MB
        std::vector<char> buf(kChunkSize);

        DWORD   read      = 0;
        DWORD64 totalRead = 0;
        bool    writeOk   = true;

        while (WinHttpReadData(h.hRequest, buf.data(), kChunkSize, &read) && read > 0)
        {
            DWORD written = 0;
            if (!WriteFile(hFile, buf.data(), read, &written, nullptr) || written != read)
            {
                writeOk = false;
                break;
            }
            totalRead += read;
            if (progressCb)
                progressCb(totalRead, contentLength);
        }

        CloseHandle(hFile);

        if (!writeOk)
        {
            DeleteFileW(tmpPath.c_str());
            return false;
        }

        // Atomic replace: delete old file, rename temp
        DeleteFileW(destPath.c_str());
        if (!MoveFileW(tmpPath.c_str(), destPath.c_str()))
        {
            DeleteFileW(tmpPath.c_str());
            return false;
        }
        return true;
    }

} // namespace AutoUpdate
