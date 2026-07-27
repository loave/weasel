#include "stdafx.h"
#include "CandidateList.h"
#include "ResponseParser.h"
#include "WeaselTSF.h"

#include <cstdio>
#include <fstream>
#include <string>

// 内联调试日志（独立于 IPC；级别文件 %APPDATA%\Rime\weasel_debug.level）
namespace {
inline int APLogLevel() {
  static int cached = -1;
  if (cached >= 0)
    return cached;
  cached = 0;
  wchar_t appdata[MAX_PATH];
  DWORD n = GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
  if (n == 0 || n >= MAX_PATH)
    return cached;
  std::wstring p = std::wstring(appdata) + L"\\Rime\\weasel_debug.level";
  std::ifstream f(p);
  if (f) {
    int lv = 0;
    if (f >> lv)
      cached = lv;
  }
  return cached;
}
inline void APLog(int level, const std::string& msg) {
  if (level > APLogLevel())
    return;
  wchar_t appdata[MAX_PATH];
  DWORD n = GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
  if (n == 0 || n >= MAX_PATH)
    return;
  std::wstring p = std::wstring(appdata) + L"\\Rime\\weasel_autopair.log";
  std::ofstream f(p, std::ios::app);
  if (!f)
    return;
  SYSTEMTIME st;
  GetLocalTime(&st);
  char ts[64];
  sprintf_s(ts, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond,
            st.wMilliseconds);
  f << ts << msg << "\n";
}
inline std::string APDump(const std::wstring& s) {
  std::string out;
  char buf[16];
  for (wchar_t c : s) {
    sprintf_s(buf, "U+%04X ", (unsigned int)(unsigned short)c);
    out += buf;
  }
  return out;
}
}  // namespace

STDAPI WeaselTSF::DoEditSession(TfEditCookie ec) {
  // get commit string from server
  std::wstring commit;
  weasel::Config config;
  auto context = std::make_shared<weasel::Context>();
  weasel::ResponseParser parser(&commit, context.get(), &_status, &config,
                                &_cand->style());

  bool ok = m_client.GetResponseData(std::ref(parser));

  _UpdateLanguageBar(_status);

  if (ok) {
    // record cursor_back_count for this commit (set by Lua auto_pair)
    _cursorBackCount = config.cursor_back_count;
    if (!commit.empty()) {
      APLog(1, std::string("[EditSession] commit len=") +
                   std::to_string(commit.length()) + " cursor_back_count=" +
                   std::to_string(config.cursor_back_count));
      APLog(2,
            std::string("[EditSession] commit codepoints: ") + APDump(commit));
      // For auto-selecting, commit and preedit can both exist.
      // Commit and close the original composition first.
      if (!_IsComposing()) {
        _StartComposition(_pEditSessionContext,
                          _fCUASWorkaroundEnabled && !config.inline_preedit);
      }
      _InsertText(_pEditSessionContext, commit);
      _EndComposition(_pEditSessionContext, false);
      _committed = TRUE;
    } else {
      _committed = FALSE;
    }
    if (_status.composing && !_IsComposing()) {
      _StartComposition(_pEditSessionContext,
                        _fCUASWorkaroundEnabled && !config.inline_preedit);
    } else if (!_status.composing && _IsComposing()) {
      _EndComposition(_pEditSessionContext, true);
    }
    if (_IsComposing() && config.inline_preedit) {
      _ShowInlinePreedit(_pEditSessionContext, context);
    }
    _UpdateCompositionWindow(_pEditSessionContext);
  }

  _UpdateUI(*context, _status);

  return TRUE;
}
