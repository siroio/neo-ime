/* Native lifecycle check: gcc ... check-ime-native.c -limm32 -lcomctl32 */
#define NEO_IME_TEST
#include <windows.h>
#include <imm.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>
#include <limits.h>
/* Synthetic IMM payload; real window threads and messages below. */
static BOOL fail_composition;
static HWND test_window;
static BOOL setting_position;
static unsigned int default_start_count;
static int bad_candidate;
static DWORD fake_candidates(HIMC context, DWORD index, CANDIDATELIST *output, DWORD bytes) {
  (void)context; (void)index; (void)bytes;
  struct { DWORD size, style, count, selection, start, page, offsets[2]; WCHAR text[5]; } data =
    {0, IME_CAND_READ, 2, 1, 1, 1, {32, 36}, {0x4e9c, 0, 0xd83d, 0xde00, 0}};
  data.size = sizeof(data);
  if (bad_candidate == 1) data.offsets[0] = 1;
  if (bad_candidate == 2) { data.offsets[1] = sizeof(data) - 2; ((WCHAR *)&data)[sizeof(data)/2-1] = 1; }
  if (bad_candidate == 3) data.count = 999;
  if (bad_candidate == 4) { data.count = 0; data.start = 1; }
  if (output) memcpy(output, &data, sizeof(data));
  return sizeof(data);
}
static LRESULT CALLBACK fake_default(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_IME_STARTCOMPOSITION) ++default_start_count;
  return DefWindowProcW(hwnd, msg, wp, lp);
}
static BOOL fake_set_composition(HIMC context, COMPOSITIONFORM *form) {
  (void)context;
  if (form->dwStyle == CFS_RECT) {
    assert((int64_t)form->rcArea.right - form->rcArea.left <= INT_MAX);
    assert((int64_t)form->rcArea.bottom - form->rcArea.top <= INT_MAX);
  }
  /* Google IME can synchronously restart composition while setting its form. */
  assert(!setting_position);
  setting_position = TRUE;
  SendMessageW(test_window, WM_IME_STARTCOMPOSITION, 0, 0);
  setting_position = FALSE;
  return TRUE;
}
static LONG fake_composition(HIMC context, DWORD kind, LPVOID output, DWORD bytes) {
  (void)context;
  static const WCHAR text[] = {0x3042, 0xd83d, 0xde00, 0x3044};
  static const BYTE attrs[] = {0, 1, 1, 0};
  if (fail_composition) return IMM_ERROR_GENERAL;
  if (kind == GCS_CURSORPOS) return 3;
  const void *data = kind == GCS_COMPSTR ? (const void *)text : (const void *)attrs;
  LONG size = kind == GCS_COMPSTR ? sizeof(text) : sizeof(attrs);
  if (output) memcpy(output, data, bytes < (DWORD)size ? bytes : (DWORD)size);
  return size;
}
#define ImmGetCompositionStringW fake_composition
#define ImmGetCandidateListW fake_candidates
#define ImmSetCompositionWindow fake_set_composition
#define DefWindowProcW fake_default
#include "neo-ime-native.c"
#include <assert.h>
#include <stdio.h>

static HANDLE ready;
static DWORD WINAPI window_thread(void *unused) {
  (void)unused;
  WNDCLASSW cls = {0};
  cls.lpfnWndProc = DefWindowProcW;
  cls.hInstance = GetModuleHandleW(NULL);
  cls.lpszClassName = L"Emacs";
  assert(RegisterClassW(&cls));
  test_window = CreateWindowW(L"Emacs", L"neo-ime check", WS_OVERLAPPED,
                              0, 0, 320, 200, NULL, NULL, cls.hInstance, NULL);
  assert(test_window);
  SetEvent(ready);
  MSG msg;
  while (GetMessageW(&msg, NULL, 0, 0) > 0) DispatchMessageW(&msg);
  DestroyWindow(test_window);
  return 0;
}

int main(void) {
  ready = CreateEventW(NULL, TRUE, FALSE, NULL);
  DWORD tid;
  HANDLE thread = CreateThread(NULL, 0, window_thread, NULL, 0, &tid);
  assert(thread && WaitForSingleObject(ready, 3000) == WAIT_OBJECT_0);
  assert(!attach_window(NULL));
  assert(attach_window(test_window));
  assert(attach_window(test_window));
  State *state = find_state(test_window);
  assert(state && state->alive);
  SendMessageW(test_window, WM_IME_STARTCOMPOSITION, 0, 0);
  assert(state->composing);
  assert(default_start_count == 1);
  SendMessageW(test_window, WM_IME_COMPOSITION, 0, GCS_COMPSTR | GCS_CURSORPOS | GCS_COMPATTR);
  assert(state->units == 4 && state->cursor == 3 && state->composing);
  assert(state->text[1] == 0xd83d && state->attributes[1] == 1);
  SendMessageW(test_window, WM_IME_NOTIFY, IMN_CHANGECANDIDATE, 1);
  assert(state->candidates && state->candidates->dwCount == 2);
  assert(state->candidates->dwSelection == 1 && state->candidates->dwPageStart == 1);
  for (bad_candidate = 1; bad_candidate <= 4; ++bad_candidate) {
    SendMessageW(test_window, WM_IME_NOTIFY, IMN_CHANGECANDIDATE, 1);
    assert(!state->candidates);
  }
  bad_candidate = 0;
  SendMessageW(test_window, WM_IME_NOTIFY, IMN_OPENCANDIDATE, 1);
  assert(state->candidates);
  SendMessageW(test_window, WM_IME_NOTIFY, IMN_CLOSECANDIDATE, 1);
  assert(!state->candidates && state->composing);
  /* Owner-drawn preedit must answer the IME's screen-coordinate query;
   * its hidden native composition rectangle is not a candidate anchor. */
  state->anchor = (POINT){25, 40};
  state->height = 24;
  IMECHARPOSITION position = {0};
  position.dwSize = sizeof(position);
  position.dwCharPos = 1;
  POINT expected = state->anchor;
  assert(ClientToScreen(test_window, &expected));
  assert(SendMessageW(test_window, WM_IME_REQUEST, IMR_QUERYCHARPOSITION,
                      (LPARAM)&position));
  assert(position.pt.x == expected.x && position.pt.y == expected.y);
  assert(position.cLineHeight == 24);
  assert(position.rcDocument.left <= position.pt.x
         && position.rcDocument.right > position.pt.x);
  assert(position.dwCharPos == 1);
  position.dwSize = 1;
  assert(!SendMessageW(test_window, WM_IME_REQUEST, IMR_QUERYCHARPOSITION,
                       (LPARAM)&position));
  uint64_t revision = state->revision;
  SendMessageW(test_window, WM_IME_STARTCOMPOSITION, 0, 0);
  assert(state->revision == revision && state->units == 4);
  /* Partial result + remaining preedit must not clear the next clause. */
  SendMessageW(test_window, WM_IME_COMPOSITION, 0, GCS_RESULTSTR | GCS_COMPSTR);
  assert(state->units == 4 && state->composing);
  SendMessageW(test_window, WM_IME_COMPOSITION, 0, GCS_RESULTSTR);
  assert(!state->composing && state->units == 0);
  SendMessageW(test_window, WM_IME_STARTCOMPOSITION, 0, 0);
  fail_composition = TRUE;
  SendMessageW(test_window, WM_IME_COMPOSITION, 0, GCS_COMPSTR);
  assert(state->fallback && !state->composing);
  fail_composition = FALSE;
  SendMessageW(test_window, WM_IME_STARTCOMPOSITION, 0, 0);
  assert(!state->fallback);
  /* If cancellation/focus cleanup is removed, this stays active. */
  SendMessageW(test_window, WM_KILLFOCUS, 0, 0);
  assert(!state->composing);
  assert(detach_window(test_window));
  assert(!find_state(test_window));
  assert(detach_window(test_window));
  assert(attach_window(test_window));
  /* Destruction clears the state on the owning GUI thread. */
  PostThreadMessageW(tid, WM_QUIT, 0, 0);
  assert(WaitForSingleObject(thread, 3000) == WAIT_OBJECT_0);
  assert(!find_state(test_window)->alive);
  assert(detach_window(test_window));
  CloseHandle(thread);
  CloseHandle(ready);
  puts("NEO_IME_NATIVE=PASS cross-thread lifecycle synthetic-IMM utf16 partial-commit safe-fallback");
}
