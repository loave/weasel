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
      // When cursorBackCount > 0, EndComposition is done inside
      // CInsertTextEditSession to keep cursor positioning atomic.
      // Otherwise, end composition separately as before.
      if (_cursorBackCount == 0) {
        _EndComposition(_pEditSessionContext, false);
      } else {
        // InsertText already ended composition; just clean up UI
        _cand->EndUI();
        _cursorBackCount = 0;
      }
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
