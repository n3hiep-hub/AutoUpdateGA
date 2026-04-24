// MD5Generator â Console tool for server-side use.
// Scans a directory tree, computes MD5 for every game file,
// and writes AutoUpdate.ini ready to be placed on the HTTP server.
//
// Usage:
//   MD5Generator.exe [folder] [baseURL]
//
// Examples:
//   MD5Generator.exe                              (scans current folder)
//   MD5Generator.exe "C:\GameBuild"              (scans given folder)
//   MD5Generator.exe "C:\GameBuild" "http://update.mygame.com/v2"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <filesystem>

#include "MD5Helper.h"

namespace fs = std::filesystem;

// Extensions and filenames to exclude from the manifest
static const std::vector<std::wstring> kSkipExtensions = {
    L".ini", L".tmp", L".log", L".bak", L".db", L".pdb"
};

static const std::vector<std::wstring> kSkipFilenames = {
    L"AutoUpdate.ini",
    L"MD5Generator.exe",
    L"desktop.ini",
    L"thumbs.db"
};

static bool ShouldSkip(const fs::path& p)
{
    std::wstring ext  = p.extension().wstring();
    std::wstring name = p.filename().wstring();

    for (const auto& e : kSkipExtensions)
        if (_wcsicmp(ext.c_str(), e.c_str()) == 0) return true;

    for (const auto& n : kSkipFilenames)
        if (_wcsicmp(name.c_str(), n.c_str()) == 0) return true;

    return false;
}

int main(int argc, char* argv[])
{
    // --- Resolve target directory ---
    std::wstring targetDir;
    if (argc > 1)
    {
        std::string a = argv[1];
        targetDir = std::wstring(a.begin(), a.end());
    }
    else
    {
        wchar_t buf[MAX_PATH] = {};
        GetCurrentDirectoryW(MAX_PATH, buf);
        targetDir = buf;
    }

    // Normalise trailing backslash
    if (!targetDir.empty() && targetDir.back() == L'\\')
        targetDir.pop_back();

    // --- Resolve BaseURL ---
    std::string baseURL = "http://localhost/Update";
    if (argc > 2)
        baseURL = argv[2];

    // --- Validate directory ---
    std::error_code ec;
    if (!fs::exists(targetDir, ec) || !fs::is_directory(targetDir, ec))
    {
        std::cerr << "[ERROR] Directory not found: "
                  << std::string(targetDir.begin(), targetDir.end()) << "\n";
        return 1;
    }

    std::string outPath =
        std::string(targetDir.begin(), targetDir.end()) + "\\AutoUpdate.ini";

    std::ofstream out(outPath, std::ios::binary);
    if (!out)
    {
        std::cerr << "[ERROR] Cannot write: " << outPath << "\n";
        return 1;
    }

    // Write header
    out << "[AutoUpdate]\r\n";
    out << "BaseURL=" << baseURL << "\r\n";
    out << "\r\n";
    out << "[Files]\r\n";

    int  count   = 0;
    int  skipped = 0;

    std::cout << "Scanning: " << std::string(targetDir.begin(), targetDir.end()) << "\n\n";

    for (const auto& entry :
         fs::recursive_directory_iterator(
             targetDir, fs::directory_options::skip_permission_denied, ec))
    {
        if (!entry.is_regular_file(ec)) continue;
        if (ShouldSkip(entry.path()))   continue;

        // Build relative path with forward slashes (URL-friendly).
        // Explicit wchar_t->char cast is intentional — game file paths are ASCII.
        fs::path relPath = fs::relative(entry.path(), targetDir, ec);
        if (ec) continue;

        std::wstring relW = relPath.generic_wstring(); // generic_wstring uses '/' already
        std::string relStr;
        relStr.reserve(relW.size());
        for (wchar_t c : relW)
            relStr += static_cast<char>(c);

        std::string md5 = AutoUpdate::ComputeFileMD5(entry.path().wstring());
        if (md5.empty())
        {
            std::cout << "  [SKIP] " << relStr << " (cannot read)\n";
            ++skipped;
            continue;
        }

        out << relStr << "=" << md5 << "\r\n";
        std::cout << "  [OK]   " << relStr << "\n        -> " << md5 << "\n";
        ++count;
    }

    out.close();

    std::cout << "\n----------------------------------------\n";
    std::cout << "  Files indexed : " << count   << "\n";
    if (skipped > 0)
        std::cout << "  Skipped       : " << skipped << "\n";
    std::cout << "  Output        : " << outPath  << "\n";
    std::cout << "  BaseURL       : " << baseURL  << "\n";
    std::cout << "----------------------------------------\n";
    std::cout << "\nUpload the contents of this folder (including AutoUpdate.ini)\n";
    std::cout << "to: " << baseURL << "/\n";

    return 0;
}
