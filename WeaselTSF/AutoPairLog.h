#pragma once
// 轻量调试日志（独立于 IPC 配置链路）
// 级别文件: %APPDATA%\Rime\weasel_debug.level
//   0=静默 1=关键 2=详细
// 日志输出: %APPDATA%\Rime\weasel_autopair.log
// 同时输出到 OutputDebugString（可用 DebugView 查看）
// 重开应用进程生效（TSF DLL 进程内加载）
#include <windows.h>
#include <shlobj.h>
#include <cstdio>
#include <fstream>
#include <string>

namespace autopair_log {

inline std::wstring GetRimePath() {
  wchar_t path[MAX_PATH] = {0};
  if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, path))) {
    return std::wstring(path) + L"\\Rime";
  }
  // fallback to env var
  DWORD n = GetEnvironmentVariableW(L"APPDATA", path, MAX_PATH);
  if (n > 0 && n < MAX_PATH) {
    return std::wstring(path) + L"\\Rime";
  }
  return L"";
}

inline int GetLogLevel() {
  static int cached = -1;
  if (cached >= 0)
    return cached;
  cached = 0;
  std::wstring rime = GetRimePath();
  if (rime.empty())
    return cached;
  std::wstring levelFile = rime + L"\\weasel_debug.level";
  std::ifstream f(levelFile);
  if (f) {
    int lv = 0;
    if (f >> lv)
      cached = lv;
  }
  return cached;
}

inline void Log(int level, const std::string& msg) {
  // Always output to debugger (DebugView) regardless of level
  OutputDebugStringA(("[autopair] " + msg + "\n").c_str());

  if (level > GetLogLevel())
    return;
  std::wstring rime = GetRimePath();
  if (rime.empty())
    return;
  std::wstring logFile = rime + L"\\weasel_autopair.log";
  std::ofstream f(logFile, std::ios::app);
  if (!f)
    return;
  SYSTEMTIME st;
  GetLocalTime(&st);
  char ts[64];
  sprintf_s(ts, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond,
            st.wMilliseconds);
  f << ts << msg << "\n";
}

inline std::string DumpCodepoints(const std::wstring& s) {
  std::string out;
  char buf[16];
  for (wchar_t c : s) {
    sprintf_s(buf, "U+%04X ", (unsigned int)(unsigned short)c);
    out += buf;
  }
  return out;
}

}  // namespace autopair_log

#define APLOG(level, msg) autopair_log::Log((level), (msg))
