/* neo-ime: Windows IMM composition bridge, GPL-3.0-or-later.
 * No Emacs API calls on the GUI thread.  Lisp polls locked snapshots.
 * Confirmed characters always follow Emacs's original WM_IME_CHAR path.
 */
#define UNICODE
#define _UNICODE
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#include <windows.h>
#include <commctrl.h>
#include <imm.h>
#include <stdint.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#ifndef NEO_IME_TEST
#include <emacs-module.h>
__declspec(dllexport) int plugin_is_GPL_compatible;
#endif

typedef struct State {
  HWND hwnd;
  SRWLOCK lock;
  BOOL alive, composing, saved_form, fallback;
  COMPOSITIONFORM original_form;
  CANDIDATEFORM original_candidate;
  BOOL saved_candidate;
  uint64_t revision;
  WCHAR *text;
  BYTE *attributes;
  LONG units, cursor;
  POINT anchor;
  int height;
  struct State *next;
} State;

static State *states;
static UINT control_message;
static const UINT_PTR subclass_id = 0x4e454f49;
static char control_cookie;
enum { ATTACH = 1, DETACH, CANCEL, POSITION };
typedef struct { State *state; BOOL ok; } AttachRequest;
static AttachRequest request;

static State *find_state(HWND hwnd) {
  for (State *s = states; s; s = s->next)
    if (s->hwnd == hwnd) return s;
  return NULL;
}

/* Called on the GUI thread, except after that thread has removed the subclass. */
static void clear_state(State *s) {
  AcquireSRWLockExclusive(&s->lock);
  free(s->text);
  free(s->attributes);
  s->text = NULL;
  s->attributes = NULL;
  s->units = s->cursor = 0;
  s->composing = FALSE;
  ++s->revision;
  ReleaseSRWLockExclusive(&s->lock);
}

static void position_ime(State *s, HIMC context) {
  POINT anchor;
  int height;
  AcquireSRWLockShared(&s->lock);
  anchor = s->anchor;
  height = s->height;
  ReleaseSRWLockShared(&s->lock);
  /* Google IME can ignore ISC_SHOWUICOMPOSITIONWINDOW.  Also move its
   * composition form off the client area, keeping candidates at point. */
  COMPOSITIONFORM composition = {0};
  composition.dwStyle = CFS_RECT;
  composition.ptCurrentPos = anchor;
  RECT client;
  GetClientRect(s->hwnd, &client);
  composition.rcArea.left = composition.rcArea.top = -INT_MAX;
  composition.rcArea.right = client.right;
  composition.rcArea.bottom = client.bottom;
  ImmSetCompositionWindow(context, &composition);
  CANDIDATEFORM candidate = {0};
  candidate.dwStyle = CFS_EXCLUDE;
  candidate.ptCurrentPos = anchor;
  candidate.rcArea.left = anchor.x;
  candidate.rcArea.right = anchor.x + 1;
  candidate.rcArea.top = anchor.y;
  candidate.rcArea.bottom = anchor.y + height;
  ImmSetCandidateWindow(context, &candidate);
}

static BOOL read_composition(State *s, HIMC context) {
  LONG bytes = ImmGetCompositionStringW(context, GCS_COMPSTR, NULL, 0);
  /* Reject invalid API lengths rather than allocate from signed errors. */
  if (bytes < 0 || bytes > 1024 * 1024 || bytes % sizeof(WCHAR)) return FALSE;
  WCHAR *text = calloc((size_t)bytes / sizeof(WCHAR) + 1, sizeof(WCHAR));
  BYTE *attributes = calloc((size_t)bytes / sizeof(WCHAR) + 1, 1);
  if (!text || !attributes) { free(text); free(attributes); return FALSE; }
  LONG actual = ImmGetCompositionStringW(context, GCS_COMPSTR, text, bytes);
  if (actual < 0 || actual > bytes || actual % sizeof(WCHAR)) {
    free(text); free(attributes); return FALSE;
  }
  LONG units = actual / (LONG)sizeof(WCHAR);
  ImmGetCompositionStringW(context, GCS_COMPATTR, attributes, units);
  LONG cursor = ImmGetCompositionStringW(context, GCS_CURSORPOS, NULL, 0);
  if (cursor < 0) cursor = 0;
  if (cursor > units) cursor = units;
  AcquireSRWLockExclusive(&s->lock);
  free(s->text);
  free(s->attributes);
  s->text = text;
  s->attributes = attributes;
  s->units = units;
  s->cursor = cursor;
  s->composing = TRUE;
  ++s->revision;
  ReleaseSRWLockExclusive(&s->lock);
  return TRUE;
}

static void restore_ime(State *s) {
  HIMC context = ImmGetContext(s->hwnd);
  if (!context) return;
  if (s->saved_form) ImmSetCompositionWindow(context, &s->original_form);
  if (s->saved_candidate) ImmSetCandidateWindow(context, &s->original_candidate);
  ImmReleaseContext(s->hwnd, context);
}

static LRESULT CALLBACK ime_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                  UINT_PTR id, DWORD_PTR ref) {
  (void)id;
  State *s = (State *)ref;
  if (msg == control_message && wp == (WPARAM)&control_cookie) {
    if (lp == DETACH) {
      restore_ime(s);
      RemoveWindowSubclass(hwnd, ime_proc, subclass_id);
      /* Restore default native UI for this focused window. */
      if (GetFocus() == hwnd)
        SendMessageW(hwnd, WM_IME_SETCONTEXT, TRUE, ISC_SHOWUIALL);
      return 1;
    }
    HIMC context = ImmGetContext(hwnd);
    if (context) {
      if (lp == CANCEL) ImmNotifyIME(context, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
      else if (lp == POSITION && s->composing) position_ime(s, context);
      ImmReleaseContext(hwnd, context);
    }
    if (lp == CANCEL) clear_state(s);
    return 1;
  }
  if (msg == WM_IME_SETCONTEXT && !s->fallback)
    return DefSubclassProc(hwnd, msg, wp, lp & ~ISC_SHOWUICOMPOSITIONWINDOW);
  if (msg == WM_IME_STARTCOMPOSITION) {
    clear_state(s);
    s->fallback = FALSE;
    HIMC context = ImmGetContext(hwnd);
    if (context) {
      s->saved_form = ImmGetCompositionWindow(context, &s->original_form);
      s->saved_candidate = ImmGetCandidateWindow(context, 0, &s->original_candidate);
      position_ime(s, context);
      ImmReleaseContext(hwnd, context);
    } else {
      s->fallback = TRUE;
      return DefSubclassProc(hwnd, msg, wp, lp);
    }
    AcquireSRWLockExclusive(&s->lock);
    s->composing = TRUE;
    ReleaseSRWLockExclusive(&s->lock);
    /* We own preedit drawing.  Default processing would open the white box. */
    return 0;
  }
  if (msg == WM_IME_COMPOSITION) {
    if (s->fallback) return DefSubclassProc(hwnd, msg, wp, lp);
    HIMC context = ImmGetContext(hwnd);
    BOOL valid = context != NULL;
    if (context) {
      if (lp & (GCS_COMPSTR | GCS_COMPATTR | GCS_CURSORPOS)) valid = read_composition(s, context);
      else if (!lp || (lp & GCS_RESULTSTR)) clear_state(s);
      if (valid) position_ime(s, context);
      ImmReleaseContext(hwnd, context);
    }
    if (!valid) {
      clear_state(s);
      s->fallback = TRUE;
      restore_ime(s);
      DefSubclassProc(hwnd, WM_IME_SETCONTEXT, TRUE, ISC_SHOWUIALL);
      return DefSubclassProc(hwnd, msg, wp, lp);
    }
    /* Never insert result text ourselves: preserve Emacs's commit path.
     * A result can include a new preedit clause, so keep its snapshot. */
    if (lp & GCS_RESULTSTR) return DefSubclassProc(hwnd, msg, wp, lp);
    return 0;
  }
  if (msg == WM_IME_ENDCOMPOSITION || msg == WM_KILLFOCUS) {
    clear_state(s);
    restore_ime(s);
  }
  if (msg == WM_NCDESTROY) {
    clear_state(s);
    AcquireSRWLockExclusive(&s->lock);
    s->alive = FALSE;
    ReleaseSRWLockExclusive(&s->lock);
    RemoveWindowSubclass(hwnd, ime_proc, subclass_id);
  }
  return DefSubclassProc(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK attach_hook(int code, WPARAM wp, LPARAM lp) {
  if (code >= 0) {
    const CWPSTRUCT *message = (const CWPSTRUCT *)lp;
    if (message->message == control_message && message->wParam == (WPARAM)&control_cookie
        && message->lParam == ATTACH && message->hwnd == request.state->hwnd) {
      request.ok = SetWindowSubclass(message->hwnd, ime_proc, subclass_id,
                                     (DWORD_PTR)request.state);
      if (request.ok && GetFocus() == message->hwnd)
        SendMessageW(message->hwnd, WM_IME_SETCONTEXT, TRUE, ISC_SHOWUIALL);
    }
  }
  return CallNextHookEx(NULL, code, wp, lp);
}

static BOOL attach_window(HWND hwnd) {
  DWORD pid = 0;
  DWORD tid = GetWindowThreadProcessId(hwnd, &pid);
  WCHAR class_name[32];
  if (!tid || pid != GetCurrentProcessId()
      || !GetClassNameW(hwnd, class_name, 32) || wcscmp(class_name, L"Emacs")) return FALSE;
  if (find_state(hwnd)) return TRUE;
  if (!control_message) control_message = RegisterWindowMessageW(L"neo-ime-native-v1");
  if (!control_message) return FALSE;
  State *s = calloc(1, sizeof(*s));
  if (!s) return FALSE;
  s->hwnd = hwnd;
  s->alive = TRUE;
  s->height = 20;
  InitializeSRWLock(&s->lock);
  /* SetWindowSubclass must run on the HWND's thread, not the Lisp thread. */
  request.state = s;
  request.ok = FALSE;
  HHOOK hook = SetWindowsHookExW(WH_CALLWNDPROC, attach_hook, NULL, tid);
  if (hook) {
    SendMessageW(hwnd, control_message, (WPARAM)&control_cookie, ATTACH);
    UnhookWindowsHookEx(hook);
  }
  if (!request.ok) { free(s); request.state = NULL; return FALSE; }
  request.state = NULL;
  s->next = states;
  states = s;
  return TRUE;
}

static BOOL detach_window(HWND hwnd) {
  State **link = &states;
  while (*link && (*link)->hwnd != hwnd) link = &(*link)->next;
  if (!*link) return TRUE;
  State *s = *link;
  if (s->alive && !SendMessageW(hwnd, control_message, (WPARAM)&control_cookie, DETACH))
    return FALSE;
  *link = s->next;
  clear_state(s);
  free(s);
  return TRUE;
}

#ifndef NEO_IME_TEST
static emacs_value error(emacs_env *env, const char *message) {
  emacs_value text = env->make_string(env, message, (ptrdiff_t)strlen(message));
  emacs_value args = env->funcall(env, env->intern(env, "list"), 1, &text);
  env->non_local_exit_signal(env, env->intern(env, "error"), args);
  return env->intern(env, "nil");
}

static HWND argument_hwnd(emacs_env *env, emacs_value arg) {
  intmax_t handle = env->extract_integer(env, arg);
  if (env->non_local_exit_check(env) != emacs_funcall_exit_return) return NULL;
  return (HWND)(uintptr_t)handle;
}

static emacs_value native_attach(emacs_env *env, ptrdiff_t n, emacs_value *args, void *data) {
  (void)n; (void)data;
  HWND hwnd = argument_hwnd(env, args[0]);
  if (!hwnd || !attach_window(hwnd)) return error(env, "Cannot attach neo-ime to this Emacs GUI frame");
  return env->intern(env, "t");
}

static emacs_value native_detach(emacs_env *env, ptrdiff_t n, emacs_value *args, void *data) {
  (void)n; (void)data;
  HWND hwnd = argument_hwnd(env, args[0]);
  if (!detach_window(hwnd)) return error(env, "Cannot detach neo-ime from frame");
  return env->intern(env, "t");
}

static emacs_value native_position(emacs_env *env, ptrdiff_t n, emacs_value *args, void *data) {
  (void)n; (void)data;
  HWND hwnd = argument_hwnd(env, args[0]);
  intmax_t x = env->extract_integer(env, args[1]);
  intmax_t y = env->extract_integer(env, args[2]);
  intmax_t height = env->extract_integer(env, args[3]);
  if (env->non_local_exit_check(env) != emacs_funcall_exit_return) return env->intern(env, "nil");
  if (x < 0 || x > INT_MAX || y < 0 || y > INT_MAX || height < 1 || height > INT_MAX)
    return error(env, "Invalid neo-ime candidate coordinates");
  State *s = find_state(hwnd);
  if (s) {
    AcquireSRWLockExclusive(&s->lock);
    s->anchor.x = (LONG)x; s->anchor.y = (LONG)y; s->height = (int)height;
    ReleaseSRWLockExclusive(&s->lock);
    if (s->alive) SendMessageW(hwnd, control_message, (WPARAM)&control_cookie, POSITION);
  }
  return env->intern(env, "nil");
}

static emacs_value native_cancel(emacs_env *env, ptrdiff_t n, emacs_value *args, void *data) {
  (void)n; (void)data;
  HWND hwnd = argument_hwnd(env, args[0]);
  State *s = find_state(hwnd);
  if (s && s->alive) SendMessageW(hwnd, control_message, (WPARAM)&control_cookie, CANCEL);
  return env->intern(env, "nil");
}

static emacs_value native_snapshot(emacs_env *env, ptrdiff_t n, emacs_value *args, void *data) {
  (void)n; (void)data;
  State *s = find_state(argument_hwnd(env, args[0]));
  if (!s) return env->intern(env, "nil");
  /* Copy under the lock; allocate Lisp values only after releasing it. */
  AcquireSRWLockShared(&s->lock);
  LONG units = s->units, cursor = s->cursor;
  uint64_t revision = s->revision;
  BOOL composing = s->composing && s->alive;
  int bytes = units ? WideCharToMultiByte(CP_UTF8, 0, s->text, units, NULL, 0, NULL, NULL) : 0;
  char *text = malloc((size_t)bytes + 1);
  BYTE *attrs = malloc((size_t)units + 1);
  if (text && attrs) {
    if (bytes) WideCharToMultiByte(CP_UTF8, 0, s->text, units, text, bytes, NULL, NULL);
    if (units) memcpy(attrs, s->attributes, (size_t)units);
  }
  ReleaseSRWLockShared(&s->lock);
  if (!text || !attrs) { free(text); free(attrs); return error(env, "Cannot allocate IME snapshot"); }
  emacs_value *values = malloc(((size_t)units + 1) * sizeof(*values));
  if (!values) { free(text); free(attrs); return error(env, "Cannot allocate IME attributes"); }
  for (LONG i = 0; i < units; ++i) values[i] = env->make_integer(env, attrs[i]);
  emacs_value result[5];
  result[0] = env->make_integer(env, (intmax_t)revision);
  result[1] = env->make_string(env, text, bytes);
  result[2] = env->make_integer(env, cursor);
  result[3] = env->funcall(env, env->intern(env, "vector"), units, values);
  result[4] = env->intern(env, composing ? "t" : "nil");
  free(values); free(text); free(attrs);
  return env->funcall(env, env->intern(env, "vector"), 5, result);
}

static void bind_function(emacs_env *env, const char *name, ptrdiff_t count,
                          emacs_value (*fn)(emacs_env *, ptrdiff_t, emacs_value *, void *),
                          const char *doc) {
  emacs_value args[] = {env->intern(env, name), env->make_function(env, count, count, fn, doc, NULL)};
  env->funcall(env, env->intern(env, "fset"), 2, args);
}

__declspec(dllexport) int emacs_module_init(struct emacs_runtime *runtime) {
  if (runtime->size < (ptrdiff_t)sizeof(*runtime)) return 1;
  emacs_env *env = runtime->get_environment(runtime);
  /* All methods used here are in the original Emacs 25 module ABI. */
  if (env->size < (ptrdiff_t)sizeof(struct emacs_env_25)) return 2;
  bind_function(env, "neo-ime-native-attach", 1, native_attach, "Attach to an Emacs HWND.");
  bind_function(env, "neo-ime-native-detach", 1, native_detach, "Restore native IME UI and detach HWND.");
  bind_function(env, "neo-ime-native-position", 4, native_position, "Set HWND's candidate anchor X Y HEIGHT.");
  bind_function(env, "neo-ime-native-cancel", 1, native_cancel, "Cancel composition in HWND.");
  bind_function(env, "neo-ime-native-snapshot", 1, native_snapshot,
                "Return [revision text utf16-cursor attributes composing] for HWND.");
  emacs_value feature = env->intern(env, "neo-ime-native");
  env->funcall(env, env->intern(env, "provide"), 1, &feature);
  return env->non_local_exit_check(env) == emacs_funcall_exit_return ? 0 : 3;
}
#endif
