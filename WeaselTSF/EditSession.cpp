#include "stdafx.h"
#include "CandidateList.h"
#include "ResponseParser.h"
#include "WeaselTSF.h"
#include "AutoPairLog.h"

#include <cstdio>
#include <fstream>
#include <string>

// Use APLOG from AutoPairLog.h; APLog/APDump as local aliases
namespace {
inline void APLog(int level, const std::string& msg) {
  APLOG(level, msg);
}
inline std::string APDump(const std::wstring& s) {
  return autopair_log::DumpCodepoints(s);
}
}  // namespace

STDAPI WeaselTSF::DoEditSession(TfEditCookie ec) {
  // Version marker - outputs once on first call
  static bool _marker_logged = false;
  if (!_marker_logged) {
    _marker_logged = true;
    APLOG(1, "[BUILD_MARKER] auto_pair_cursor_back 2027-1950");
  }

  // get commit string from server
  std::wstring commit;
  weasel::Config config;
  auto context = std::make_shared<weasel::Context>();
  weasel::ResponseParser parser(&commit, context.get(), &_status, &config,
                                &_cand->style());

  bool ok = m_client.GetResponseData(std::ref(parser));

  APLOG(1,
        std::string("[DoEditSession] enter: ok=") + std::to_string((int)ok) +
            " commit_empty=" + std::to_string((int)commit.empty()) +
            " cursor_back_count=" + std::to_string(config.cursor_back_count));

  _UpdateLanguageBar(_status);

  if (ok) {
    if (!commit.empty()) {
      APLOG(1, std::string("[DoEditSession] commit path: len=") +
                   std::to_string(commit.length()));
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

  // auto_pair: move cursor back after commit_text (which bypasses TSF)
  if (config.cursor_back_count > 0) {
    APLOG(1, std::string("[DoEditSession] SendInput VK_LEFT x") +
                 std::to_string(config.cursor_back_count));
    INPUT inputs[2] = {};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = VK_LEFT;
    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = VK_LEFT;
    inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
    for (int i = 0; i < config.cursor_back_count; i++) {
      SendInput(2, inputs, sizeof(INPUT));
    }
  } else {
    APLOG(2, std::string("[DoEditSession] no cursor_back (count=0)"));
  }

  return TRUE;
}
