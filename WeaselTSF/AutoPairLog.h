#pragma once
// auto_pair 临时调试日志（调试完成后删除此文件）
// 输出到 OutputDebugString（DebugView）和 %TEMP%\rime.weasel\auto_pair.log
#include <windows.h>
#include <msctf.h>
#include <cstdio>
#include <fstream>
#include <string>

#define AUTOPAIR_VERSION "V04"

namespace autopair {

inline std::wstring GetLogPath() {
  wchar_t buf[MAX_PATH] = {0};
  if (ExpandEnvironmentStringsW(L"%TEMP%\\rime.weasel", buf, MAX_PATH) == 0)
    return L"";
  return std::wstring(buf) + L"\\auto_pair.log";
}

inline void Log(const std::string& msg) {
  std::string line = "[" AUTOPAIR_VERSION "][DLL] " + msg;

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

// 诊断用：返回光标相对文档起点的字符偏移，失败返回 -1
inline int GetCursorOffset(ITfContext* pContext, TfEditCookie ec) {
  if (!pContext)
    return -1;
  TF_SELECTION sel;
  ULONG fetched = 0;
  if (FAILED(pContext->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &sel,
                                    &fetched)) ||
      fetched == 0)
    return -1;
  ITfRange* pSel = sel.range;

  ITfRange* pStart = nullptr;
  int offset = -1;
  if (SUCCEEDED(pContext->GetStart(ec, &pStart)) && pStart) {
    ITfRange* pMeasure = nullptr;
    if (SUCCEEDED(pStart->Clone(&pMeasure)) && pMeasure) {
      // 把 measure range 的终点移到光标处，再读文本长度
      if (SUCCEEDED(pMeasure->ShiftEndToRange(ec, pSel, TF_ANCHOR_START))) {
        offset = 0;
        WCHAR buf[256];
        // 每读一段就把起点前移，否则 range 不推进会死循环
        for (int guard = 0; guard < 4096; ++guard) {
          ULONG got = 0;
          if (FAILED(pMeasure->GetText(ec, 0, buf, 256, &got)) || got == 0)
            break;
          offset += (int)got;
          LONG shifted = 0;
          if (FAILED(pMeasure->ShiftStart(ec, (LONG)got, &shifted, NULL)) ||
              shifted == 0)
            break;
        }
      }
      pMeasure->Release();
    }
    pStart->Release();
  }
  pSel->Release();
  return offset;
}

}  // namespace autopair

#define APLOG(msg) autopair::Log(msg)
