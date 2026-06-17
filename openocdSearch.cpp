#include "openocdSearch.h"

bool ContainsIgnoreCase(const std::wstring& str, const std::wstring& sub) {
    auto it = std::search(str.begin(), str.end(), sub.begin(), sub.end(),
        [](wchar_t ch1, wchar_t ch2) { return towlower(ch1) == towlower(ch2); });
    return it != str.end();
}

std::wstring CheckOpenOCDInDir(const std::wstring& dir) {
    std::wstring candidate = dir + L"\\openocd.exe";
    if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES)
        return candidate;
    candidate = dir + L"\\bin\\openocd.exe";
    if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES)
        return candidate;
    return L"";
}
bool SearchRecursive(const std::wstring& dir, int depth, int maxDepth,
                     const std::wstring& filter, std::wstring& outPath) {
    if (depth > maxDepth) return false;

    // 检查当前目录
    std::wstring found = CheckOpenOCDInDir(dir);
    if (!found.empty()) {
        outPath = found;
        return true;
    }

    // 枚举子目录
    std::wstring searchPath = dir + L"\\*";
    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(searchPath.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return false;

    std::vector<std::wstring> subdirs;
    do {
        if (fd.cFileName[0] == L'.') continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            std::wstring subdir = dir + L"\\" + fd.cFileName;
            // 如果设置了过滤器且目录名不包含关键词，则跳过
            if (!filter.empty() && !ContainsIgnoreCase(fd.cFileName, filter))
                continue;
            subdirs.push_back(subdir);
        }
    } while (FindNextFileW(hFind, &fd));
    FindClose(hFind);

    // 递归搜索符合条件的子目录
    for (const auto& subdir : subdirs) {
        if (SearchRecursive(subdir, depth + 1, maxDepth, filter, outPath))
            return true;
    }
    return false;
}

std::wstring SearchOpenOCDEfficient() {
    WCHAR curDir[MAX_PATH];
    GetCurrentDirectoryW(MAX_PATH, curDir);
    std::wstring startDir = curDir;

    std::wstring result;
    // 第一轮：只进入名称包含 "OpenOCD" 的目录
    if (SearchRecursive(startDir, 0, 5, L"OpenOCD", result))
        return result;
    // 第二轮：无限制搜索
    if (SearchRecursive(startDir, 0, 5, L"", result))
        return result;
    return L"";
}


/**
 * 从系统 PATH 环境变量中查找 openocd.exe
 * @return 找到的完整路径（宽字符串），如果未找到则返回空字符串
 */

 /**
  * 从系统 PATH 环境变量中查找 openocd.exe
  * @return 找到的完整路径（宽字符串），如果未找到则返回空字符串
  */
std::wstring FindOpenOCDFromPath() {
    // 1. 获取 PATH 环境变量所需缓冲区大小
    DWORD requiredSize = ::GetEnvironmentVariableW(L"PATH", nullptr, 0);
    if (requiredSize == 0) return L"";

    // 2. 动态分配缓冲区（RAII 自动释放）
    std::vector<wchar_t> buffer(requiredSize);
    DWORD len = ::GetEnvironmentVariableW(L"PATH", buffer.data(), requiredSize);
    if (len == 0 || len > requiredSize) return L"";

    // 3. 遍历 PATH 中的每个目录
    wchar_t* context = nullptr;
    wchar_t* token = ::wcstok_s(buffer.data(), L";", &context);
    while (token != nullptr) {
        if (token[0] == L'\0') {
            token = ::wcstok_s(nullptr, L";", &context);
            continue;
        }

        wchar_t fullPath[MAX_PATH];
        if (::PathCombineW(fullPath, token, L"openocd.exe") == nullptr) {
            token = ::wcstok_s(nullptr, L";", &context);
            continue;
        }

        DWORD attrs = ::GetFileAttributesW(fullPath);
        if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
            return std::wstring(fullPath);
        }

        token = ::wcstok_s(nullptr, L";", &context);
    }

    return L"";
}



// 从 openocd.exe 路径枚举所有 target/*.cfg 文件
std::vector<std::wstring> EnumTargetCfgFiles(const std::wstring& openocdPath, std::wstring &targetDir, std::wstring &searchPath) {
    std::vector<std::wstring> files;
    //return files;
    // 1. 获取 openocd.exe 所在目录
    std::wstring exeDir = openocdPath;
    for (size_t i = 0; i < exeDir.size(); i++)
    {
        auto& it = exeDir[i];
        if (it == '/') {
            exeDir[i] = '\\';
        }
    }
    size_t pos = exeDir.find_last_of(L'\\');

    if (pos == std::wstring::npos) {
        return files;
    }

    exeDir = exeDir.substr(0, pos);

    pos = exeDir.find_last_of(L'\\');

    if (pos == std::wstring::npos) {
        return files;
    }

    exeDir = exeDir.substr(0, pos);


    // 逆推 target 目录
    // D:\...\bin\openocd.exe -> D:\...\share\openocd\scripts\target
    targetDir = exeDir + L"\\share\\openocd\\scripts\\target";

    searchPath = targetDir + L"\\*.cfg";

    WIN32_FIND_DATAW findData;
    HANDLE hFind = FindFirstFileW(searchPath.c_str(), &findData);

    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            // 跳过目录，只取文件
            if (!(findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                files.push_back(findData.cFileName);
            }
        } while (FindNextFileW(hFind, &findData));
        FindClose(hFind);
    }

    return files;
}


std::wstring GetCurrentWorkingDirectory() {
    DWORD bufferSize = GetCurrentDirectoryW(0, nullptr);          // 获取所需缓冲区大小（含结尾'\0'）
    if (bufferSize == 0) return L"";

    std::vector<wchar_t> buffer(bufferSize);
    if (GetCurrentDirectoryW(bufferSize, buffer.data()) == 0) return L"";

    return std::wstring(buffer.data());
}
