#pragma once
// 轻量调试日志（独立于 IPC 配置链路）
// 级别文件: %APPDATA%\Rime\weasel_debug.level
//   0=静默 1=关键 2=详细
// 日志输出: %APPDATA%\Rime\weasel_autopair.log
// 重开应用进程生效（TSF DLL 进程内加载）
#include <windows.h>
#include <cstdio>
#include <fstream>
#include <string>

namespace autopair_log {

inline int GetLogLevel() {
  static int cached = -1;
  if (cached >= 0)
    return cached;
  cached = 0;
  wchar_t appdata[MAX_PATH];
  DWORD n = GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
  if (n == 0 || n >= MAX_PATH)
    return cached;
  std::wstring path = std::wstring(appdata) + L"\\Rime\\weasel_debug.level";
  std::ifstream f(path);
  if (f) {
    int lv = 0;
    if (f >> lv)
      cached = lv;
  }
  return cached;
}

inline void Log(int level, const std::string& msg) {
  if (level > GetLogLevel())
    return;
  wchar_t appdata[MAX_PATH];
  DWORD n = GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
  if (n == 0 || n >= MAX_PATH)
    return;
  std::wstring path = std::wstring(appdata) + L"\\Rime\\weasel_autopair.log";
  std::ofstream f(path, std::ios::app);
  if (!f)
    return;
  SYSTEMTIME st;
  GetLocalTime(&st);
  char ts[64];
  sprintf_s(ts, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond,
            st.wMilliseconds);
  f << ts << msg << "\n";
}

// 把宽字符串按码点转成 "U+XXXX U+XXXX ..." 便于诊断 U+FDD0 是否到达
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
