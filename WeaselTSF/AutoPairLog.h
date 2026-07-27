#pragma once
// 调试日志 - 始终输出到 OutputDebugString + 文件
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
  DWORD n = GetEnvironmentVariableW(L"APPDATA", path, MAX_PATH);
  if (n > 0 && n < MAX_PATH) {
    return std::wstring(path) + L"\\Rime";
  }
  return L"";
}

inline void Log(int level, const std::string& msg) {
  // ALWAYS output to debugger (DebugView)
  OutputDebugStringA(("[autopair] " + msg + "\n").c_str());

  // ALWAYS output to file (no level check)
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
  f.flush();
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
