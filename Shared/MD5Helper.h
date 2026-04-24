#pragma once

// MD5Helper.h — Computes MD5 hash of a file using Windows CryptAPI.
// No third-party dependencies; only advapi32.lib (linked via pragma).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincrypt.h>
#include <string>
#include <vector>

#pragma comment(lib, "advapi32.lib")

namespace AutoUpdate
{
    // Returns lowercase hex MD5 string, or empty string on failure.
    inline std::string ComputeFileMD5(const std::wstring& filePath)
    {
        HANDLE hFile = CreateFileW(
            filePath.c_str(), GENERIC_READ, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

        if (hFile == INVALID_HANDLE_VALUE)
            return {};

        HCRYPTPROV hProv = 0;
        if (!CryptAcquireContextW(&hProv, nullptr, nullptr,
            PROV_RSA_FULL, CRYPT_VERIFYCONTEXT))
        {
            CloseHandle(hFile);
            return {};
        }

        HCRYPTHASH hHash = 0;
        if (!CryptCreateHash(hProv, CALG_MD5, 0, 0, &hHash))
        {
            CryptReleaseContext(hProv, 0);
            CloseHandle(hFile);
            return {};
        }

        // 64 KB read buffer — good balance between memory and I/O overhead
        constexpr DWORD kBufSize = 64 * 1024;
        std::vector<BYTE> buf(kBufSize);
        DWORD bytesRead = 0;
        bool ok = true;

        while (ReadFile(hFile, buf.data(), kBufSize, &bytesRead, nullptr) && bytesRead > 0)
        {
            if (!CryptHashData(hHash, buf.data(), bytesRead, 0))
            {
                ok = false;
                break;
            }
        }

        std::string result;
        if (ok)
        {
            BYTE  hashVal[16] = {};
            DWORD hashLen     = sizeof(hashVal);
            if (CryptGetHashParam(hHash, HP_HASHVAL, hashVal, &hashLen, 0))
            {
                char hex[33] = {};
                for (int i = 0; i < 16; ++i)
                    sprintf_s(hex + i * 2, 3, "%02x", hashVal[i]);
                result = hex;
            }
        }

        CryptDestroyHash(hHash);
        CryptReleaseContext(hProv, 0);
        CloseHandle(hFile);
        return result;
    }

} // namespace AutoUpdate
