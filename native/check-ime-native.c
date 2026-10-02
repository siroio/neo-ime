/* Native lifecycle check: gcc ... check-ime-native.c -limm32 -lcomctl32 */
#define NEO_IME_TEST
#include <windows.h>
#include <imm.h>
#include <string.h>
/* Synthetic IMM payload; real window threads and messages below. */
static BOOL fail_composition;
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
#include "neo-ime-native.c"
#include <assert.h>
#include <stdio.h>

static HANDLE ready;
static HWND test_window;
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
  SendMessageW(test_window, WM_IME_COMPOSITION, 0, GCS_COMPSTR | GCS_CURSORPOS | GCS_COMPATTR);
  assert(state->units == 4 && state->cursor == 3 && state->composing);
  assert(state->text[1] == 0xd83d && state->attributes[1] == 1);
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
