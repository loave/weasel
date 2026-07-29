#include "stdafx.h"
#include "WeaselIPC.h"
#include "WeaselTSF.h"
#include <KeyEvent.h>
#include "CandidateList.h"
#include "AutoPairLog.h"

static weasel::KeyEvent prevKeyEvent;
static BOOL prevfEaten = FALSE;
static int keyCountToSimulate = 0;

namespace {

/* [auto_pair] One line per sink call: which handler, the key, whether Windows
 * marked it as an auto-repeat (lParam bit 30) and with what repeat count, and
 * the two pairing flags as they were on entry. Temporary; goes away with
 * AutoPairLog.h. */
std::string SinkLine(const char* sink,
                     WPARAM wParam,
                     LPARAM lParam,
                     BOOL downPending,
                     BOOL upPending) {
  char buf[192];
  sprintf_s(buf, "%s vk=0x%02X repeat=%d count=%u downPending=%d upPending=%d",
            sink, (UINT)wParam, (lParam & (1L << 30)) ? 1 : 0,
            (UINT)(lParam & 0xFFFF), downPending ? 1 : 0, upPending ? 1 : 0);
  return buf;
}

}  // namespace

/* [auto_pair] Is this one of the keys we synthesized in _SendCursorBackKeys?
 *
 * Matched by key code within a short time window rather than by an exact
 * count: a single physical key reaches the sink twice (OnTestKeyDown then
 * OnKeyDown) whenever pfEaten stays FALSE, and the multiplier varies by
 * application, so a counter cannot be balanced.
 *
 * VK_LEFT only, which is now the only thing ever injected: the arrow keys wait
 * for the user to release Shift rather than injecting a release themselves, so
 * every VK_SHIFT reaching this sink is genuinely the user's and must be passed
 * on to rime untouched. See _SendCursorBackKeys.
 *
 * Swallowing VK_LEFT has no observable cost. pfEaten stays FALSE either way,
 * so if the user really does press Left inside the window the application
 * still moves the caret; only rime skips the key, and no composition is in
 * flight at this point.
 */
BOOL WeaselTSF::_IsAutoPairSynthKey(UINT vk) {
  if (_apSynthUntil == 0)
    return FALSE;
  if (GetTickCount64() > _apSynthUntil) {
    _apSynthUntil = 0;
    return FALSE;
  }
  return vk == VK_LEFT;
}

/* [auto_pair] Leave the sink immediately for the keys we synthesized: the app
 * still acts on them, rime never sees them, and nothing else in the sink is
 * touched.
 *
 * That last part is the point, and it has to happen in the four sink entry
 * points rather than inside _ProcessKeyEvent. _fTestKeyDownPending pairs a
 * TestKeyDown with the KeyDown that follows it; an injected key landing in
 * between steals the pairing. OnTestKeyDown sees the flag still set and eats
 * our arrow key, so the caret never moves, and OnKeyDown then clears the flag,
 * after which the real key's own OnKeyDown no longer finds it pending and gets
 * processed a second time -- committing a second pair of symbols. A single [
 * in Firefox came out as three pairs, nested, that way. Raising
 * style/cursor_back_delay_ms only made the injection miss the window instead
 * of being transparent to it. */
BOOL WeaselTSF::_SkipAutoPairSynthKey(const char* sink,
                                      WPARAM wParam,
                                      BOOL* pfEaten) {
  if (!_IsAutoPairSynthKey(static_cast<UINT>(wParam)))
    return FALSE;
  *pfEaten = FALSE;
  char buf[96];
  sprintf_s(buf, "%s: synthesized key passed through untouched", sink);
  APLOG(buf);
  return TRUE;
}

void WeaselTSF::_ProcessKeyEvent(WPARAM wParam, LPARAM lParam, BOOL* pfEaten) {
  // when _IsKeyboardDisabled don't eat the key,
  // when keyboard closable and keyboard closed, don't eat the key
  if ((_isToOpenClose && !_IsKeyboardOpen()) || _IsKeyboardDisabled()) {
    *pfEaten = FALSE;
    return;
  }

  // if server connection is Not OK, don't eat it.
  if (!_EnsureServerConnected()) {
    *pfEaten = FALSE;
    return;
  }
  weasel::KeyEvent ke;
  GetKeyboardState(_lpbKeyState);
  if (!ConvertKeyEvent(static_cast<UINT>(wParam), lParam, _lpbKeyState, ke)) {
    /* Unknown key event */
    *pfEaten = FALSE;
  } else {
    // cheet key code when vertical auto reverse happened, swap up and down
    if (_cand->GetIsReposition()) {
      if (ke.keycode == ibus::Up)
        ke.keycode = ibus::Down;
      else if (ke.keycode == ibus::Down)
        ke.keycode = ibus::Up;
    }
    if (!keyCountToSimulate)
      *pfEaten = (BOOL)m_client.ProcessKeyEvent(ke);

    if (ke.keycode == ibus::Caps_Lock) {
      if (prevKeyEvent.keycode == ibus::Caps_Lock && prevfEaten == TRUE &&
          (ke.mask & ibus::RELEASE_MASK) && (!keyCountToSimulate)) {
        if ((GetKeyState(VK_CAPITAL) & 0x01)) {
          if (_committed || (!*pfEaten && _status.composing)) {
            keyCountToSimulate = 2;
            INPUT inputs[2];
            inputs[0].type = INPUT_KEYBOARD;
            inputs[0].ki = {VK_CAPITAL, 0, 0, 0, 0};
            inputs[1].type = INPUT_KEYBOARD;
            inputs[1].ki = {VK_CAPITAL, 0, KEYEVENTF_KEYUP, 0, 0};
            ::SendInput(sizeof(inputs) / sizeof(INPUT), inputs, sizeof(INPUT));
          }
        }
        *pfEaten = TRUE;
      }
      if (keyCountToSimulate)
        keyCountToSimulate--;
    }

    prevfEaten = *pfEaten;
    prevKeyEvent = ke;
  }
}

STDAPI WeaselTSF::OnSetFocus(BOOL fForeground) {
  if (fForeground)
    m_client.FocusIn();
  else {
    m_client.FocusOut();
    _AbortComposition();
  }

  return S_OK;
}

/* Some apps sends strange OnTestKeyDown/OnKeyDown combinations:
 *  Some sends OnKeyDown() only. (QQ2012)
 *  Some sends multiple OnTestKeyDown() for a single key event. (MS WORD 2010
 * x64)
 *
 * We assume every key event will eventually cause a OnKeyDown() call.
 * We use _fTestKeyDownPending to omit multiple OnTestKeyDown() calls,
 *  and for OnKeyDown() to check if the key has already been sent to the server.
 */

STDAPI WeaselTSF::OnTestKeyDown(ITfContext* pContext,
                                WPARAM wParam,
                                LPARAM lParam,
                                BOOL* pfEaten) {
  APLOG(SinkLine("OnTestKeyDown", wParam, lParam, _fTestKeyDownPending,
                 _fTestKeyUpPending));
  if (_SkipAutoPairSynthKey("OnTestKeyDown", wParam, pfEaten))
    return S_OK;
  _fTestKeyUpPending = FALSE;
  if (_fTestKeyDownPending) {
    *pfEaten = TRUE;
    return S_OK;
  }
  _ProcessKeyEvent(wParam, lParam, pfEaten);
  _UpdateComposition(pContext);
  if (*pfEaten)
    _fTestKeyDownPending = TRUE;
  return S_OK;
}

STDAPI WeaselTSF::OnKeyDown(ITfContext* pContext,
                            WPARAM wParam,
                            LPARAM lParam,
                            BOOL* pfEaten) {
  APLOG(SinkLine("OnKeyDown", wParam, lParam, _fTestKeyDownPending,
                 _fTestKeyUpPending));
  if (_SkipAutoPairSynthKey("OnKeyDown", wParam, pfEaten))
    return S_OK;
  _fTestKeyUpPending = FALSE;
  if (_fTestKeyDownPending) {
    _fTestKeyDownPending = FALSE;
    *pfEaten = TRUE;
  } else {
    _ProcessKeyEvent(wParam, lParam, pfEaten);
    _UpdateComposition(pContext);
  }
  return S_OK;
}

STDAPI WeaselTSF::OnTestKeyUp(ITfContext* pContext,
                              WPARAM wParam,
                              LPARAM lParam,
                              BOOL* pfEaten) {
  APLOG(SinkLine("OnTestKeyUp", wParam, lParam, _fTestKeyDownPending,
                 _fTestKeyUpPending));
  if (_SkipAutoPairSynthKey("OnTestKeyUp", wParam, pfEaten))
    return S_OK;
  _fTestKeyDownPending = FALSE;
  if (_fTestKeyUpPending) {
    *pfEaten = TRUE;
    return S_OK;
  }
  _ProcessKeyEvent(wParam, lParam, pfEaten);
  _UpdateComposition(pContext);
  if (*pfEaten)
    _fTestKeyUpPending = TRUE;
  return S_OK;
}

STDAPI WeaselTSF::OnKeyUp(ITfContext* pContext,
                          WPARAM wParam,
                          LPARAM lParam,
                          BOOL* pfEaten) {
  APLOG(SinkLine("OnKeyUp", wParam, lParam, _fTestKeyDownPending,
                 _fTestKeyUpPending));
  if (_SkipAutoPairSynthKey("OnKeyUp", wParam, pfEaten))
    return S_OK;
  _fTestKeyDownPending = FALSE;
  if (_fTestKeyUpPending) {
    _fTestKeyUpPending = FALSE;
    *pfEaten = TRUE;
  } else {
    _ProcessKeyEvent(wParam, lParam, pfEaten);
    if (!_async_edit)
      _UpdateComposition(pContext);
  }
  return S_OK;
}

STDAPI WeaselTSF::OnPreservedKey(ITfContext* pContext,
                                 REFGUID rguid,
                                 BOOL* pfEaten) {
  *pfEaten = FALSE;
  return S_OK;
}

BOOL WeaselTSF::_InitKeyEventSink() {
  com_ptr<ITfKeystrokeMgr> pKeystrokeMgr;
  HRESULT hr;

  if (_pThreadMgr->QueryInterface(&pKeystrokeMgr) != S_OK)
    return FALSE;

  hr = pKeystrokeMgr->AdviseKeyEventSink(_tfClientId, (ITfKeyEventSink*)this,
                                         TRUE);

  return (hr == S_OK);
}

void WeaselTSF::_UninitKeyEventSink() {
  com_ptr<ITfKeystrokeMgr> pKeystrokeMgr;

  if (_pThreadMgr->QueryInterface(&pKeystrokeMgr) != S_OK)
    return;

  pKeystrokeMgr->UnadviseKeyEventSink(_tfClientId);
}

BOOL WeaselTSF::_InitPreservedKey() {
  return TRUE;
#if 0
	com_ptr<ITfKeystrokeMgr> pKeystrokeMgr;
	if (_pThreadMgr->QueryInterface(pKeystrokeMgr.GetAddressOf()) != S_OK)
	{
		return FALSE;
	}
	TF_PRESERVEDKEY preservedKeyImeMode;

	/* Define SHIFT ONLY for now */
	preservedKeyImeMode.uVKey = VK_SHIFT;
	preservedKeyImeMode.uModifiers = TF_MOD_ON_KEYUP;

	auto hr = pKeystrokeMgr->PreserveKey(
		_tfClientId,
		GUID_IME_MODE_PRESERVED_KEY,
		&preservedKeyImeMode, L"", 0);
	
	return SUCCEEDED(hr);
#endif
}

void WeaselTSF::_UninitPreservedKey() {}
