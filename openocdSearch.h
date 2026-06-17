#pragma once

#ifndef MODE_INTERNAL_INCLUDE_OPENOCD_SEARCH_H
#define MODE_INTERNAL_INCLUDE_OPENOCD_SEARCH_H

#include <windows.h>
#include <shlwapi.h>
#include <string>
#include <vector>
#include <algorithm>
#include <cctype>

#pragma comment(lib, "shlwapi.lib")

// 辅助：不区分大小写判断字符串是否包含子串
bool ContainsIgnoreCase(const std::wstring &str, const std::wstring &sub);
// 检查指定目录下是否存在 openocd.exe（直接或在 bin 子目录下）
std::wstring CheckOpenOCDInDir(const std::wstring &dir);

// 主搜索函数：当前目录，深度 ≤ 5，优先搜索名称含 "OpenOCD" 的目录
std::wstring SearchOpenOCDEfficient();
// 递归搜索（深度限制，目录名过滤器）
bool SearchRecursive(const std::wstring &dir, int depth, int maxDepth,
                     const std::wstring &filter, std::wstring &outPath);

/**
 * 从系统 PATH 环境变量中查找 openocd.exe
 * @return 找到的完整路径（宽字符串），如果未找到则返回空字符串
 */
std::wstring FindOpenOCDFromPath();

// 从 openocd.exe 路径枚举所有 target/*.cfg 文件
std::vector<std::wstring> EnumTargetCfgFiles(const std::wstring& openocdPath, std::wstring &targetDir, std::wstring &searchPath);


std::wstring GetCurrentWorkingDirectory();

#endif // MODE_INTERNAL_INCLUDE_OPENOCD_SEARCH_H