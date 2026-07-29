#pragma once
// [auto_pair] 临时诊断日志（定位完成后连同此文件一起删除）
//
// 输出到 OutputDebugString（DebugView）和 %TEMP%\rime.weasel\auto_pair.log。
//
// 每行都带进程名和 PID：这个日志文件被所有加载了输入法的进程共享写入，而未重启
// 的进程仍在跑旧版 DLL，上一轮就因为分不清来源误判过"DLL 没更新"。
#include <windows.h>
#include <msctf.h>
#include <cstdio>
#include <fstream>
#include <string>

#define AUTOPAIR_VERSION "V11"

namespace autopair {

inline std::string ProcessTag() {
  static std::string tag;
  if (!tag.empty())
    return tag;
  char path[MAX_PATH] = {0};
  const char* name = "?";
  if (GetModuleFileNameA(NULL, path, MAX_PATH) > 0) {
    const char* slash = strrchr(path, '\\');
    name = slash ? slash + 1 : path;
  }
  char buf[MAX_PATH + 32];
  sprintf_s(buf, "%s:%lu", name, GetCurrentProcessId());
  tag = buf;
  return tag;
}

inline std::wstring GetLogPath() {
  wchar_t buf[MAX_PATH] = {0};
  if (ExpandEnvironmentStringsW(L"%TEMP%\\rime.weasel", buf, MAX_PATH) == 0)
    return L"";
  return std::wstring(buf) + L"\\auto_pair.log";
}

inline void Log(const std::string& msg) {
  std::string line = "[" AUTOPAIR_VERSION "][" + ProcessTag() + "] " + msg;

  OutputDebugStringA((line + "\n").c_str());

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
