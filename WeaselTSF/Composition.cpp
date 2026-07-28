#include "stdafx.h"
#include "WeaselTSF.h"
#include "EditSession.h"
#include "ResponseParser.h"
#include "CandidateList.h"
#include "AutoPairLog.h"

/* Start Composition */
class CStartCompositionEditSession : public CEditSession {
 public:
  CStartCompositionEditSession(com_ptr<WeaselTSF> pTextService,
                               com_ptr<ITfContext> pContext,
                               BOOL fCUASWorkaroundEnabled,
                               BOOL inlinePreeditEnabled)
      : CEditSession(pTextService, pContext),
        _inlinePreeditEnabled(inlinePreeditEnabled) {
    _fCUASWorkaroundEnabled = fCUASWorkaroundEnabled;
  }

  /* ITfEditSession */
  STDMETHODIMP DoEditSession(TfEditCookie ec);

 private:
  BOOL _fCUASWorkaroundEnabled;
  BOOL _inlinePreeditEnabled;
};

STDAPI CStartCompositionEditSession::DoEditSession(TfEditCookie ec) {
  HRESULT hr = E_FAIL;
  com_ptr<ITfInsertAtSelection> pInsertAtSelection;
  com_ptr<ITfRange> pRangeComposition;
  if (_pContext->QueryInterface(IID_ITfInsertAtSelection,
                                (LPVOID*)&pInsertAtSelection) != S_OK)
    return hr;
  if (pInsertAtSelection->InsertTextAtSelection(ec, TF_IAS_QUERYONLY, NULL, 0,
                                                &pRangeComposition) != S_OK)
    return hr;

  com_ptr<ITfContextComposition> pContextComposition;
  com_ptr<ITfComposition> pComposition;
  if (_pContext->QueryInterface(IID_ITfContextComposition,
                                (LPVOID*)&pContextComposition) != S_OK)
    return hr;
  if ((pContextComposition->StartComposition(
           ec, pRangeComposition, _pTextService, &pComposition) == S_OK) &&
      (pComposition != NULL)) {
    _pTextService->_SetComposition(pComposition);

    /* WORKAROUND:
     *   CUAS does not provide a correct GetTextExt() position unless the
     * composition is filled with characters. So we insert a zero width space
     * here. The workaround is only needed when inline preedit is not enabled.
     *   See https://github.com/rime/weasel/pull/883#issuecomment-1567625762
     */
    if (!_inlinePreeditEnabled) {
      pRangeComposition->SetText(ec, TF_ST_CORRECTION, L" ", 1);
    }

    /* set selection */
    TF_SELECTION tfSelection;
    if (_inlinePreeditEnabled)
      pRangeComposition->Collapse(ec, TF_ANCHOR_END);
    else
      pRangeComposition->Collapse(ec, TF_ANCHOR_START);
    tfSelection.range = pRangeComposition;
    tfSelection.style.ase = TF_AE_NONE;
    tfSelection.style.fInterimChar = FALSE;
    _pContext->SetSelection(ec, 1, &tfSelection);
  }

  return hr;
}

void WeaselTSF::_StartComposition(com_ptr<ITfContext> pContext,
                                  BOOL fCUASWorkaroundEnabled) {
  com_ptr<CStartCompositionEditSession> pStartCompositionEditSession;
  pStartCompositionEditSession.Attach(new CStartCompositionEditSession(
      this, pContext, fCUASWorkaroundEnabled, _cand->style().inline_preedit));
  _cand->StartUI();
  if (pStartCompositionEditSession != nullptr) {
    HRESULT hr;
    pContext->RequestEditSession(_tfClientId, pStartCompositionEditSession,
                                 TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &hr);
  }
}

/* [auto_pair] Deferred cursor move, with key injection as the fallback.
 *
 * Moving the caret inside CEndCompositionEditSession works only in apps whose
 * TSF text store models the whole editable document: Firefox and RichEdit do,
 * and there the caret lands correctly. Chromium based apps expose only the
 * composition text and reset the store when the composition ends, and apps
 * going through the IMM32 compatibility layer get a throwaway document that
 * only exists during composition. In both cases SetSelection reports success
 * against a buffer that is no longer connected to the real caret.
 *
 * So a short while after the commit, from the message loop, we read the caret
 * back. If it is where we put it, the TSF route worked and we are done. If it
 * is anywhere else (a stale offset, or unreadable) the text store is useless
 * to us and we inject real VK_LEFT presses instead.
 *
 * The probe is idempotent: it only moves the caret when it sits exactly at the
 * end of the pair we just committed, so repeated attempts cannot walk the
 * caret past the target.
 */
namespace {

struct CursorBackPending {
  com_ptr<WeaselTSF> service;
  com_ptr<ITfContext> context;
  int cursorBack = 0;
  int targetOffset = -1;  // where we want the caret
  int endOffset = -1;     // end of the committed pair
  int attempt = 0;
  UINT_PTR timerId = 0;
  bool active = false;
  bool needKeys = false;  // TSF route failed, fall back to key injection
  bool injected = false;  // keys already sent, do not send twice
  // Bumped on every schedule. An edit session that ended up running
  // asynchronously carries the value it was created with, so a stale one can
  // be recognised and its verdict discarded.
  int generation = 0;
};

CursorBackPending g_cursorBack;

// retry delays in ms, one per attempt
const UINT kCursorBackDelays[] = {10, 40, 120, 200};
const int kCursorBackMaxAttempts = 4;

void CALLBACK CursorBackTimerProc(HWND, UINT, UINT_PTR id, DWORD) {
  KillTimer(NULL, id);
  g_cursorBack.timerId = 0;
  if (!g_cursorBack.active)
    return;
  com_ptr<WeaselTSF> svc = g_cursorBack.service;
  if (svc == nullptr) {
    g_cursorBack.active = false;
    return;
  }
  svc->_RunCursorBackAttempt();
}

/* [auto_pair] Waiting for the user to let go of Shift before injecting the
 * arrow keys. Only used for the pairs that need Shift (（ 《 ｛ ＂). */
struct ShiftWaitPending {
  com_ptr<WeaselTSF> service;
  int count = 0;
  int tries = 0;
  UINT_PTR timerId = 0;
  bool active = false;
};

ShiftWaitPending g_shiftWait;

const UINT kShiftWaitInterval = 20;  // ms between polls
const int kShiftWaitMaxTries = 25;   // give up after ~500ms

void CALLBACK ShiftWaitTimerProc(HWND, UINT, UINT_PTR id, DWORD) {
  KillTimer(NULL, id);
  g_shiftWait.timerId = 0;
  if (!g_shiftWait.active)
    return;
  com_ptr<WeaselTSF> svc = g_shiftWait.service;
  if (svc == nullptr) {
    g_shiftWait.active = false;
    return;
  }
  svc->_RunShiftWait();
}

}  // namespace

class CCursorBackEditSession : public CEditSession {
 public:
  CCursorBackEditSession(com_ptr<WeaselTSF> pTextService,
                         com_ptr<ITfContext> pContext,
                         int cursorBack,
                         int attempt,
                         int targetOffset,
                         int endOffset,
                         int generation)
      : CEditSession(pTextService, pContext),
        _cursorBack(cursorBack),
        _attempt(attempt),
        _targetOffset(targetOffset),
        _endOffset(endOffset),
        _generation(generation) {}

  /* ITfEditSession */
  STDMETHODIMP DoEditSession(TfEditCookie ec);

 private:
  // Own copies rather than reads of g_cursorBack: this session may run
  // asynchronously, by which time a newer commit could have replaced it.
  int _cursorBack;
  int _attempt;
  int _targetOffset;
  int _endOffset;
  int _generation;

  bool _IsStale(const std::string& tag) const {
    if (_generation == g_cursorBack.generation)
      return false;
    APLOG(tag + "stale (generation " + std::to_string(_generation) + " vs " +
          std::to_string(g_cursorBack.generation) + "), verdict discarded");
    return true;
  }
};

STDAPI CCursorBackEditSession::DoEditSession(TfEditCookie ec) {
  std::string tag = "[Deferred#" + std::to_string(_attempt) + "] ";
  if (!_pContext) {
    APLOG(tag + "no context");
    return S_OK;
  }
  if (_IsStale(tag))
    return S_OK;

  int cur = autopair::GetCursorOffset(_pContext, ec);
  APLOG(tag + "offset now = " + std::to_string(cur) + " target=" +
        std::to_string(_targetOffset) + " end=" + std::to_string(_endOffset));

  if (cur >= 0 && cur == _targetOffset) {
    APLOG(tag + "already at target, TSF route worked");
    g_cursorBack.active = false;
    return S_OK;
  }

  // The caret is not where we put it. Either this text store only covers the
  // composition (Chromium based apps, IMM32 compatibility layer) and has been
  // reset, or the app moved the caret itself. Either way SetSelection cannot
  // reach the real caret here, so fall back to injecting key presses.
  if (_endOffset >= 0 && cur != _endOffset) {
    APLOG(tag + "caret not at expected end -> TSF route unusable, " +
          "will inject keys");
    g_cursorBack.needKeys = true;
    return S_OK;
  }

  TF_SELECTION sel;
  ULONG fetched = 0;
  if (FAILED(_pContext->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &sel,
                                     &fetched)) ||
      fetched == 0) {
    APLOG(tag + "GetSelection failed -> will inject keys");
    g_cursorBack.needKeys = true;
    return S_OK;
  }

  ITfRange* pRange = sel.range;
  pRange->Collapse(ec, TF_ANCHOR_END);
  LONG shifted = 0;
  HRESULT hr = pRange->ShiftStart(ec, -_cursorBack, &shifted, NULL);
  pRange->Collapse(ec, TF_ANCHOR_START);
  TF_SELECTION newSel;
  newSel.range = pRange;
  newSel.style.ase = TF_AE_NONE;
  newSel.style.fInterimChar = FALSE;
  HRESULT hr2 = _pContext->SetSelection(ec, 1, &newSel);
  pRange->Release();

  APLOG(tag + "ShiftStart hr=" + std::to_string((long)hr) +
        " shifted=" + std::to_string((long)shifted) +
        " SetSelection hr=" + std::to_string((long)hr2));
  APLOG(tag + "offset after = " +
        std::to_string(autopair::GetCursorOffset(_pContext, ec)));
  return S_OK;
}

void WeaselTSF::_ScheduleCursorBack(com_ptr<ITfContext> pContext,
                                    int cursorBack,
                                    int targetOffset) {
  if (cursorBack <= 0 || pContext == nullptr)
    return;

  if (g_cursorBack.timerId != 0) {
    KillTimer(NULL, g_cursorBack.timerId);
    g_cursorBack.timerId = 0;
  }
  g_cursorBack.service = this;
  g_cursorBack.context = pContext;
  g_cursorBack.cursorBack = cursorBack;
  g_cursorBack.targetOffset = targetOffset;
  g_cursorBack.endOffset = targetOffset >= 0 ? targetOffset + cursorBack : -1;
  g_cursorBack.attempt = 0;
  // A target we could not measure means the probe below cannot decide
  // anything, so go straight to key injection.
  g_cursorBack.needKeys = targetOffset < 0;
  g_cursorBack.injected = false;
  g_cursorBack.active = true;
  g_cursorBack.generation++;
  g_cursorBack.timerId =
      SetTimer(NULL, 0, kCursorBackDelays[0], CursorBackTimerProc);

  APLOG(std::string("[Deferred] scheduled timerId=") +
        std::to_string((unsigned long long)g_cursorBack.timerId) +
        " target=" + std::to_string(targetOffset) +
        " end=" + std::to_string(g_cursorBack.endOffset) +
        " gen=" + std::to_string(g_cursorBack.generation) +
        " needKeys=" + std::to_string((int)g_cursorBack.needKeys));
}

/* [auto_pair] Inject real VK_LEFT presses.
 *
 * Most apps (Chromium based ones, and anything going through the IMM32
 * compatibility layer) expose only the composition text through their TSF
 * text store, so SetSelection cannot reach the real caret. Sending actual
 * key presses works everywhere.
 *
 * Shift is never touched. For the fullwidth pairs （ 《 ｛ ＂ the user is still
 * holding it, and a bare VK_LEFT would extend a selection rather than move the
 * caret, so in that case the injection waits for the user to let go first.
 *
 * Injecting a Shift release instead, as V05 and V06 did, is not worth it:
 *   - Pairing it with a restoring press strands a press with no release if the
 *     user let go meanwhile, leaving Shift stuck down for everything typed
 *     afterwards. The race cannot be closed, since no API reports the physical
 *     state once our own injection has updated it.
 *   - Leaving it unpaired instead tells the system Shift is up while the user
 *     still holds it, and rime sees a Shift release the user did not perform.
 *     Whatever the exact mechanism, the Chinese/English toggling came back as
 *     soon as that release was injected, so it is simply not sent any more.
 *
 * Waiting costs a few tens of milliseconds on Shift pairs only, and the user is
 * releasing Shift in that same moment anyway. Pairs that need no Shift ([ 【 )
 * are injected immediately.
 */
void WeaselTSF::_SendCursorBackKeys(int count) {
  if (count <= 0)
    return;

  if (count > 8)
    count = 8;

  // GetAsyncKeyState, not GetKeyState: whether the user is physically holding
  // Shift right now is what matters, not the state as of the last message.
  if ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0) {
    if (g_shiftWait.timerId != 0) {
      KillTimer(NULL, g_shiftWait.timerId);
      g_shiftWait.timerId = 0;
    }
    g_shiftWait.service = this;
    g_shiftWait.count = count;
    g_shiftWait.tries = 0;
    g_shiftWait.active = true;
    g_shiftWait.timerId =
        SetTimer(NULL, 0, kShiftWaitInterval, ShiftWaitTimerProc);
    APLOG(std::string("[SendKeys] shift is held, waiting for release before "
                      "injecting; count=") +
          std::to_string(count));
    return;
  }

  _InjectLeftKeys(count);
}

/* [auto_pair] Poll until the user releases Shift, then inject. */
void WeaselTSF::_RunShiftWait() {
  g_shiftWait.tries++;
  int waited = g_shiftWait.tries * kShiftWaitInterval;

  if ((GetAsyncKeyState(VK_SHIFT) & 0x8000) == 0) {
    int count = g_shiftWait.count;
    g_shiftWait.active = false;
    g_shiftWait.service.Release();
    APLOG(std::string("[ShiftWait] released after ~") + std::to_string(waited) +
          "ms, injecting now");
    _InjectLeftKeys(count);
    return;
  }

  if (g_shiftWait.tries >= kShiftWaitMaxTries) {
    g_shiftWait.active = false;
    g_shiftWait.service.Release();
    APLOG(std::string("[ShiftWait] still held after ") +
          std::to_string(waited) + "ms, giving up; caret stays at the end");
    return;
  }

  g_shiftWait.timerId =
      SetTimer(NULL, 0, kShiftWaitInterval, ShiftWaitTimerProc);
}

/* [auto_pair] Send the arrow keys. Nothing but VK_LEFT is ever injected. */
void WeaselTSF::_InjectLeftKeys(int count) {
  if (count <= 0)
    return;
  if (count > 8)
    count = 8;

  INPUT inputs[16] = {};
  UINT n = 0;
  for (int i = 0; i < count; ++i) {
    inputs[n].type = INPUT_KEYBOARD;
    inputs[n].ki.wVk = VK_LEFT;
    inputs[n].ki.dwFlags = KEYEVENTF_EXTENDEDKEY;
    n++;
    inputs[n].type = INPUT_KEYBOARD;
    inputs[n].ki.wVk = VK_LEFT;
    inputs[n].ki.dwFlags = KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP;
    n++;
  }

  // Open the suppression window before sending, not after.
  _apSynthUntil = GetTickCount64() + 200;
  _apSynthSeen = 0;

  UINT sent = ::SendInput(n, inputs, sizeof(INPUT));
  DWORD err = (sent == n) ? 0 : GetLastError();

  APLOG(std::string("[SendKeys] injected VK_LEFT x") + std::to_string(count) +
        " inputs=" + std::to_string(n) + " sent=" + std::to_string(sent) +
        " err=" + std::to_string(err) + " shiftPhys=" +
        std::to_string((GetAsyncKeyState(VK_SHIFT) & 0x8000) ? 1 : 0));
}

void WeaselTSF::_RunCursorBackAttempt() {
  if (!g_cursorBack.active || g_cursorBack.context == nullptr) {
    g_cursorBack.active = false;
    return;
  }

  g_cursorBack.attempt++;
  int attempt = g_cursorBack.attempt;

  // Probe the TSF route, unless a previous attempt already ruled it out.
  CCursorBackEditSession* pEditSession =
      g_cursorBack.needKeys
          ? NULL
          : new CCursorBackEditSession(
                this, g_cursorBack.context, g_cursorBack.cursorBack, attempt,
                g_cursorBack.targetOffset, g_cursorBack.endOffset,
                g_cursorBack.generation);
  if (pEditSession != NULL) {
    HRESULT hrSession = S_OK;
    HRESULT hr = g_cursorBack.context->RequestEditSession(
        _tfClientId, pEditSession, TF_ES_SYNC | TF_ES_READWRITE, &hrSession);
    if (FAILED(hr)) {
      APLOG(std::string("[Deferred] sync request failed hr=") +
            std::to_string((long)hr) + ", retry async");
      hr = g_cursorBack.context->RequestEditSession(
          _tfClientId, pEditSession, TF_ES_ASYNCDONTCARE | TF_ES_READWRITE,
          &hrSession);
    }
    APLOG(std::string("[Deferred] attempt ") + std::to_string(attempt) +
          " request hr=" + std::to_string((long)hr) +
          " session hr=" + std::to_string((long)hrSession));
    pEditSession->Release();
  }

  // The probe above may have ruled out the TSF route. Inject keys once and
  // stop: re-probing afterwards would only read the same stale text store.
  if (g_cursorBack.active && g_cursorBack.needKeys && !g_cursorBack.injected) {
    g_cursorBack.injected = true;
    APLOG(std::string("[Deferred] falling back to key injection, cursorBack=") +
          std::to_string(g_cursorBack.cursorBack));
    _SendCursorBackKeys(g_cursorBack.cursorBack);
    g_cursorBack.active = false;
  }

  if (g_cursorBack.active && attempt < kCursorBackMaxAttempts) {
    g_cursorBack.timerId =
        SetTimer(NULL, 0, kCursorBackDelays[attempt], CursorBackTimerProc);
  } else {
    APLOG(std::string("[Deferred] done, attempts=") + std::to_string(attempt) +
          " needKeys=" + std::to_string((int)g_cursorBack.needKeys) +
          " injected=" + std::to_string((int)g_cursorBack.injected));
    g_cursorBack.active = false;
    g_cursorBack.context.Release();
    g_cursorBack.service.Release();
  }
}

/* End Composition */
class CEndCompositionEditSession : public CEditSession {
 public:
  CEndCompositionEditSession(com_ptr<WeaselTSF> pTextService,
                             com_ptr<ITfContext> pContext,
                             com_ptr<ITfComposition> pComposition,
                             BOOL clear = TRUE,
                             int cursorBack = 0)
      : CEditSession(pTextService, pContext),
        _clear(clear),
        _cursorBack(cursorBack) {
    _pComposition = pComposition;
  }

  /* ITfEditSession */
  STDMETHODIMP DoEditSession(TfEditCookie ec);

 private:
  com_ptr<ITfComposition> _pComposition;
  BOOL _clear;
  int _cursorBack;
};

STDAPI CEndCompositionEditSession::DoEditSession(TfEditCookie ec) {
  APLOG(std::string("[EndComp] enter, cursorBack=") +
        std::to_string(_cursorBack) + " clear=" + std::to_string((int)_clear) +
        " comp=" + std::to_string(_pComposition != nullptr ? 1 : 0));
  /* Clear the dummy text we set before, if any. */
  if (_pComposition == nullptr) {
    APLOG("[EndComp] early return: composition is null");
    return S_OK;
  }
  // Avoid null pointer dereference
  if (!_pTextService || !_pContext) {
    APLOG("[EndComp] early return: textservice or context is null");
    return S_OK;
  }

  _pTextService->_ClearCompositionDisplayAttributes(ec, _pContext);

  com_ptr<ITfRange> pCompositionRange;
  if (_clear && _pComposition->GetRange(&pCompositionRange) == S_OK)
    pCompositionRange->SetText(ec, 0, L"", 0);

  _pComposition->EndComposition(ec);
  if (_pTextService)  // if _pTextService released, skip _FinalizeComposition
    _pTextService->_FinalizeComposition();

  // [auto_pair] move cursor back in the same edit session, after
  // EndComposition, so no other async session can override it
  if (_cursorBack > 0) {
    APLOG(std::string("[EndComp] cursorBack=") + std::to_string(_cursorBack));
    APLOG("[EndComp] offset before = " +
          std::to_string(autopair::GetCursorOffset(_pContext, ec)));

    TF_SELECTION sel;
    ULONG fetched = 0;
    if (SUCCEEDED(_pContext->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &sel,
                                          &fetched)) &&
        fetched > 0) {
      ITfRange* pRange = sel.range;

      // log whether the selection is empty (a caret) or a range
      BOOL isEmpty = FALSE;
      pRange->IsEmpty(ec, &isEmpty);
      APLOG(std::string("[EndComp] selection isEmpty=") +
            std::to_string((int)isEmpty));

      pRange->Collapse(ec, TF_ANCHOR_END);
      LONG shifted = 0;
      HRESULT hr = pRange->ShiftStart(ec, -_cursorBack, &shifted, NULL);
      pRange->Collapse(ec, TF_ANCHOR_START);
      TF_SELECTION newSel;
      newSel.range = pRange;
      newSel.style.ase = TF_AE_NONE;
      newSel.style.fInterimChar = FALSE;
      HRESULT hr2 = _pContext->SetSelection(ec, 1, &newSel);
      APLOG(std::string("[EndComp] ShiftStart hr=") + std::to_string((long)hr) +
            " shifted=" + std::to_string((long)shifted) +
            " SetSelection hr=" + std::to_string((long)hr2));
      pRange->Release();

      int after = autopair::GetCursorOffset(_pContext, ec);
      APLOG("[EndComp] offset after = " + std::to_string(after));

      // The app will very likely reset the caret to the end of the committed
      // text once this edit transaction completes. Redo the move from the
      // message loop.
      if (_pTextService)
        _pTextService->_ScheduleCursorBack(_pContext, _cursorBack, after);
    } else {
      // No selection to work with: schedule with an unmeasurable target so the
      // deferred step goes straight to key injection.
      APLOG("[EndComp] GetSelection failed -> schedule key injection");
      if (_pTextService)
        _pTextService->_ScheduleCursorBack(_pContext, _cursorBack, -1);
    }
  }
  return S_OK;
}

void WeaselTSF::_EndComposition(com_ptr<ITfContext> pContext,
                                BOOL clear,
                                int cursorBack) {
  CEndCompositionEditSession* pEditSession;
  HRESULT hr;

  _cand->EndUI();
  if ((pEditSession = new CEndCompositionEditSession(
           this, pContext, _pComposition, clear, cursorBack)) != NULL) {
    pContext->RequestEditSession(_tfClientId, pEditSession,
                                 TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &hr);
    pEditSession->Release();
    if (cursorBack > 0) {
      APLOG(std::string("[_EndComposition] requested, cursorBack=") +
            std::to_string(cursorBack) +
            " RequestEditSession hr=" + std::to_string((long)hr));
    }
  }
}

/* Get Text Extent */
class CGetTextExtentEditSession : public CEditSession {
 public:
  CGetTextExtentEditSession(com_ptr<WeaselTSF> pTextService,
                            com_ptr<ITfContext> pContext,
                            com_ptr<ITfContextView> pContextView,
                            com_ptr<ITfComposition> pComposition,
                            bool enhancedPosition)
      : CEditSession(pTextService, pContext) {
    _pContextView = pContextView;
    _pComposition = pComposition;
    _enhancedPosition = enhancedPosition;
  }

  /* ITfEditSession */
  STDMETHODIMP DoEditSession(TfEditCookie ec);

 private:
  com_ptr<ITfContextView> _pContextView;
  com_ptr<ITfComposition> _pComposition;
  bool _enhancedPosition;
};

STDAPI CGetTextExtentEditSession::DoEditSession(TfEditCookie ec) {
  com_ptr<ITfInsertAtSelection> pInsertAtSelection;
  com_ptr<ITfRange> pRangeComposition;
  ITfRange* pRange;
  RECT rc;
  BOOL fClipped;
  TF_SELECTION selection;
  ULONG nSelection;

  if (FAILED(_pContext->QueryInterface(IID_ITfInsertAtSelection,
                                       (LPVOID*)&pInsertAtSelection)))
    return E_FAIL;
  if (FAILED(_pContext->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &selection,
                                     &nSelection)))
    return E_FAIL;

  if (_pComposition != nullptr && _pComposition->GetRange(&pRange) == S_OK) {
    pRange->Collapse(ec, TF_ANCHOR_START);
  } else {
    // composition end
    // note: selection.range is always an empty range
    pRange = selection.range;
  }

  if ((_pContextView->GetTextExt(ec, pRange, &rc, &fClipped)) == S_OK &&
      (rc.left != 0 || rc.top != 0)) {
    // get the foreground window pos and check if rc from GetTextExt is out of
    // window
    if (_enhancedPosition) {
      HWND hwnd;
      RECT rcForegroundWindow;
      hwnd = GetForegroundWindow();
      ::GetWindowRect(hwnd, &rcForegroundWindow);

      if (rc.left < rcForegroundWindow.left ||
          rc.left > rcForegroundWindow.right ||
          rc.top < rcForegroundWindow.top ||
          rc.top > rcForegroundWindow.bottom) {
        POINT pt;
        bool hasCaret = ::GetCaretPos(&pt);
        int offsetx = rcForegroundWindow.left - rc.left + (hasCaret ? pt.x : 0);
        int offsety = rcForegroundWindow.top - rc.top + (hasCaret ? pt.y : 0);
        rc.left += offsetx;
        rc.right += offsetx;
        rc.top += offsety;
        rc.bottom += offsety;
      }
    }
    _pTextService->_SetCompositionPosition(rc);
  }
  return S_OK;
}

/* Composition Window Handling */
BOOL WeaselTSF::_UpdateCompositionWindow(com_ptr<ITfContext> pContext) {
  com_ptr<ITfContextView> pContextView;
  if (pContext->GetActiveView(&pContextView) != S_OK)
    return FALSE;
  com_ptr<CGetTextExtentEditSession> pEditSession;
  pEditSession.Attach(
      new CGetTextExtentEditSession(this, pContext, pContextView, _pComposition,
                                    _cand->style().enhanced_position));
  if (pEditSession == NULL) {
    return FALSE;
  }
  HRESULT hr;
  pContext->RequestEditSession(_tfClientId, pEditSession,
                               TF_ES_ASYNCDONTCARE | TF_ES_READ, &hr);
  return SUCCEEDED(hr);
}

void WeaselTSF::_SetCompositionPosition(const RECT& rc) {
  /* Test if rect is valid.
   * If it is invalid during CUAS test, we need to apply CUAS workaround */
  if (!_fCUASWorkaroundTested) {
    _fCUASWorkaroundTested = TRUE;
    if (rc.top == rc.bottom) {
      _fCUASWorkaroundEnabled = TRUE;
      return;
    }
  }
  RECT _rc;
  _rc.left = _rc.right = rc.left;
  _rc.top = _rc.bottom = rc.bottom;
  m_client.UpdateInputPosition(rc);
  _cand->UpdateInputPosition(rc);
}

/* Inline Preedit */
class CInlinePreeditEditSession : public CEditSession {
 public:
  CInlinePreeditEditSession(com_ptr<WeaselTSF> pTextService,
                            com_ptr<ITfContext> pContext,
                            com_ptr<ITfComposition> pComposition,
                            const std::shared_ptr<weasel::Context> context)
      : CEditSession(pTextService, pContext),
        _pComposition(pComposition),
        _context(context) {}

  /* ITfEditSession */
  STDMETHODIMP DoEditSession(TfEditCookie ec);

 private:
  com_ptr<ITfComposition> _pComposition;
  const std::shared_ptr<weasel::Context> _context;
};

STDAPI CInlinePreeditEditSession::DoEditSession(TfEditCookie ec) {
  std::wstring preedit = _context->preedit.str;

  com_ptr<ITfRange> pRangeComposition;
  if (_pComposition == nullptr)
    return E_FAIL;
  if ((_pComposition->GetRange(&pRangeComposition)) != S_OK)
    return E_FAIL;

  if ((pRangeComposition->SetText(ec, 0, preedit.c_str(),
                                  static_cast<LONG>(preedit.length()))) != S_OK)
    return E_FAIL;

  /* TODO: Check the availability and correctness of these values */
  int sel_cursor = -1;
  for (size_t i = 0; i < _context->preedit.attributes.size(); i++) {
    if (_context->preedit.attributes.at(i).type == weasel::HIGHLIGHTED) {
      sel_cursor = _context->preedit.attributes.at(i).range.cursor;
      break;
    }
  }

  _pTextService->_SetCompositionDisplayAttributes(ec, _pContext,
                                                  pRangeComposition);

  /* Set caret */
  LONG cch;
  TF_SELECTION tfSelection;
  if (sel_cursor < 0) {
    pRangeComposition->Collapse(ec, TF_ANCHOR_END);
  } else {
    pRangeComposition->Collapse(ec, TF_ANCHOR_START);
    pRangeComposition->ShiftStart(ec, sel_cursor, &cch, NULL);
  }
  tfSelection.range = pRangeComposition;
  tfSelection.style.ase = TF_AE_NONE;
  tfSelection.style.fInterimChar = FALSE;
  _pContext->SetSelection(ec, 1, &tfSelection);

  return S_OK;
}

BOOL WeaselTSF::_ShowInlinePreedit(
    com_ptr<ITfContext> pContext,
    const std::shared_ptr<weasel::Context> context) {
  com_ptr<CInlinePreeditEditSession> pEditSession;
  pEditSession.Attach(
      new CInlinePreeditEditSession(this, pContext, _pComposition, context));
  if (pEditSession != NULL) {
    HRESULT hr;
    pContext->RequestEditSession(_tfClientId, pEditSession,
                                 TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &hr);
  }
  return TRUE;
}

/* Update Composition */
class CInsertTextEditSession : public CEditSession {
 public:
  CInsertTextEditSession(com_ptr<WeaselTSF> pTextService,
                         com_ptr<ITfContext> pContext,
                         com_ptr<ITfComposition> pComposition,
                         const std::wstring& text)
      : CEditSession(pTextService, pContext),
        _text(text),
        _pComposition(pComposition) {}

  /* ITfEditSession */
  STDMETHODIMP DoEditSession(TfEditCookie ec);

 private:
  std::wstring _text;
  com_ptr<ITfComposition> _pComposition;
};

STDMETHODIMP CInsertTextEditSession::DoEditSession(TfEditCookie ec) {
  com_ptr<ITfRange> pRange;
  TF_SELECTION tfSelection;
  HRESULT hRet = S_OK;

  APLOG(std::string("[InsertText] enter, textLen=") +
        std::to_string(_text.length()));

  if (_pComposition == nullptr)
    return E_FAIL;
  if (FAILED(_pComposition->GetRange(&pRange)))
    return E_FAIL;

  if (FAILED(pRange->SetText(ec, 0, _text.c_str(),
                             static_cast<LONG>(_text.length()))))
    return E_FAIL;

  /* update the selection to an insertion point just past the inserted text. */
  pRange->Collapse(ec, TF_ANCHOR_END);

  tfSelection.range = pRange;
  tfSelection.style.ase = TF_AE_NONE;
  tfSelection.style.fInterimChar = FALSE;

  _pContext->SetSelection(ec, 1, &tfSelection);

  return hRet;
}

BOOL WeaselTSF::_InsertText(com_ptr<ITfContext> pContext,
                            const std::wstring& text) {
  CInsertTextEditSession* pEditSession;
  HRESULT hr;

  if ((pEditSession = new CInsertTextEditSession(this, pContext, _pComposition,
                                                 text)) != NULL) {
    pContext->RequestEditSession(_tfClientId, pEditSession,
                                 TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &hr);
    pEditSession->Release();
  }

  return TRUE;
}

/* [auto_pair] log InsertText edit session execution */

void WeaselTSF::_UpdateComposition(com_ptr<ITfContext> pContext) {
  HRESULT hr;

  _pEditSessionContext = pContext;

  _pEditSessionContext->RequestEditSession(
      _tfClientId, this, TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &hr);
  _async_edit = !!(hr == TF_S_ASYNC);
  _UpdateCompositionWindow(pContext);
}

/* Composition State */
STDAPI WeaselTSF::OnCompositionTerminated(TfEditCookie ecWrite,
                                          ITfComposition* pComposition) {
  // NOTE:
  // This will be called when an edit session ended up with an empty composition
  // string, Even if it is closed normally. Silly M$.

  _AbortComposition();
  return S_OK;
}

void WeaselTSF::_AbortComposition(bool clear) {
  m_client.ClearComposition();
  if (_IsComposing()) {
    _EndComposition(_pEditSessionContext, clear);
  }
  _committed = TRUE;
  _cand->Destroy();
}

void WeaselTSF::_FinalizeComposition() {
  _pComposition = nullptr;
}

void WeaselTSF::_SetComposition(com_ptr<ITfComposition> pComposition) {
  _pComposition = pComposition;
}

BOOL WeaselTSF::_IsComposing() {
  return _pComposition != NULL;
}
