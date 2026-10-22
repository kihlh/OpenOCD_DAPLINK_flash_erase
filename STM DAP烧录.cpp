#include <windows.h>
#include <process.h>
#include <string>
#include <iostream>
#include <strsafe.h>
#include <shlwapi.h>
#include <richedit.h>
#include <fstream>
#include "./json.hpp"   // nlohmann/json
#include "hmc_string_util.h"
#include <thread>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "strsafe.lib")

#include <windows.h>
#include <shlwapi.h>
#include "openocdSearch.h"
#pragma comment(lib, "shlwapi.lib")

// ====================== 控件ID定义 ======================
#define WND_CLASS_NAME L"OpenOCDFlashToolWnd"
#define IDC_EDIT_FILEPATH  1005
#define IDC_BTN_BROWSE     1006
#define IDC_LOG_EDIT       1001
#define IDC_HEX_LABEL      1002
#define IDC_BTN_RUN        1003
#define IDC_STATUS_LABEL   1004
#define IDC_COMBO_SPEED    1007
#define IDC_STATIC_SPEED   1008
#define IDC_COMBO_CFG    1009
#define IDC_CHECK_REBOOT    1010
#define IDC_CHECK_AUTO_DOWNLOAD    1011
#define IDC_BTN_REST        1012
#define IDC_BTN_REST_BOOT        1013
#define IDC_BTN_STOP        1014
#define IDC_LAST_FLASH_LABEL 1015

#define CONFIG_FILE L"config.json"

// ====================== 全局变量 ======================
HWND g_hWnd = NULL;
HWND g_hEditLog = NULL;
HWND g_hEditFilePath = NULL;
HWND g_hLabelHex = NULL;
HWND g_hLabelStatus = NULL;
HWND g_hComboSpeed = NULL;
HWND g_hComboCfg = NULL;
HFONT g_hFontBold = NULL;
HANDLE g_hWatchThread = NULL;
const DWORD WATCH_INTERVAL = 300;
const DWORD DEBOUNCE_DELAY = 3000;
FILETIME g_prevFileTime = { 0 };
BOOL g_bIsFlashing = FALSE;
DWORD g_adapterSpeed = 100000;
std::wstring g_openocdPath = L"openocd.exe";
std::wstring g_szCurrentFile = L"";

BOOL g_bIsAutoFlashDownload = TRUE;

HANDLE g_hCurrentProcess = NULL;   // 当前OpenOCD进程句柄
DWORD g_lastFlashTick = 0;          // 上次成功烧录时间戳
HWND g_hLastFlashLabel = NULL;      // 上次烧录时间标签

// ====================== OpenOCD 命令参数结构体 ======================
typedef struct _OpenOCDConfig {
    std::wstring szInterface;
    std::wstring szTransport;
    std::wstring szTarget;
    BOOL bInit;
    BOOL bResetHalt;
    BOOL bWaitHalt;
    BOOL bFlashWrite;
    WCHAR szFlashFilePath[MAX_PATH];
    BOOL bReset;
    BOOL bShutdown;
} OpenOCDConfig;

OpenOCDConfig g_openocdConfig;

// ====================== 函数声明 ======================
BOOL FindFirstHexFile(std::wstring& szOutPath, const wchar_t* pszFilter, bool jumpOut);
void AppendLog(const WCHAR* szText);
void RunOpenOCDFlash();
unsigned int __stdcall WatchHexFileThread(void* pParam);
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
void HandleDropFiles(HWND hWnd, HDROP hDrop);
void RefreshFileLabel();
void SetCurrentFile(const wchar_t* pszFile, bool isUpdate);
void InitOpenOCDConfig();
void BuildOpenOCDCommand(const OpenOCDConfig* pConfig, WCHAR* szCmdOut, DWORD dwSize);
void SaveConfig();
void LoadConfig();
DWORD GetSpeedValueFromText(const WCHAR* szText);
int GetSpeedIndexFromValue(DWORD speedKHz);
std::wstring SearchOpenOCD();

// ====================== 辅助字符串转换（如果 hmc_string_util 已提供可删除） ======================

// ====================== 异步烧录相关定义 ======================
#define WM_APPEND_LOG   (WM_USER + 101)   // wParam: 字符串指针（需要释放）
#define WM_SET_STATUS   (WM_USER + 102)   // wParam: 状态字符串指针（需要释放）
#define WM_FLASH_DONE   (WM_USER + 103)   // wParam: 退出码, lParam: 是否成功

struct FlashThreadParam {
    std::wstring openocdPath;
    std::wstring flashFilePath;
    std::wstring workDir;
    std::wstring cmdLine;
    DWORD adapterSpeed;
    OpenOCDConfig config;          // 拷贝一份配置
    HWND hWnd;                     // 主窗口句柄
};

// 强制终止当前OpenOCD进程
void StopOpenOCDProcess()
{
    if (g_hCurrentProcess != NULL)
    {
        TerminateProcess(g_hCurrentProcess, 0);
        CloseHandle(g_hCurrentProcess);
        g_hCurrentProcess = NULL;
        g_bIsFlashing = FALSE;
        AppendLog(L"\r\n已强制终止 OpenOCD 进程\r\n");
        SetWindowTextW(g_hLabelStatus, L"状态：已停止");
        EnableWindow(GetDlgItem(g_hWnd, IDC_BTN_STOP), FALSE);
    }
}

// 刷新上次烧录时间显示
void UpdateLastFlashLabel()
{
    if (!g_hLastFlashLabel) return;
    if (g_lastFlashTick == 0)
    {
        SetWindowTextW(g_hLastFlashLabel, L"上次烧录：无");
        return;
    }
    DWORD diffSec = (GetTickCount() - g_lastFlashTick) / 1000;
    WCHAR szBuf[128];
    if (diffSec < 60)
        StringCchPrintfW(szBuf, _countof(szBuf), L"上次烧录：%u秒前", diffSec);
    else if (diffSec < 3600)
        StringCchPrintfW(szBuf, _countof(szBuf), L"上次烧录：%u分钟前", diffSec / 60);
    else
        StringCchPrintfW(szBuf, _countof(szBuf), L"上次烧录：%u小时前", diffSec / 3600);
    SetWindowTextW(g_hLastFlashLabel, szBuf);
}


// 工作线程函数
unsigned int __stdcall FlashThreadProc(void* lpParam) {
    std::unique_ptr<FlashThreadParam> param(static_cast<FlashThreadParam*>(lpParam));

    // 发送开始日志（可选）
    //PostMessage(param->hWnd, WM_APPEND_LOG, 0, (LPARAM)new std::wstring(L"------------------------------\r\n开始执行\r\n------------------------------\r\n"));
    
    SYSTEMTIME st{};
    GetLocalTime(&st);

    WCHAR timeBuf[32]{};
    StringCchPrintfW(timeBuf, _countof(timeBuf),
        L"%04d-%02d-%02d %02d:%02d:%02d",
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond);

    std::wstring* pLog = new std::wstring(
        L"------------------------------\r\n"
        L"[" + std::wstring(timeBuf) + L"] 开始执行\r\n"
        L"------------------------------\r\n"
    );
    PostMessage(param->hWnd, WM_APPEND_LOG, 0, (LPARAM)pLog);


    EnableWindow(GetDlgItem(g_hWnd, IDC_BTN_STOP), TRUE);


    g_lastFlashTick = GetTickCount();
    UpdateLastFlashLabel();

    // 管道创建
    SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), NULL, TRUE };
    HANDLE hReadOut, hWriteOut, hReadErr, hWriteErr;
    CreatePipe(&hReadOut, &hWriteOut, &sa, 0);
    CreatePipe(&hReadErr, &hWriteErr, &sa, 0);
    g_hCurrentProcess = NULL;
    STARTUPINFOW si = { sizeof(STARTUPINFOW) };
    PROCESS_INFORMATION pi = { 0 };
    si.hStdOutput = hWriteOut;
    si.hStdError = hWriteErr;
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    BOOL bSuccess = CreateProcessW(param->openocdPath.c_str(), (LPWSTR)param->cmdLine.c_str(),
        NULL, NULL, TRUE, 0, NULL, param->workDir.c_str(), &si, &pi);
    CloseHandle(hWriteOut);
    CloseHandle(hWriteErr);

    if (!bSuccess) {
        WCHAR szErr[512];
        StringCchPrintfW(szErr, 512, L" OpenOCD启动失败，错误码：%d\r\n", GetLastError());
        PostMessage(param->hWnd, WM_APPEND_LOG, 0, (LPARAM)new std::wstring(szErr));
        PostMessage(param->hWnd, WM_SET_STATUS, 0, (LPARAM)new std::wstring(L"状态：启动失败"));
        CloseHandle(hReadOut);
        CloseHandle(hReadErr);
        if (pi.hProcess) CloseHandle(pi.hProcess);
        if (pi.hThread) CloseHandle(pi.hThread);
        PostMessage(param->hWnd, WM_FLASH_DONE, 1, 0);  // 失败
        return 1;
    }
    g_hCurrentProcess = pi.hProcess; // 新增：保存进程句柄
    // 读取标准输出和错误（非阻塞读取，但这里依旧使用阻塞读，但放在线程中不影响UI）
    char buf[4096];
    DWORD readBytes;
    while (ReadFile(hReadOut, buf, sizeof(buf) - 1, &readBytes, NULL) && readBytes > 0) {
        buf[readBytes] = 0;
        WCHAR wide[4096];
        MultiByteToWideChar(CP_UTF8, 0, buf, readBytes, wide, 4096);
        PostMessage(param->hWnd, WM_APPEND_LOG, 0, (LPARAM)new std::wstring(wide));
    }
    while (ReadFile(hReadErr, buf, sizeof(buf) - 1, &readBytes, NULL) && readBytes > 0) {
        buf[readBytes] = 0;
        WCHAR wide[4096];
        MultiByteToWideChar(CP_UTF8, 0, buf, readBytes, wide, 4096);
        PostMessage(param->hWnd, WM_APPEND_LOG, 0, (LPARAM)new std::wstring(wide));
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    g_hCurrentProcess = NULL; // 新增：进程已结束，清空全局句柄
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);

    WCHAR szResult[512];
    if (exitCode == 0) {
        StringCchPrintfW(szResult, 512, L"\r\n烧录完成，退出码：%d\r\n 烧录成功！\r\n", exitCode);
        PostMessage(param->hWnd, WM_SET_STATUS, 0, (LPARAM)new std::wstring(L"状态：烧录成功，等待文件更新..."));
    }
    else {
        StringCchPrintfW(szResult, 512, L"\r\n烧录完成，退出码：%d\r\n 烧录失败！\r\n", exitCode);
        PostMessage(param->hWnd, WM_SET_STATUS, 0, (LPARAM)new std::wstring(L"状态：烧录失败，等待文件更新..."));
    }
    PostMessage(param->hWnd, WM_APPEND_LOG, 0, (LPARAM)new std::wstring(szResult));

    // 保存配置（原逻辑中有 SaveConfig，这里需要调用，但 SaveConfig 涉及文件写，可以在线程中做）
    // 注意：SaveConfig 内部使用了全局变量，需要确保线程安全。由于此时 UI 线程可能正在读配置，但频率极低，可以接受。
    // 为了更安全，可以将 SaveConfig 所需的数据打包，由主线程执行。这里简化，直接调用（因为主线程此时不写配置）。
    SaveConfig();

    try
    {
        CloseHandle(hReadOut);
        CloseHandle(hReadErr);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    catch (const std::exception&)
    {

    }
    catch (...) {

    }


    PostMessage(param->hWnd, WM_FLASH_DONE, exitCode, 0);
    return 0;
}

// 修改 RunOpenOCDFlash 为异步启动
void RunOpenOCDFlash() {
    
    g_lastFlashTick = GetTickCount();
    UpdateLastFlashLabel();

    if (g_bIsFlashing) {
        AppendLog(L"正在烧录中，请等待完成！\r\n");
        return;
    }
    if (g_szCurrentFile.empty()) {
        AppendLog(L"未设置烧录文件路径！\r\n");
        return;
    }

    g_bIsFlashing = TRUE;
    SetWindowTextW(g_hLabelStatus, L"状态：正在烧录...");

    // 更新配置对象中的文件路径
    StringCchCopyW(g_openocdConfig.szFlashFilePath, MAX_PATH, g_szCurrentFile.c_str());

    // 从下拉框获取速率（必须在UI线程执行，现在提前获取）
    if (g_hComboSpeed) {
        WCHAR buf[32] = { 0 };
        int sel = ::SendMessageW(g_hComboSpeed, CB_GETCURSEL, 0, 0);
        ::SendMessageW(g_hComboSpeed, CB_GETLBTEXT, sel, (LPARAM)buf);
        g_adapterSpeed = GetSpeedValueFromText(buf);
        WCHAR logStr[128];
        ::StringCchPrintfW(logStr, ARRAYSIZE(logStr), L"下载速率已切换为：%s (%lu kHz)\r\n", buf, g_adapterSpeed);
        AppendLog(logStr);
        SaveConfig();
    }

    // 构建命令行
    WCHAR szCmdLine[4096] = { 0 };
    BuildOpenOCDCommand(&g_openocdConfig, szCmdLine, 4096);

    AppendLog(L"执行文件: \r\n");
    AppendLog(g_openocdPath.c_str());
    AppendLog(L"\r\n");
    AppendLog(L"执行命令: \r\n");
    AppendLog(szCmdLine);
    AppendLog(L"\r\n");

    // 工作目录
    WCHAR szWorkDir[MAX_PATH];
    StringCchCopyW(szWorkDir, MAX_PATH, g_szCurrentFile.c_str());
    PathRemoveFileSpecW(szWorkDir);
    AppendLog(L"工作目录: ");
    AppendLog(szWorkDir);
    AppendLog(L"\r\n");

    // 准备线程参数
    FlashThreadParam* param = new FlashThreadParam();
    param->openocdPath = g_openocdPath;
    param->flashFilePath = g_szCurrentFile;
    param->workDir = szWorkDir;
    param->cmdLine = szCmdLine;
    param->adapterSpeed = g_adapterSpeed;
    param->config = g_openocdConfig;   // 拷贝当前配置
    param->hWnd = g_hWnd;

    // 创建工作线程
    HANDLE hThread = (HANDLE)_beginthreadex(NULL, 0, FlashThreadProc, param, 0, NULL);
    if (hThread) CloseHandle(hThread);   // 不需要等待，关闭句柄即可
    else {
        delete param;
        AppendLog(L"创建烧录线程失败！\r\n");
        SetWindowTextW(g_hLabelStatus, L"状态：启动失败");
        g_bIsFlashing = FALSE;
    }
}

// ====================== 初始化配置 ======================
void InitOpenOCDConfig() {
    g_openocdConfig.szInterface = L"cmsis-dap.cfg";
    g_openocdConfig.szTransport = L"swd";
    g_openocdConfig.szTarget = L"stm32f1x.cfg";
    g_openocdConfig.bInit = TRUE;
    g_openocdConfig.bResetHalt = TRUE;
    g_openocdConfig.bWaitHalt = TRUE;
    g_openocdConfig.bFlashWrite = TRUE;
    ZeroMemory(g_openocdConfig.szFlashFilePath, sizeof(g_openocdConfig.szFlashFilePath));
    g_openocdConfig.bReset = TRUE;
    g_openocdConfig.bShutdown = TRUE;
}

// ====================== 构建OpenOCD命令行 ======================
void BuildOpenOCDCommand(const OpenOCDConfig* pConfig, WCHAR* szCmdOut, DWORD dwSize) {
    WCHAR szCmd[4096] = { 0 };
    WCHAR szBuffer[512];

    StringCchPrintfW(szBuffer, 512, L" -c \"source [find interface/%ls]\"", pConfig->szInterface.c_str());
    StringCchCatW(szCmd, 4096, szBuffer);
    StringCchPrintfW(szBuffer, 512, L" -c \"transport select %ls\"", pConfig->szTransport.c_str());
    StringCchCatW(szCmd, 4096, szBuffer);
    StringCchPrintfW(szBuffer, 512, L" -c \"source [find target/%ls]\"", pConfig->szTarget.c_str());
    StringCchCatW(szCmd, 4096, szBuffer);
    StringCchPrintfW(szBuffer, 512, L" -c \"adapter speed %lu\"", g_adapterSpeed);
    StringCchCatW(szCmd, 4096, szBuffer);

    if (pConfig->bInit) StringCchCatW(szCmd, 4096, L" -c init");
    if (pConfig->bResetHalt) StringCchCatW(szCmd, 4096, L" -c \"reset halt\"");

    if (pConfig->bFlashWrite && wcslen(pConfig->szFlashFilePath) > 0) {
        WCHAR* pFileName = PathFindFileNameW(pConfig->szFlashFilePath);
        BOOL bNeedQuote = (wcschr(pConfig->szFlashFilePath, L' ') != NULL);
        if (bNeedQuote)
            StringCchPrintfW(szBuffer, 512, L" -c \"flash write_image erase \\\"%ls\\\"\"", pFileName);
        else
            StringCchPrintfW(szBuffer, 512, L" -c \"flash write_image erase %ls\"", pFileName);
        StringCchCatW(szCmd, 4096, szBuffer);
    }

    if (pConfig->bReset) StringCchCatW(szCmd, 4096, L" -c reset");
    if (pConfig->bShutdown) StringCchCatW(szCmd, 4096, L" -c shutdown");

    StringCchCopyW(szCmdOut, dwSize, szCmd);
}

// ====================== 设置当前文件路径 ======================
void SetCurrentFile(const wchar_t* pszFile, bool isUpdate) {
    if (pszFile) g_szCurrentFile = pszFile;
    else g_szCurrentFile.clear();
    if (isUpdate) SaveConfig();
}

// ====================== 速率值验证函数 ======================
static BOOL IsValidAdapterSpeed(DWORD speed) {
    // 支持所有 1kHz 到 10000kHz 的值，但为了配置文件的整洁性，还是验证一下
    // 实际支持：1,5,10,20,50,100,200,500,1000,5000,10000
    return (speed == 1 || speed == 5 || speed == 10 || speed == 20 ||
        speed == 50 || speed == 100 || speed == 200 || speed == 500 ||
        speed == 1000 || speed == 5000 || speed == 10000);
}


// ====================== JSON 配置保存/加载 ======================
void SaveConfig1() {
    try {
        nlohmann::json config;
        config["Settings"]["FlashHexFile"] = hmc_string_util::utf16_to_utf8(g_szCurrentFile);
        config["Settings"]["AdapterSpeed"] = g_adapterSpeed;
        config["Settings"]["OpenOCDPath"] = hmc_string_util::utf16_to_utf8(g_openocdPath);

        config["OpenOCD"]["Interface"] = hmc_string_util::utf16_to_utf8(g_openocdConfig.szInterface);
        config["OpenOCD"]["Transport"] = hmc_string_util::utf16_to_utf8(g_openocdConfig.szTransport);
        config["OpenOCD"]["Target"] = hmc_string_util::utf16_to_utf8(g_openocdConfig.szTarget);
        config["OpenOCD"]["Init"] = (g_openocdConfig.bInit == TRUE);
        config["OpenOCD"]["ResetHalt"] = (g_openocdConfig.bResetHalt == TRUE);
        config["OpenOCD"]["WaitHalt"] = (g_openocdConfig.bWaitHalt == TRUE);
        config["OpenOCD"]["FlashWrite"] = (g_openocdConfig.bFlashWrite == TRUE);
        config["OpenOCD"]["Reset"] = (g_openocdConfig.bReset == TRUE);
        config["OpenOCD"]["Shutdown"] = (g_openocdConfig.bShutdown == TRUE);

        std::ofstream fout(CONFIG_FILE);
        if (fout.is_open()) {
            fout << config.dump(4);
            fout.close();
        }
    }
    catch (const std::exception& e) {
        OutputDebugStringA("SaveConfig failed: ");
        OutputDebugStringA(e.what());
    }
}

void LoadConfig1() {
    try {
        std::ifstream fin(CONFIG_FILE);
        if (!fin.is_open()) {
            g_openocdPath = FindOpenOCDFromPath();
            SaveConfig();
            return;
        }
        nlohmann::json config;
        fin >> config;
        fin.close();

        if (config.contains("Settings")) {
            auto& s = config["Settings"];
            if (s.contains("FlashHexFile") && s["FlashHexFile"].is_string())
                g_szCurrentFile =hmc_string_util::utf8_to_utf16(s["FlashHexFile"].get<std::string>());
            if (s.contains("AdapterSpeed") && s["AdapterSpeed"].is_number()) {
                int sp = s["AdapterSpeed"];
                if (sp == 1000 || sp == 5000 || sp == 10000 || sp == 20000 || sp == 50000 || sp == 100000)
                    g_adapterSpeed = sp;
            }
            if (s.contains("OpenOCDPath") && s["OpenOCDPath"].is_string())
                g_openocdPath = hmc_string_util::utf8_to_utf16(s["OpenOCDPath"].get<std::string>());
        }
        if (config.contains("OpenOCD")) {
            auto& o = config["OpenOCD"];
            if (o.contains("Interface") && o["Interface"].is_string())
                g_openocdConfig.szInterface = hmc_string_util::utf8_to_utf16(o["Interface"].get<std::string>());
            if (o.contains("Transport") && o["Transport"].is_string())
                g_openocdConfig.szTransport = hmc_string_util::utf8_to_utf16(o["Transport"].get<std::string>());
            if (o.contains("Target") && o["Target"].is_string())
                g_openocdConfig.szTarget = hmc_string_util::utf8_to_utf16(o["Target"].get<std::string>());
            if (o.contains("Init") && o["Init"].is_boolean())
                g_openocdConfig.bInit = o["Init"].get<bool>() ? TRUE : FALSE;
            if (o.contains("ResetHalt") && o["ResetHalt"].is_boolean())
                g_openocdConfig.bResetHalt = o["ResetHalt"].get<bool>() ? TRUE : FALSE;
            if (o.contains("WaitHalt") && o["WaitHalt"].is_boolean())
                g_openocdConfig.bWaitHalt = o["WaitHalt"].get<bool>() ? TRUE : FALSE;
            if (o.contains("FlashWrite") && o["FlashWrite"].is_boolean())
                g_openocdConfig.bFlashWrite = o["FlashWrite"].get<bool>() ? TRUE : FALSE;
            if (o.contains("Reset") && o["Reset"].is_boolean())
                g_openocdConfig.bReset = o["Reset"].get<bool>() ? TRUE : FALSE;
            if (o.contains("Shutdown") && o["Shutdown"].is_boolean())
                g_openocdConfig.bShutdown = o["Shutdown"].get<bool>() ? TRUE : FALSE;
        }
        if (GetFileAttributesW(g_openocdPath.c_str()) == INVALID_FILE_ATTRIBUTES)
            g_openocdPath = L"openocd.exe";
    }
    catch (const std::exception& e) {
        InitOpenOCDConfig();
        g_adapterSpeed = 100000;
        g_openocdPath = L"openocd.exe";
        SaveConfig();
    }
}

std::wstring SearchOpenOCD() {

    std::wstring foundPath;
    foundPath.reserve(MAX_PATH);
    bool isRootCwd = PathIsRootW(GetCurrentWorkingDirectory().c_str());

    // 配置文件不存在，从 PATH 查找 openocd.exe
    if (isRootCwd) {
        MessageBoxW(g_hWnd, L"请勿把本软件放在根目录中，本软件启动时会检索目录下的openocd.exe，将会导致软件卡顿", L"错误", MB_ICONERROR | MB_OK);
    }


    if (!isRootCwd) {
        if (foundPath.empty()) {
            foundPath = SearchOpenOCDEfficient();

            if (!foundPath.empty()) {
                g_openocdPath = foundPath;
                AppendLog(L"在软件根目录下中找到 openocd.exe\r\n");
            }
        }
    }


    if (foundPath.empty()) {
       foundPath = FindOpenOCDFromPath();

        if (!foundPath.empty()) {
            g_openocdPath = foundPath;
            AppendLog(L"在环境变量中找到 openocd.exe\r\n");
        }
    }


    if (foundPath.empty()) {
        foundPath = L"openocd.exe";
        AppendLog(L"找不到 openocd.exe\r\n");
    }

    SaveConfig();

    g_openocdPath = foundPath;

	return foundPath;
}


// ====================== JSON 配置保存/加载 ======================
void SaveConfig() {
    try {
        nlohmann::json config;

        // Settings 节点
        config["Settings"]["FlashHexFile"] = hmc_string_util::utf16_to_utf8(g_szCurrentFile);
        config["Settings"]["AdapterSpeed"] = g_adapterSpeed;  // 存储数值（kHz）
        config["Settings"]["OpenOCDPath"] = hmc_string_util::utf16_to_utf8(g_openocdPath);
        config["Settings"]["IsAutoFlashDownload"] = (g_bIsAutoFlashDownload == TRUE); 


        // OpenOCD 配置节点
        config["OpenOCD"]["Interface"] = hmc_string_util::utf16_to_utf8(g_openocdConfig.szInterface);
        config["OpenOCD"]["Transport"] = hmc_string_util::utf16_to_utf8(g_openocdConfig.szTransport);
        config["OpenOCD"]["Target"] = hmc_string_util::utf16_to_utf8(g_openocdConfig.szTarget);
        config["OpenOCD"]["Init"] = (g_openocdConfig.bInit == TRUE);
        config["OpenOCD"]["ResetHalt"] = (g_openocdConfig.bResetHalt == TRUE);
        config["OpenOCD"]["WaitHalt"] = (g_openocdConfig.bWaitHalt == TRUE);
        config["OpenOCD"]["FlashWrite"] = (g_openocdConfig.bFlashWrite == TRUE);
        config["OpenOCD"]["Reset"] = (g_openocdConfig.bReset == TRUE);
        config["OpenOCD"]["Shutdown"] = (g_openocdConfig.bShutdown == TRUE);

        std::ofstream fout(CONFIG_FILE);
        if (fout.is_open()) {
            fout << config.dump(4);  // 格式化输出，缩进4空格
            fout.close();
        }
    }
    catch (const std::exception& e) {
        OutputDebugStringA("SaveConfig failed: ");
        OutputDebugStringA(e.what());
    }
}

void LoadConfig() {
    try {
        std::ifstream fin(CONFIG_FILE);
        if (!fin.is_open()) {
            SearchOpenOCD();
            return;
        }

        nlohmann::json config;
        fin >> config;
        fin.close();

        // 读取 Settings 节点
        if (config.contains("Settings")) {
            auto& s = config["Settings"];

            // 读取烧录文件路径
            if (s.contains("FlashHexFile") && s["FlashHexFile"].is_string())
                g_szCurrentFile = hmc_string_util::utf8_to_utf16(s["FlashHexFile"].get<std::string>());

            // 读取速率（兼容新旧格式）
            if (s.contains("AdapterSpeed")) {
                if (s["AdapterSpeed"].is_number()) {
                    int sp = s["AdapterSpeed"];
                    // 验证速率是否为有效值（支持新的速率范围）
                    if (sp == 1 || sp == 5 || sp == 10 || sp == 20 || sp == 50 ||
                        sp == 100 || sp == 200 || sp == 500 || sp == 1000 ||
                        sp == 5000 || sp == 10000) {
                        g_adapterSpeed = sp;
                    }
                    else {
                        // 无效速率，使用默认值 10MHz
                        g_adapterSpeed = 10000;
                    }
                }
            }

            // 读取 OpenOCD 路径
            if (s.contains("OpenOCDPath") && s["OpenOCDPath"].is_string())
                g_openocdPath = hmc_string_util::utf8_to_utf16(s["OpenOCDPath"].get<std::string>());


            if (s.contains("IsAutoFlashDownload") && s["IsAutoFlashDownload"].is_boolean())
                g_bIsAutoFlashDownload = s["IsAutoFlashDownload"].get<bool>() ? TRUE : FALSE;

        }

        // 读取 OpenOCD 配置节点
        if (config.contains("OpenOCD")) {
            auto& o = config["OpenOCD"];

            if (o.contains("Interface") && o["Interface"].is_string())
                g_openocdConfig.szInterface = hmc_string_util::utf8_to_utf16(o["Interface"].get<std::string>());

            if (o.contains("Transport") && o["Transport"].is_string())
                g_openocdConfig.szTransport = hmc_string_util::utf8_to_utf16(o["Transport"].get<std::string>());

            if (o.contains("Target") && o["Target"].is_string())
                g_openocdConfig.szTarget = hmc_string_util::utf8_to_utf16(o["Target"].get<std::string>());

            if (o.contains("Init") && o["Init"].is_boolean())
                g_openocdConfig.bInit = o["Init"].get<bool>() ? TRUE : FALSE;

            if (o.contains("ResetHalt") && o["ResetHalt"].is_boolean())
                g_openocdConfig.bResetHalt = o["ResetHalt"].get<bool>() ? TRUE : FALSE;

            if (o.contains("WaitHalt") && o["WaitHalt"].is_boolean())
                g_openocdConfig.bWaitHalt = o["WaitHalt"].get<bool>() ? TRUE : FALSE;

            if (o.contains("FlashWrite") && o["FlashWrite"].is_boolean())
                g_openocdConfig.bFlashWrite = o["FlashWrite"].get<bool>() ? TRUE : FALSE;

            if (o.contains("Reset") && o["Reset"].is_boolean())
                g_openocdConfig.bReset = o["Reset"].get<bool>() ? TRUE : FALSE;

            if (o.contains("Shutdown") && o["Shutdown"].is_boolean())
                g_openocdConfig.bShutdown = o["Shutdown"].get<bool>() ? TRUE : FALSE;
        }

        // 验证 OpenOCD 路径是否有效，无效则从系统 PATH 查找
        if (GetFileAttributesW(g_openocdPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
            std::wstring foundPath = SearchOpenOCD();
            if (!foundPath.empty()) {
                g_openocdPath = foundPath;
            }
            else {
                g_openocdPath = L"openocd.exe";
            }
        }
    }
    catch (const std::exception& e) {
        // 配置文件损坏，使用默认配置并重新保存
        InitOpenOCDConfig();
        g_adapterSpeed = 10000;  // 默认 10MHz
        // 尝试从 PATH 查找 openocd.exe
        std::wstring foundPath = SearchOpenOCD();
        g_openocdPath = foundPath.empty() ? L"openocd.exe" : foundPath;
        SaveConfig();
    }
}

// ====================== 查找第一个 hex/bin ======================
BOOL FindFirstHexFile(std::wstring& szOutPath, const wchar_t* pszFilter, bool jumpOut) {
    WCHAR szExeDir[MAX_PATH];
    GetModuleFileNameW(NULL, szExeDir, MAX_PATH);
    PathRemoveFileSpecW(szExeDir);

    WCHAR szSearch[MAX_PATH];
    StringCchPrintfW(szSearch, MAX_PATH, pszFilter, szExeDir);

    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(szSearch, &fd);
    if (hFind == INVALID_HANDLE_VALUE) {
        if (jumpOut) return FALSE;
        return FindFirstHexFile(szOutPath, L"%s\\*.bin", TRUE);
    }
    WCHAR szFull[MAX_PATH];
    PathCombineW(szFull, szExeDir, fd.cFileName);
    szOutPath = szFull;
    FindClose(hFind);
    return TRUE;
}

// ====================== 日志输出 ======================
void AppendLog(const WCHAR* szText) {
    if (!g_hEditLog) return;
    SendMessageW(g_hEditLog, EM_SETSEL, 0, 0);
    int len = GetWindowTextLengthW(g_hEditLog);
    SendMessageW(g_hEditLog, EM_SETSEL, len, len);
    SendMessageW(g_hEditLog, EM_REPLACESEL, 0, (LPARAM)szText);
    SendMessageW(g_hEditLog, EM_SCROLLCARET, 0, 0);
    InvalidateRect(g_hEditLog, NULL, TRUE);
    UpdateWindow(g_hEditLog);
}

// ====================== 刷新文件标签 ======================
void RefreshFileLabel() {
    WCHAR szBuf[1024];
    if (g_szCurrentFile.empty())
        StringCchPrintfW(szBuf, 1024, L"当前无有效烧录文件");
    else
        StringCchPrintfW(szBuf, 1024, L"待烧录文件：%s", g_szCurrentFile.c_str());
    SetWindowTextW(g_hLabelHex, szBuf);
    InvalidateRect(g_hLabelHex, NULL, TRUE);
    UpdateWindow(g_hLabelHex);
}

// ====================== 拖放处理 ======================
void HandleDropFiles(HWND hWnd, HDROP hDrop) {
    UINT fileCount = DragQueryFileW(hDrop, 0xFFFFFFFF, NULL, 0);
    for (UINT i = 0; i < fileCount; i++) {
        WCHAR szDropPath[MAX_PATH];
        DragQueryFileW(hDrop, i, szDropPath, MAX_PATH);
        wchar_t* szExt = PathFindExtensionW(szDropPath);
        if (_wcsicmp(szExt, L".hex") == 0 || _wcsicmp(szExt, L".bin") == 0) {
            SetCurrentFile(szDropPath, false);
            SetWindowTextW(g_hEditFilePath, szDropPath);
            RefreshFileLabel();
            AppendLog(L"拖入烧录文件：");
            AppendLog(szDropPath);
            AppendLog(L"\r\n");
            SaveConfig();
            break;
        }
    }
    DragFinish(hDrop);
}


// ====================== 烧录执行 ======================
void RunOpenOCDFlashSync() {
    if (g_bIsFlashing) {
        AppendLog(L"正在烧录中，请等待完成！\r\n");
        return;
    }
    if (g_szCurrentFile.empty()) {
        AppendLog(L"未设置烧录文件路径！\r\n");
        return;
    }


    g_bIsFlashing = TRUE;
    SetWindowTextW(g_hLabelStatus, L"状态：正在烧录...");

    // 更新配置对象中的文件路径
    StringCchCopyW(g_openocdConfig.szFlashFilePath, MAX_PATH, g_szCurrentFile.c_str());

    // 从下拉框获取速率（如果用户已改变）
    if (g_hComboSpeed) {
        //WCHAR buf[32] = { 0 };
        //int sel = SendMessageW(g_hComboSpeed, CB_GETCURSEL, 0, 0);
        //if (sel != CB_ERR) {
        //    SendMessageW(g_hComboSpeed, CB_GETLBTEXT, sel, (LPARAM)buf);
        //    g_adapterSpeed = _wtoi(buf);
        //    SaveConfig();
        //}

        WCHAR buf[32] = { 0 };
        int sel = ::SendMessageW(g_hComboSpeed, CB_GETCURSEL, 0, 0);
        ::SendMessageW(g_hComboSpeed, CB_GETLBTEXT, sel, (LPARAM)buf);

        // 将显示文本转换为数值
        g_adapterSpeed = GetSpeedValueFromText(buf);

        WCHAR logStr[128] = { 0 };
        ::StringCchPrintfW(logStr, ARRAYSIZE(logStr), L"下载速率已切换为：%s (%lu kHz)\r\n", buf, g_adapterSpeed);
        AppendLog(logStr);

        ::UpdateWindow(g_hWnd);
        SaveConfig();

    }

    // 构建命令行
    WCHAR szCmdLine[4096] = { 0 };
    BuildOpenOCDCommand(&g_openocdConfig, szCmdLine, 4096);

    AppendLog(L"执行文件: \r\n");
    AppendLog(g_openocdPath.c_str());
    AppendLog(L"\r\n");
    AppendLog(L"执行命令: \r\n");
    AppendLog(szCmdLine);
    AppendLog(L"\r\n");

    // 工作目录（烧录文件所在目录）
    WCHAR szWorkDir[MAX_PATH];
    StringCchCopyW(szWorkDir, MAX_PATH, g_szCurrentFile.c_str());
    PathRemoveFileSpecW(szWorkDir);

    // 管道
    SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), NULL, TRUE };
    HANDLE hReadOut, hWriteOut, hReadErr, hWriteErr;
    CreatePipe(&hReadOut, &hWriteOut, &sa, 0);
    CreatePipe(&hReadErr, &hWriteErr, &sa, 0);

    STARTUPINFOW si = { sizeof(STARTUPINFOW) };
    PROCESS_INFORMATION pi = { 0 };
    si.hStdOutput = hWriteOut;
    si.hStdError = hWriteErr;
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    BOOL bSuccess = CreateProcessW(g_openocdPath.c_str(), szCmdLine, NULL, NULL, TRUE,
        0, NULL, szWorkDir, &si, &pi);
    CloseHandle(hWriteOut);
    CloseHandle(hWriteErr);

    AppendLog(L"工作目录: ");
    AppendLog(szWorkDir);
    AppendLog(L"\r\n");
    SYSTEMTIME st{};
    GetLocalTime(&st);

    WCHAR timeBuf[32]{};
    StringCchPrintfW(timeBuf, _countof(timeBuf),
        L"%04d-%02d-%02d %02d:%02d:%02d",
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond);

    std::wstring* pLog = new std::wstring(
        L"------------------------------\r\n"
        L"[" + std::wstring(timeBuf) + L"] 开始执行\r\n"
        L"------------------------------\r\n"
    );

    AppendLog(pLog->c_str());

    if (!bSuccess) {
        WCHAR szErr[512];
        StringCchPrintfW(szErr, 512, L" OpenOCD启动失败，错误码：%d\r\n", GetLastError());
        AppendLog(szErr);
        SetWindowTextW(g_hLabelStatus, L"状态：启动失败");
        CloseHandle(hReadOut);
        CloseHandle(hReadErr);
        if (pi.hProcess) CloseHandle(pi.hProcess);
        if (pi.hThread) CloseHandle(pi.hThread);
        g_bIsFlashing = FALSE;
        return;
    }

    // 读取输出
    char buf[4096] = {0};
    DWORD readBytes;

    while (ReadFile(hReadOut, buf, sizeof(buf) - 1, &readBytes, NULL) && readBytes > 0) {
        buf[readBytes] = 0;
        WCHAR wide[4096] = { 0 };
        MultiByteToWideChar(CP_UTF8, 0, buf, readBytes, wide, 4096);
        AppendLog(wide);
    }

    while (ReadFile(hReadErr, buf, sizeof(buf) - 1, &readBytes, NULL) && readBytes > 0) {
        buf[readBytes] = 0;
        WCHAR wide[4096] = {0};
        MultiByteToWideChar(CP_UTF8, 0, buf, readBytes, wide, 4096);
        AppendLog(wide);
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);


    WCHAR szResult[512] = {0};
    if (exitCode == 0) {
        StringCchPrintfW(szResult, 512, L"\r\n烧录完成，退出码：%d\r\n 烧录成功！\r\n", exitCode);
        SetWindowTextW(g_hLabelStatus, L"状态：烧录成功，等待文件更新...");
    }
    else {
        StringCchPrintfW(szResult, 512, L"\r\n烧录完成，退出码：%d\r\n 烧录失败！\r\n", exitCode);
        SetWindowTextW(g_hLabelStatus, L"状态：烧录失败，等待文件更新...");
    }

    AppendLog(szResult);
    
    SaveConfig();

    CloseHandle(hReadOut);
    CloseHandle(hReadErr);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    g_bIsFlashing = FALSE;
}


void RunOpenOCDComdSync(std::wstring szCmdLine) {
    if (g_bIsFlashing) {
        AppendLog(L"正在烧录中，请等待完成！\r\n");
        return;
    }

    EnableWindow(GetDlgItem(g_hWnd, IDC_BTN_STOP), TRUE);

    g_bIsFlashing = TRUE;
    SetWindowTextW(g_hLabelStatus, L"状态：正在执行中...");

    AppendLog(L"执行文件: \r\n");
    AppendLog(g_openocdPath.c_str());
    AppendLog(L"\r\n");
    AppendLog(L"执行命令: \r\n");
    AppendLog(szCmdLine.c_str());
    AppendLog(L"\r\n");

    // 工作目录（烧录文件所在目录）
    WCHAR szWorkDir[MAX_PATH];
    StringCchCopyW(szWorkDir, MAX_PATH, g_szCurrentFile.c_str());
    PathRemoveFileSpecW(szWorkDir);

    // 管道
    SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), NULL, TRUE };
    HANDLE hReadOut, hWriteOut, hReadErr, hWriteErr;
    CreatePipe(&hReadOut, &hWriteOut, &sa, 0);
    CreatePipe(&hReadErr, &hWriteErr, &sa, 0);

    STARTUPINFOW si = { sizeof(STARTUPINFOW) };
    PROCESS_INFORMATION pi = { 0 };
    si.hStdOutput = hWriteOut;
    si.hStdError = hWriteErr;
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    BOOL bSuccess = CreateProcessW(g_openocdPath.c_str(), (wchar_t*)szCmdLine.c_str(), NULL, NULL, TRUE,
        0, NULL, szWorkDir, &si, &pi);
    CloseHandle(hWriteOut);
    CloseHandle(hWriteErr);

    AppendLog(L"工作目录: ");
    AppendLog(szWorkDir);
    AppendLog(L"\r\n");

    SYSTEMTIME st{};
    GetLocalTime(&st);

    WCHAR timeBuf[32]{};
    StringCchPrintfW(timeBuf, _countof(timeBuf),
        L"%04d-%02d-%02d %02d:%02d:%02d",
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond);

    std::wstring* pLog = new std::wstring(
        L"------------------------------\r\n"
        L"[" + std::wstring(timeBuf) + L"] 开始执行\r\n"
        L"------------------------------\r\n"
    );

    AppendLog(pLog->c_str());

    if (!bSuccess) {
        WCHAR szErr[512];
        StringCchPrintfW(szErr, 512, L" OpenOCD启动失败，错误码：%d\r\n", GetLastError());
        AppendLog(szErr);
        SetWindowTextW(g_hLabelStatus, L"状态：启动失败");
        CloseHandle(hReadOut);
        CloseHandle(hReadErr);
        if (pi.hProcess) CloseHandle(pi.hProcess);
        if (pi.hThread) CloseHandle(pi.hThread);
        g_bIsFlashing = FALSE;
        return;
    }

    g_hCurrentProcess = pi.hProcess; // 新增：保存进程句柄

    // 读取输出
    char buf[4096] = { 0 };
    DWORD readBytes;

    while (ReadFile(hReadOut, buf, sizeof(buf) - 1, &readBytes, NULL) && readBytes > 0) {
        buf[readBytes] = 0;
        WCHAR wide[4096] = { 0 };
        MultiByteToWideChar(CP_UTF8, 0, buf, readBytes, wide, 4096);
        AppendLog(wide);
    }

    while (ReadFile(hReadErr, buf, sizeof(buf) - 1, &readBytes, NULL) && readBytes > 0) {
        buf[readBytes] = 0;
        WCHAR wide[4096] = { 0 };
        MultiByteToWideChar(CP_UTF8, 0, buf, readBytes, wide, 4096);
        AppendLog(wide);
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    g_hCurrentProcess = NULL; // 新增：进程已结束，清空全局句柄


    WCHAR szResult[512] = { 0 };
    if (exitCode == 0) {
        StringCchPrintfW(szResult, 512, L"\r\n执行命令完成，退出码：%d\r\n 执行成功！\r\n", exitCode);
        SetWindowTextW(g_hLabelStatus, L"状态：执行命令成功，等待文件更新...");
    }
    else {
        StringCchPrintfW(szResult, 512, L"\r\n执行命令完成，退出码：%d\r\n 执行失败！\r\n", exitCode);
        SetWindowTextW(g_hLabelStatus, L"状态：执行命令失败，等待文件更新...");
    }

    AppendLog(szResult);

    SaveConfig();
    EnableWindow(GetDlgItem(g_hWnd, IDC_BTN_STOP), FALSE);

    CloseHandle(hReadOut);
    CloseHandle(hReadErr);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    g_bIsFlashing = FALSE;
}

// ====================== 监控线程 ======================
unsigned int __stdcall WatchHexFileThread(void* pParam) {
    Sleep(500);
    WCHAR szMonitorFile[MAX_PATH];
    StringCchCopyW(szMonitorFile, MAX_PATH, g_szCurrentFile.c_str());

    HANDLE hFile = CreateFileW(szMonitorFile, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
        GetFileTime(hFile, NULL, NULL, &g_prevFileTime);
        CloseHandle(hFile);
    }

    DWORD debounceTimer = 0;
    BOOL needFlash = FALSE;

    while (g_hWnd != NULL) {
        if (g_bIsAutoFlashDownload) {

        Sleep(WATCH_INTERVAL);
        if (_wcsicmp(szMonitorFile, g_szCurrentFile.c_str()) != 0) {
            StringCchCopyW(szMonitorFile, MAX_PATH, g_szCurrentFile.c_str());
            hFile = CreateFileW(szMonitorFile, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            if (hFile != INVALID_HANDLE_VALUE) {
                GetFileTime(hFile, NULL, NULL, &g_prevFileTime);
                CloseHandle(hFile);
            }
            continue;
        }

        hFile = CreateFileW(szMonitorFile, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile == INVALID_HANDLE_VALUE) continue;
        FILETIME currTime;
        GetFileTime(hFile, NULL, NULL, &currTime);
        CloseHandle(hFile);

        if (CompareFileTime(&currTime, &g_prevFileTime) != 0) {
            g_prevFileTime = currTime;
            needFlash = TRUE;
            debounceTimer = GetTickCount();
            AppendLog(L"\n 文件已更新，3秒后自动烧录...\r\n");
            SetWindowTextW(g_hLabelStatus, L"状态：文件已修改，等待防抖");
        }

        if (needFlash && (GetTickCount() - debounceTimer) >= DEBOUNCE_DELAY) {
            needFlash = FALSE;
            RunOpenOCDFlash();
        }
        }
    }
    return 0;
}

// 将显示文本转换为数值（kHz）
DWORD GetSpeedValueFromText(const WCHAR* szText) {
    WCHAR szNum[32] = { 0 };
    DWORD i = 0;

    // 提取数字部分
    while (szText[i] >= L'0' && szText[i] <= L'9') {
        szNum[i] = szText[i];
        i++;
    }

    DWORD value = _wtoi(szNum);

    // 检查单位
    if (wcsstr(szText, L"MHz")) {
        value *= 1000;  // MHz 转 kHz
        return value;
    }

    // 检查单位
    if (wcsstr(szText, L"00kHz")) {
        value *= 10;  // MHz 转 kHz
        return value;
    }

    return value * 100;
}

// 根据数值获取下拉框索引
int GetSpeedIndexFromValue(DWORD speedKHz) {
    // 预定义显示文本和对应数值的映射表
    static const struct {
        DWORD value;
        const wchar_t* text;
    } speedMap[] = {
        { 1, L"1kHz" },
        { 5, L"5kHz" },
        { 10, L"10kHz" },
        { 20, L"20kHz" },
        { 50, L"50kHz" },
        { 100, L"100kHz" },
        { 200, L"200kHz" },
        { 500, L"500kHz" },
        { 1000, L"1MHz" },
        { 5000, L"5MHz" },
        { 10000, L"10MHz" }
    };

    for (int i = 0; i < _countof(speedMap); i++) {
        if (speedMap[i].value == speedKHz) {
            return i;
        }
    }

    // 如果找不到精确匹配，找最接近的
    for (int i = 0; i < _countof(speedMap); i++) {
        if (speedMap[i].value >= speedKHz) {
            return i;
        }
    }

    return 10;  // 默认返回 10MHz（最后一个）
}

// ====================== 窗口过程 ======================
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wParam;
        SetBkMode(hdc, TRANSPARENT);
        return (LRESULT)GetStockObject(WHITE_BRUSH);
    }
    case WM_CREATE: {
        CreateWindowExW(0, L"STATIC", L"烧录文件路径：", WS_VISIBLE | WS_CHILD, 10, 10, 110, 24, hWnd, (HMENU)IDC_STATIC_SPEED, NULL, NULL);
        g_hEditFilePath = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_VISIBLE | WS_CHILD | ES_AUTOHSCROLL, 120, 10, 570, 24, hWnd, (HMENU)IDC_EDIT_FILEPATH, NULL, NULL);
        SendMessageW(g_hEditFilePath, WM_SETFONT, (WPARAM)g_hFontBold, TRUE);
        CreateWindowExW(0, L"BUTTON", L"浏览", WS_VISIBLE | WS_CHILD | BS_FLAT , 710, 10, 67, 26, hWnd, (HMENU)IDC_BTN_BROWSE, NULL, NULL);

        CreateWindowExW(0, L"STATIC", L"下载速率(kHz)：", WS_VISIBLE | WS_CHILD, 10, 43, 120, 24, hWnd, NULL, NULL, NULL);
        g_hComboSpeed = CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", L"",
            WS_VISIBLE | WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL,
            120, 40, 160, 200,  // 高度改为 200，足够显示更多选项
            hWnd, (HMENU)IDC_COMBO_SPEED, NULL, NULL);        SendMessageW(g_hComboSpeed, CB_ADDSTRING, 0, (LPARAM)L"1kHz");
        SendMessageW(g_hComboSpeed, CB_ADDSTRING, 0, (LPARAM)L"5kHz");
        SendMessageW(g_hComboSpeed, CB_ADDSTRING, 0, (LPARAM)L"10kHz");
        SendMessageW(g_hComboSpeed, CB_ADDSTRING, 0, (LPARAM)L"20kHz");
        SendMessageW(g_hComboSpeed, CB_ADDSTRING, 0, (LPARAM)L"50kHz");
        SendMessageW(g_hComboSpeed, CB_ADDSTRING, 0, (LPARAM)L"100kHz");
        SendMessageW(g_hComboSpeed, CB_ADDSTRING, 0, (LPARAM)L"200kHz");
        SendMessageW(g_hComboSpeed, CB_ADDSTRING, 0, (LPARAM)L"500kHz");
        SendMessageW(g_hComboSpeed, CB_ADDSTRING, 0, (LPARAM)L"1MHz");
        SendMessageW(g_hComboSpeed, CB_ADDSTRING, 0, (LPARAM)L"5MHz");
        SendMessageW(g_hComboSpeed, CB_ADDSTRING, 0, (LPARAM)L"10MHz");
        SendMessageW(g_hComboSpeed, CB_SETCURSEL, 5, 0);
        SendMessageW(g_hComboSpeed, WM_SETFONT, (WPARAM)g_hFontBold, TRUE);

        auto setof_2 = -20;
        CreateWindowExW(0, L"STATIC", L"配置文件：", WS_VISIBLE | WS_CHILD, 310 + setof_2, 43, 120, 24, hWnd, NULL, NULL, NULL);
        g_hComboCfg = CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", L"",
            WS_VISIBLE | WS_CHILD | CBS_DROPDOWN | WS_VSCROLL,
            380 + setof_2, 40, 160, 500,  // 高度改为 200，足够显示更多选项
            hWnd, (HMENU)IDC_COMBO_CFG, NULL, NULL);
        
        if (!g_openocdConfig.szTarget.empty()) {

            SendMessageW(g_hComboCfg, CB_ADDSTRING, 0, (LPARAM)g_openocdConfig.szTarget.c_str());
        }
        else {
            SendMessageW(g_hComboCfg, CB_ADDSTRING, 0, (LPARAM)L"未找到.cfg文件");
        }

        SendMessageW(g_hComboCfg, CB_SETCURSEL, 0, 0);
        SendMessageW(g_hComboCfg, WM_SETFONT, (WPARAM)g_hFontBold, TRUE);




        g_hLabelHex = CreateWindowExW(0, L"STATIC", L"", WS_VISIBLE | WS_CHILD, 10, 70, 800, 22, hWnd, (HMENU)IDC_HEX_LABEL, NULL, NULL);
        g_hLabelStatus = CreateWindowExW(0, L"STATIC", L"状态：等待文件更新...", WS_VISIBLE | WS_CHILD, 10, 95, 400, 22, hWnd, (HMENU)IDC_STATUS_LABEL, NULL, NULL);

        CreateWindowExW(0, L"BUTTON", L"手动一键烧录", WS_VISIBLE | WS_CHILD | BS_FLAT, 660, 95, 120, 30, hWnd, (HMENU)IDC_BTN_RUN, NULL, NULL);

        CreateWindowExW(0, L"BUTTON", L"REST", WS_VISIBLE | WS_CHILD | BS_FLAT , 660 - 69 , 95, 50, 30, hWnd, (HMENU)IDC_BTN_REST, NULL, NULL);

        CreateWindowExW(0, L"BUTTON", L"BOOT", WS_VISIBLE | WS_CHILD | BS_FLAT, 660 - 69 - 65, 95, 50, 30, hWnd, (HMENU)IDC_BTN_REST_BOOT, NULL, NULL);
        
        // 停止按钮（初始禁用，执行中启用）
        CreateWindowExW(0, L"BUTTON", L"停止", WS_VISIBLE | WS_CHILD | BS_FLAT | WS_DISABLED,
            660 - 69 - 65 - 65, 95, 50, 30, hWnd, (HMENU)IDC_BTN_STOP, NULL, NULL);

        g_hEditLog = CreateWindowExW(WS_EX_CLIENTEDGE, L"RICHEDIT20W", L"", WS_VISIBLE | WS_CHILD | ES_MULTILINE | ES_READONLY | WS_VSCROLL | ES_AUTOVSCROLL | ES_NOHIDESEL, 10, 130, 780, 360, hWnd, (HMENU)IDC_LOG_EDIT, NULL, NULL);
        CHARFORMAT2W cf = { sizeof(CHARFORMAT2W) };
        cf.dwMask = CFM_BACKCOLOR;
        cf.crBackColor = RGB(255, 255, 255);
        SendMessageW(g_hEditLog, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&cf);
        SendMessageW(g_hEditLog, EM_SETEDITSTYLE, SES_EXTENDBACKCOLOR, SES_EXTENDBACKCOLOR);


        CreateWindowExW(0, L"BUTTON", L"开启更新自动烧录", WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX,
            10, 500, 160, 24, hWnd, (HMENU)IDC_CHECK_AUTO_DOWNLOAD, NULL, NULL);
        SendMessageW(GetDlgItem(hWnd, IDC_CHECK_AUTO_DOWNLOAD), BM_SETCHECK, g_bIsAutoFlashDownload ? BST_CHECKED : BST_UNCHECKED, 0);

        CreateWindowExW(0, L"BUTTON", L"烧录完成时重启开发板", WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX,
            160 + 10, 500, 160, 24, hWnd, (HMENU)IDC_CHECK_REBOOT, NULL, NULL);
        SendMessageW(GetDlgItem(hWnd, IDC_CHECK_REBOOT), BM_SETCHECK, g_openocdConfig.bReset ? BST_CHECKED : BST_UNCHECKED, 0);
        
        // 新增：上次烧录时间标签（右对齐）
        g_hLastFlashLabel = CreateWindowExW(0, L"STATIC", L"上次烧录：无", WS_VISIBLE | WS_CHILD | SS_LEFT,
            160 + 160 + 30, 500+3+1, 160, 24, hWnd, (HMENU)IDC_LAST_FLASH_LABEL, NULL, NULL);


        SetTimer(hWnd, 1, 1000, NULL); // 每秒刷新上次烧录时间

        // 全局
        HFONT g_hEditFont = NULL;

        // 先创建edit
        g_hEditFilePath = CreateWindowExW(
            WS_EX_CLIENTEDGE,
            L"EDIT",
            L"",
            WS_VISIBLE | WS_CHILD | ES_AUTOHSCROLL,
            120, 10, 570, 24,
            hWnd, (HMENU)IDC_EDIT_FILEPATH, NULL, NULL
        );

        // 创建小字体，lfHeight 负数=字符高度（推荐）
        LOGFONTW lf{};
        lf.lfHeight = -12;        // 字号，-12≈12px，越小字越小
        lf.lfWeight = FW_NORMAL;
        wcscpy_s(lf.lfFaceName, L"Segoe UI");
        g_hEditFont = CreateFontIndirectW(&lf);

        // 设置给edit，最后一个TRUE：通知控件重绘
        SendMessageW(g_hEditFilePath, WM_SETFONT, (WPARAM)g_hEditFont, TRUE);


        // 统一字体
        HWND hChild = GetWindow(hWnd, GW_CHILD);
        while (hChild) {
            SendMessageW(hChild, WM_SETFONT, (WPARAM)g_hFontBold, TRUE);
            hChild = GetWindow(hChild, GW_HWNDNEXT);
        }


        break;
    }
    case WM_DROPFILES:
        HandleDropFiles(hWnd, (HDROP)wParam);
        UpdateWindow(hWnd);
        break;
    case WM_COMMAND: {
        int id = LOWORD(wParam);
        int notify = HIWORD(wParam);
        //if (id == IDC_COMBO_SPEED && notify == CBN_SELCHANGE) {
        //    WCHAR buf[32];
        //    int sel = SendMessageW(g_hComboSpeed, CB_GETCURSEL, 0, 0);
        //    SendMessageW(g_hComboSpeed, CB_GETLBTEXT, sel, (LPARAM)buf);
        //    g_adapterSpeed = _wtoi(buf);
        //    WCHAR log[128];
        //    StringCchPrintfW(log, 128, L"下载速率已切换为：%lu kHz\r\n", g_adapterSpeed);
        //    AppendLog(log);
        //    SaveConfig();
        //    break;
        //}

        if (id == IDC_COMBO_SPEED && notify == CBN_SELCHANGE)
        {
            WCHAR buf[32] = { 0 };
            int sel = ::SendMessageW(g_hComboSpeed, CB_GETCURSEL, 0, 0);
            ::SendMessageW(g_hComboSpeed, CB_GETLBTEXT, sel, (LPARAM)buf);

            // 将显示文本转换为数值
            g_adapterSpeed = GetSpeedValueFromText(buf);

            WCHAR logStr[128] = { 0 };
            ::StringCchPrintfW(logStr, ARRAYSIZE(logStr), L"下载速率已切换为：%s (%lu kHz)\r\n", buf, g_adapterSpeed);
            AppendLog(logStr);

            ::UpdateWindow(g_hWnd);
            SaveConfig();
            break;
        }

        if (id == IDC_CHECK_REBOOT && notify == BN_CLICKED) {
            BOOL bChecked = (IsDlgButtonChecked(hWnd, IDC_CHECK_REBOOT) == BST_CHECKED);
            g_openocdConfig.bReset = bChecked;
            SaveConfig();
            break;
        }

        if (id == IDC_CHECK_AUTO_DOWNLOAD && notify == BN_CLICKED) {
            BOOL bChecked = (IsDlgButtonChecked(hWnd, IDC_CHECK_AUTO_DOWNLOAD) == BST_CHECKED);
            g_bIsAutoFlashDownload = bChecked;
            SaveConfig();
            break;
        }

        if (id == IDC_COMBO_CFG && notify == CBN_SELCHANGE)
        {
            WCHAR buf[32] = { 0 };
            int sel = ::SendMessageW(g_hComboCfg, CB_GETCURSEL, 0, 0);
            ::SendMessageW(g_hComboCfg, CB_GETLBTEXT, sel, (LPARAM)buf);

            // 将显示文本转换为数值
            g_openocdConfig.szTarget = buf;
            WCHAR logStr[128] = { 0 };
            ::StringCchPrintfW(logStr, ARRAYSIZE(logStr), L"开发板已切换为：%s \r\n", buf, g_adapterSpeed);
            AppendLog(logStr);

            ::UpdateWindow(g_hWnd);
            SaveConfig();
            break;
        }

        if (id == IDC_BTN_STOP && notify == BN_CLICKED)
        {
            StopOpenOCDProcess();
            break;
        }

        if (id == IDC_BTN_BROWSE && notify == BN_CLICKED) {
            WCHAR szFileFilter[] = L"烧录文件(*.hex)\0*.hex\0二进制(*.bin)\0*.bin\0所有文件(*.*)\0*.*\0\0";
            WCHAR szFilePath[MAX_PATH] = { 0 };
            OPENFILENAMEW ofn = { sizeof(OPENFILENAMEW) };
            ofn.hwndOwner = hWnd;
            ofn.lpstrFilter = szFileFilter;
            ofn.lpstrFile = szFilePath;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
            if (GetOpenFileNameW(&ofn)) {
                SetCurrentFile(szFilePath, true);
                SetWindowTextW(g_hEditFilePath, g_szCurrentFile.c_str());
                RefreshFileLabel();
                AppendLog(L"已手动选择烧录文件: ");
                AppendLog(szFilePath);
                AppendLog(L"\r\n");
                UpdateWindow(hWnd);
            }
            break;
        }
        if (id == IDC_BTN_RUN && notify == BN_CLICKED) {
            if (g_szCurrentFile.empty()) AppendLog(L"未选择任何烧录文件！\r\n");
            else {
                AppendLog(L"\n手动触发烧录...\r\n");
                RunOpenOCDFlash();
            }
            break;
        }

        if (id == IDC_BTN_REST && notify == BN_CLICKED) {
            SYSTEMTIME st{};
            GetLocalTime(&st);

            WCHAR timeBuf[32]{};
            StringCchPrintfW(timeBuf, _countof(timeBuf),
                L"%04d-%02d-%02d %02d:%02d:%02d",
                st.wYear, st.wMonth, st.wDay,
                st.wHour, st.wMinute, st.wSecond);

            std::wstring* pLog = new std::wstring(
                L"------------------------------\r\n"
                L"[" + std::wstring(timeBuf) + L"] 开始执行\r\n"
                L"------------------------------\r\n"
            );

            AppendLog(pLog->c_str());

            AppendLog(L"\n开始执行设备重启...\r\n");

            WCHAR szCmdLine[4096] = { 0 };
            StringCchPrintfW(szCmdLine, 4096,
                L"\"%s\" -c \"source [find interface/%ls]\" -c \"transport select %ls\" -c \"source [find target/%ls]\" -c \"adapter speed 1000\" -c init -c \"reset run\" -c shutdown",
                g_openocdPath.c_str(),
                g_openocdConfig.szInterface.c_str(),
                g_openocdConfig.szTransport.c_str(),
                g_openocdConfig.szTarget.c_str()
               );
            RunOpenOCDComdSync(szCmdLine);
            break;
        }
        if (id == IDC_BTN_REST_BOOT && notify == BN_CLICKED) {

            SYSTEMTIME st{};
            GetLocalTime(&st);

            WCHAR timeBuf[32]{};
            StringCchPrintfW(timeBuf, _countof(timeBuf),
                L"%04d-%02d-%02d %02d:%02d:%02d",
                st.wYear, st.wMonth, st.wDay,
                st.wHour, st.wMinute, st.wSecond);

            std::wstring* pLog = new std::wstring(
                L"------------------------------\r\n"
                L"[" + std::wstring(timeBuf) + L"] 开始执行\r\n"
                L"------------------------------\r\n"
            );

            AppendLog(pLog->c_str());

            AppendLog(L"\n开始执行设备重启进入复位...\r\n");

            WCHAR szCmdLine[4096] = { 0 };
            // 使用 program 命令替代一长串的 flash write_image
            StringCchPrintfW(szCmdLine, 4096,
                L"\"%s\" -c \"source [find interface/%ls]\" -c \"transport select %ls\" -c \"source [find target/%ls]\" -c \"adapter speed 1000\" -c \"program %ls verify reset\" -c shutdown",
                g_openocdPath.c_str(),
                g_openocdConfig.szInterface.c_str(),
                g_openocdConfig.szTransport.c_str(),
                g_openocdConfig.szTarget.c_str(),
                g_szCurrentFile.c_str()); // 这里是你的 .hex 文件路径
            RunOpenOCDComdSync(szCmdLine);
            break;
        }
        if (id == IDC_EDIT_FILEPATH && notify == EN_CHANGE) {
            WCHAR szTemp[MAX_PATH];
            GetWindowTextW(g_hEditFilePath, szTemp, MAX_PATH);
            SetCurrentFile(szTemp, true);
            RefreshFileLabel();
            UpdateWindow(hWnd);
            break;
        }
        break;
    }
    case WM_APPEND_LOG: {
        std::wstring* pStr = reinterpret_cast<std::wstring*>(lParam);
        if (pStr) {
            AppendLog(pStr->c_str());
            delete pStr;
        }
        return 0;
    }
    case WM_SET_STATUS: {
        std::wstring* pStr = reinterpret_cast<std::wstring*>(lParam);
        if (pStr) {
            SetWindowTextW(g_hLabelStatus, pStr->c_str());
            delete pStr;
        }
        return 0;
    }
    case WM_FLASH_DONE: {
        g_bIsFlashing = FALSE;
        g_hCurrentProcess = NULL;
        EnableWindow(GetDlgItem(g_hWnd, IDC_BTN_STOP), FALSE);
        if (wParam == 0) // 烧录成功
        {
            g_lastFlashTick = GetTickCount();
            UpdateLastFlashLabel();
        }
        return 0;
    }
    case WM_TIMER:
    {
        UpdateLastFlashLabel();
        break;
    }
    case WM_CLOSE:
        SaveConfig();
        DestroyWindow(hWnd);
        break;
    case WM_DESTROY:
        KillTimer(hWnd, 1);
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
    return 0;
}

// 在需要的地方调用
bool RefreshConfigComboBox() {

    std::wstring targetDir;
    std::wstring searchPath;

    std::vector<std::wstring> cfgFiles = EnumTargetCfgFiles(g_openocdPath, targetDir, searchPath);


    AppendLog(L"环境信息:   \r\n");
    AppendLog(L"Arduino IDE 请选择 导出已编译的二进制文件\r\n");
    AppendLog(L"-------------------------------------------\r\n");
    AppendLog(L"openocd.exe位置:   \r\n");
    AppendLog(g_openocdPath.c_str());
    AppendLog(L"     [ ");
    DWORD attrs = ::GetFileAttributesW(g_openocdPath.c_str());
    bool isOpenocd = (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY));
    AppendLog(isOpenocd ? L"有效" : L"无效");
    AppendLog(L" ] \r\n");

    if (!isOpenocd) {
        AppendLog(L"..................................\r\n");
        AppendLog(L"openocd.exe位置可能不存在 \r\n");
        AppendLog(L"处理方法如下(任选其一) \r\n");
        AppendLog(L"1. 在系统环境中的path配置openocd.exe路径\r\n");
        AppendLog(L"2. 将openocd文件夹重命名为'openocd'并放在本软件根目录下\r\n");
        AppendLog(L"3. 在本软件根目录下config.json 的\"OpenOCDPath\"中填写openocd.exe位置\r\n");
        AppendLog(L"..................................\r\n");
    }

    AppendLog(L"开发板cfg文件检索:   \r\n");
    AppendLog(searchPath.c_str());
    AppendLog(L"\r\n");

    // 清空并重新填充
    SendMessageW(g_hComboCfg, CB_RESETCONTENT, 0, 0);

    if (cfgFiles.empty()) {
        SendMessageW(g_hComboCfg, CB_ADDSTRING, 0, (LPARAM)L"未找到.cfg文件");
        SendMessageW(g_hComboCfg, CB_SETCURSEL, 0, 0);
        AppendLog(L"但是未找到");
        return false;
    }
    else {
        int defaultIdx = 0;
        for (size_t i = 0; i < cfgFiles.size(); i++) {
            SendMessageW(g_hComboCfg, CB_ADDSTRING, 0, (LPARAM)cfgFiles[i].c_str());
            if (cfgFiles[i] == g_openocdConfig.szTarget) {
                defaultIdx = (int)i;
            }
        }

        SendMessageW(g_hComboCfg, CB_SETCURSEL, defaultIdx, 0);
        
        AppendLog((std::wstring(L"找到 : ") + std::to_wstring(cfgFiles.size()) + std::wstring(L" 个开发板配置文件")).c_str());

        AppendLog(L"\r\n");
        AppendLog(L"\r\n");
        AppendLog(L"-------------------------------------------\r\n");
        return true;
    }

    AppendLog(L"\r\n");
    AppendLog(L"-------------------------------------------\r\n");

}


// ====================== 程序入口 ======================
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrev, LPSTR lpCmdLine, int nCmdShow) {
    LoadLibraryW(L"riched20.dll");
    // 初始化配置结构
    InitOpenOCDConfig();

    // 静默烧录模式
    const wchar_t* cmd = GetCommandLineW();
    if (wcsstr(cmd, L"-run") != NULL) {
        LoadConfig();
        if (g_szCurrentFile.empty())
            FindFirstHexFile(g_szCurrentFile, L"%s\\*.hex", false);
        if (!g_szCurrentFile.empty())
            RunOpenOCDFlash();
        else
            MessageBoxW(NULL, L"未找到烧录文件", L"错误", MB_ICONERROR);
        return 0;
    }

    // 创建字体
    g_hFontBold = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, GB2312_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"SimHei");

    // 注册窗口类
    WNDCLASSEXW wc = { sizeof(WNDCLASSEXW) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = WND_CLASS_NAME;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(1));      // 大图标
    wc.hIconSm = LoadIcon(hInstance, MAKEINTRESOURCE(1));    // 小图标

    RegisterClassExW(&wc);

    DWORD dwExStyle = WS_EX_ACCEPTFILES;
    DWORD dwStyle = WS_OVERLAPPED | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT rcClient = { 0,0,815,575 };
    AdjustWindowRectEx(&rcClient, dwStyle, FALSE, dwExStyle);
    g_hWnd = CreateWindowExW(dwExStyle, WND_CLASS_NAME, L"STM32 DAP Link (OpenOCD) 自动烧录工具", dwStyle,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rcClient.right - rcClient.left, rcClient.bottom - rcClient.top,
        NULL, NULL, hInstance, NULL);
    if (!g_hWnd) return 1;



    ShowWindow(g_hWnd, nCmdShow);
    UpdateWindow(g_hWnd);

    // 加载配置并更新界面
    LoadConfig();
    if (g_szCurrentFile.empty())
        FindFirstHexFile(g_szCurrentFile, L"%s\\*.hex", false);
    if (g_hEditFilePath)
        SetWindowTextW(g_hEditFilePath, g_szCurrentFile.c_str());
    RefreshFileLabel();

    // 同步下拉框速率
    if (g_hComboSpeed) {
        //int idx = 5;
        //switch (g_adapterSpeed) {
        //case 1000: idx = 0; break;
        //case 5000: idx = 1; break;
        //case 10000: idx = 2; break;
        //case 20000: idx = 3; break;
        //case 50000: idx = 4; break;
        //default: idx = 5;
        //}
        //SendMessageW(g_hComboSpeed, CB_SETCURSEL, idx, 0);

        int idx = GetSpeedIndexFromValue(g_adapterSpeed);
        ::SendMessageW(g_hComboSpeed, CB_SETCURSEL, idx, 0);
    }

    // 启动监控线程
    unsigned int tid;
    g_hWatchThread = (HANDLE)_beginthreadex(NULL, 0, WatchHexFileThread, NULL, 0, &tid);

	std::thread([]() {
		while (g_hWnd == NULL) Sleep(100);
        Sleep(1500);
        RefreshConfigComboBox();

		}).detach();

    // 消息循环
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (g_hWatchThread) CloseHandle(g_hWatchThread);
    if (g_hFontBold) DeleteObject(g_hFontBold);
    return (int)msg.wParam;
}


