#pragma once
// auto_pair temporary debug log (delete this file when done debugging)
// Writes to OutputDebugString (DebugView) and
// %TEMP%\rime.weasel\auto_pair.log
#include <windows.h>
#include <cstdio>
#include <fstream>
#include <string>

#define AUTOPAIR_VERSION "V06"
#define AUTOPAIR_VERSION_W L"V06"

namespace autopair {

inline std::wstring GetLogPath() {
  wchar_t buf[MAX_PATH] = {0};
  if (ExpandEnvironmentStringsW(L"%TEMP%\\rime.weasel", buf, MAX_PATH) == 0)
    return L"";
  return std::wstring(buf) + L"\\auto_pair.log";
}

inline void Log(const std::string& msg) {
  std::string line = "[" AUTOPAIR_VERSION "] " + msg;

  // 1. OutputDebugString for DebugView
  OutputDebugStringA((line + "\n").c_str());

  // 2. File log
  std::wstring path = GetLogPath();
  if (path.empty())
    return;
  std::ofstream f(path, std::ios::app);
  if (!f)
    return;
  SYSTEMTIME st;
  GetLocalTime(&st);
  char ts[64];
  sprintf_s(ts, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond,
            st.wMilliseconds);
  f << ts << line << "\n";
  f.flush();
}

}  // namespace autopair

#define APLOG(msg) autopair::Log(msg)
