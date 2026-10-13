// Copyright 2026 neo-ime contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// No Emacs API calls on the GUI thread. Confirmed characters use Emacs's
// original WM_IME_CHAR path; Lisp polls independently owned snapshots.
#include "neo_ime_native.h"

#define UNICODE
#define _UNICODE
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#include <windows.h>

#include <commctrl.h>
#include <imm.h>

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <type_traits>
#include <utility>

#ifndef NEO_IME_TEST
extern "C" {
__declspec(dllexport) int plugin_is_GPL_compatible = 1;
}
#endif

namespace neo_ime {
namespace {

constexpr DWORD kMaximumPayloadBytes = 1024 * 1024;
constexpr UINT_PTR kSubclassId = 0x4e454f49;
#ifdef NEO_IME_TEST
bool g_fail_buffer_allocation = false;
#endif

struct FreeBuffer {
  void operator()(void* buffer) const noexcept { std::free(buffer); }
};

template <typename Element>
using OwnedBuffer = std::unique_ptr<Element[], FreeBuffer>;

template <typename Element>
OwnedBuffer<Element> AllocateBuffer(std::size_t count) noexcept {
  static_assert(std::is_trivially_copyable_v<Element>);
#ifdef NEO_IME_TEST
  if (g_fail_buffer_allocation) {
    return nullptr;
  }
#endif
  count = std::max(count, std::size_t{1});
  if (count > std::numeric_limits<std::size_t>::max() / sizeof(Element)) {
    return nullptr;
  }
  return OwnedBuffer<Element>(
      static_cast<Element*>(std::malloc(count * sizeof(Element))));
}

template <typename Element>
OwnedBuffer<Element> AllocateZeroedBuffer(std::size_t count) noexcept {
  auto buffer = AllocateBuffer<Element>(count);
  if (buffer) {
    std::memset(buffer.get(), 0,
                std::max(count, std::size_t{1}) * sizeof(Element));
  }
  return buffer;
}

class SharedLock {
 public:
  explicit SharedLock(SRWLOCK& lock) noexcept : lock_(lock) {
    AcquireSRWLockShared(&lock_);
  }
  ~SharedLock() { ReleaseSRWLockShared(&lock_); }
  SharedLock(const SharedLock&) = delete;
  SharedLock& operator=(const SharedLock&) = delete;

 private:
  SRWLOCK& lock_;
};

class ExclusiveLock {
 public:
  explicit ExclusiveLock(SRWLOCK& lock) noexcept : lock_(lock) {
    AcquireSRWLockExclusive(&lock_);
  }
  ~ExclusiveLock() { ReleaseSRWLockExclusive(&lock_); }
  ExclusiveLock(const ExclusiveLock&) = delete;
  ExclusiveLock& operator=(const ExclusiveLock&) = delete;

 private:
  SRWLOCK& lock_;
};

class ImeContext {
 public:
  explicit ImeContext(HWND window) noexcept
      : window_(window), context_(ImmGetContext(window)) {}
  ~ImeContext() {
    if (context_) {
      ImmReleaseContext(window_, context_);
    }
  }
  ImeContext(const ImeContext&) = delete;
  ImeContext& operator=(const ImeContext&) = delete;
  HIMC get() const noexcept { return context_; }

 private:
  HWND window_;
  HIMC context_;
};

class FormUpdate {
 public:
  explicit FormUpdate(bool& updating) noexcept
      : updating_(updating), previous_(updating) {
    updating_ = true;
  }
  ~FormUpdate() { updating_ = previous_; }
  FormUpdate(const FormUpdate&) = delete;
  FormUpdate& operator=(const FormUpdate&) = delete;

 private:
  bool& updating_;
  bool previous_;
};

// CANDIDATELIST's trailing dwOffset[1] is a variable-sized Win32 payload.
// Copy its header/offsets instead of indexing beyond that C++ array.
struct CandidateHeader {
  DWORD byte_size;
  DWORD style;
  DWORD candidate_count;
  DWORD selection;
  DWORD page_start;
  DWORD page_size;
};
static_assert(sizeof(CandidateHeader) == offsetof(CANDIDATELIST, dwOffset));

class CandidateList {
 public:
  CandidateList() = default;
  CandidateList(CandidateList&&) = default;
  CandidateList& operator=(CandidateList&&) = default;
  CandidateList(const CandidateList&) = delete;
  CandidateList& operator=(const CandidateList&) = delete;

  static CandidateList Read(HIMC context) noexcept {
    CandidateList candidates;
    if (!context) {
      return candidates;
    }
    const DWORD byte_size = ImmGetCandidateListW(context, 0, nullptr, 0);
    if (byte_size < sizeof(CandidateHeader) ||
        byte_size > kMaximumPayloadBytes) {
      return candidates;
    }
    candidates.bytes_ = AllocateBuffer<BYTE>(byte_size);
    if (!candidates.bytes_) {
      return candidates;
    }
    const DWORD actual_bytes = ImmGetCandidateListW(
        context, 0, reinterpret_cast<CANDIDATELIST*>(candidates.bytes_.get()),
        byte_size);
    if (!candidates.Validate(actual_bytes, byte_size)) {
      return {};
    }
    return candidates;
  }

  bool empty() const noexcept { return !bytes_; }
  const CandidateHeader& header() const noexcept { return header_; }

  const WCHAR* Text(DWORD candidate_index) const noexcept {
    return reinterpret_cast<const WCHAR*>(bytes_.get() +
                                          Offset(candidate_index));
  }

  bool CopyTo(CandidateList& destination) const noexcept {
    if (empty()) {
      destination = {};
      return true;
    }
    auto bytes = AllocateBuffer<BYTE>(header_.byte_size);
    if (!bytes) {
      return false;
    }
    std::memcpy(bytes.get(), bytes_.get(), header_.byte_size);
    destination.bytes_ = std::move(bytes);
    destination.header_ = header_;
    return true;
  }

 private:
  DWORD Offset(DWORD candidate_index) const noexcept {
    DWORD offset = 0;
    std::memcpy(&offset,
                bytes_.get() + sizeof(CandidateHeader) +
                    candidate_index * sizeof(DWORD),
                sizeof(offset));
    return offset;
  }

  bool Validate(DWORD actual_bytes, DWORD allocated_bytes) noexcept {
    if (actual_bytes < sizeof(CandidateHeader) ||
        actual_bytes > allocated_bytes) {
      return false;
    }
    std::memcpy(&header_, bytes_.get(), sizeof(header_));
    if (header_.byte_size != actual_bytes ||
        header_.candidate_count >
            (actual_bytes - sizeof(CandidateHeader)) / sizeof(DWORD)) {
      return false;
    }
    if (header_.page_start > header_.candidate_count) {
      return false;
    }
    if (header_.candidate_count != 0 &&
        (header_.selection >= header_.candidate_count ||
         header_.page_start >= header_.candidate_count)) {
      return false;
    }
    const std::size_t strings_start =
        sizeof(CandidateHeader) + header_.candidate_count * sizeof(DWORD);
    for (DWORD candidate_index = 0; candidate_index < header_.candidate_count;
         ++candidate_index) {
      const DWORD offset = Offset(candidate_index);
      if (offset < strings_start || offset >= actual_bytes ||
          offset % sizeof(WCHAR) != 0) {
        return false;
      }
      const std::size_t available_units =
          (actual_bytes - offset) / sizeof(WCHAR);
      if (!std::wmemchr(Text(candidate_index), L'\0', available_units)) {
        return false;
      }
    }
    return true;
  }

  OwnedBuffer<BYTE> bytes_;
  CandidateHeader header_{};
};

struct WindowState {
  explicit WindowState(HWND window_handle) noexcept : window(window_handle) {}

  HWND window;
  SRWLOCK lock = SRWLOCK_INIT;
  // Only GUI callbacks write this flag; Lisp can observe window destruction.
  std::atomic<bool> is_alive{true};
  bool is_composing = false;
  bool has_saved_composition_form = false;
  bool has_saved_candidate_form = false;
  bool use_native_fallback = false;
  bool updating_forms = false;
  COMPOSITIONFORM original_composition_form{};
  CANDIDATEFORM original_candidate_form{};
  std::uint64_t revision = 0;
  OwnedBuffer<WCHAR> composition_text;
  OwnedBuffer<BYTE> composition_attributes;
  LONG composition_units = 0;
  LONG cursor_units = 0;
  CandidateList candidates;
  POINT candidate_anchor{};
  int line_height = 20;
  WindowState* next = nullptr;
};

// Registry links are accessed only on the Lisp thread. Explicit synchronous
// detach must remove the GUI subclass before an owning state can be destroyed.
WindowState* g_window_states = nullptr;
UINT g_control_message = 0;
char g_control_cookie;
enum class ControlOperation : LPARAM {
  kAttach = 1,
  kDetach,
  kCancel,
  kPosition
};
struct AttachRequest {
  WindowState* state = nullptr;
  bool succeeded = false;
};
AttachRequest g_attach_request;

WindowState* FindState(HWND window) noexcept {
  for (WindowState* state = g_window_states; state; state = state->next) {
    if (state->window == window) {
      return state;
    }
  }
  return nullptr;
}

void ClearState(WindowState& state) noexcept {
  const ExclusiveLock lock(state.lock);
  state.composition_text.reset();
  state.composition_attributes.reset();
  state.candidates = {};
  state.composition_units = 0;
  state.cursor_units = 0;
  state.is_composing = false;
  ++state.revision;
}

void PositionIme(WindowState& state, HIMC context) noexcept {
  if (state.updating_forms) {
    return;
  }
  const FormUpdate update(state.updating_forms);
  POINT anchor;
  int line_height;
  {
    const SharedLock lock(state.lock);
    anchor = state.candidate_anchor;
    line_height = state.line_height;
  }
  // Google IME may ignore ISC_SHOWUICOMPOSITIONWINDOW. Keep the native
  // composition rectangle offscreen without overflowing its signed extents.
  COMPOSITIONFORM composition{};
  composition.dwStyle = CFS_RECT;
  composition.ptCurrentPos = anchor;
  composition.rcArea.left = -INT_MAX;
  composition.rcArea.top = -INT_MAX;
  ImmSetCompositionWindow(context, &composition);
  CANDIDATEFORM candidate{};
  candidate.dwStyle = CFS_EXCLUDE;
  candidate.ptCurrentPos = anchor;
  candidate.rcArea = {anchor.x, anchor.y, anchor.x + 1, anchor.y + line_height};
  ImmSetCandidateWindow(context, &candidate);
}

bool ReadComposition(WindowState& state, HIMC context) noexcept {
  const LONG byte_size =
      ImmGetCompositionStringW(context, GCS_COMPSTR, nullptr, 0);
  if (byte_size < 0 || byte_size > static_cast<LONG>(kMaximumPayloadBytes) ||
      byte_size % static_cast<LONG>(sizeof(WCHAR)) != 0) {
    return false;
  }
  const std::size_t capacity_units =
      static_cast<std::size_t>(byte_size) / sizeof(WCHAR);
  auto text = AllocateZeroedBuffer<WCHAR>(capacity_units + 1);
  auto attributes = AllocateZeroedBuffer<BYTE>(capacity_units + 1);
  if (!text || !attributes) {
    return false;
  }
  const LONG actual_bytes = ImmGetCompositionStringW(
      context, GCS_COMPSTR, text.get(), static_cast<DWORD>(byte_size));
  if (actual_bytes < 0 || actual_bytes > byte_size ||
      actual_bytes % static_cast<LONG>(sizeof(WCHAR)) != 0) {
    return false;
  }
  const LONG units = actual_bytes / static_cast<LONG>(sizeof(WCHAR));
  const LONG attribute_bytes = ImmGetCompositionStringW(
      context, GCS_COMPATTR, attributes.get(), static_cast<DWORD>(units));
  const bool empty_attributes =
      units == 0 && attribute_bytes == IMM_ERROR_NODATA;
  if ((attribute_bytes < 0 && !empty_attributes) || attribute_bytes > units) {
    return false;
  }
  const LONG cursor =
      std::clamp(ImmGetCompositionStringW(context, GCS_CURSORPOS, nullptr, 0),
                 LONG{0}, units);
  const ExclusiveLock lock(state.lock);
  state.composition_text = std::move(text);
  state.composition_attributes = std::move(attributes);
  state.composition_units = units;
  state.cursor_units = cursor;
  state.is_composing = true;
  ++state.revision;
  return true;
}

void ReadCandidates(WindowState& state, HIMC context) noexcept {
  CandidateList candidates = CandidateList::Read(context);
  const ExclusiveLock lock(state.lock);
  state.candidates = std::move(candidates);
  ++state.revision;
}

void RestoreIme(WindowState& state) noexcept {
  const ImeContext context(state.window);
  if (!context.get()) {
    return;
  }
  const FormUpdate update(state.updating_forms);
  if (state.has_saved_composition_form) {
    ImmSetCompositionWindow(context.get(), &state.original_composition_form);
  }
  if (state.has_saved_candidate_form) {
    ImmSetCandidateWindow(context.get(), &state.original_candidate_form);
  }
}

LRESULT CALLBACK ImeWindowProcedure(HWND window, UINT message, WPARAM parameter,
                                    LPARAM payload, UINT_PTR,
                                    DWORD_PTR reference);

LRESULT HandleControl(WindowState& state, ControlOperation operation) noexcept {
  if (operation == ControlOperation::kDetach) {
    RestoreIme(state);
    if (!RemoveWindowSubclass(state.window, ImeWindowProcedure, kSubclassId)) {
      return 0;
    }
    if (GetFocus() == state.window) {
      SendMessageW(state.window, WM_IME_SETCONTEXT, TRUE, ISC_SHOWUIALL);
    }
    return 1;
  }
  {
    const ImeContext context(state.window);
    if (context.get() && operation == ControlOperation::kCancel) {
      ImmNotifyIME(context.get(), NI_COMPOSITIONSTR, CPS_CANCEL, 0);
    }
    if (context.get() && operation == ControlOperation::kPosition &&
        state.is_composing) {
      PositionIme(state, context.get());
    }
  }
  if (operation == ControlOperation::kCancel) {
    ClearState(state);
  }
  return 1;
}

LRESULT QueryCharacterPosition(WindowState& state, LPARAM payload) noexcept {
  auto* position = reinterpret_cast<IMECHARPOSITION*>(payload);
  if (!position || position->dwSize < sizeof(*position)) {
    return 0;
  }
  {
    const SharedLock lock(state.lock);
    // One anchor for the clause; per-character pixel positions are not exposed.
    position->pt = state.candidate_anchor;
    position->cLineHeight = static_cast<UINT>(state.line_height);
  }
  if (!ClientToScreen(state.window, &position->pt) ||
      !GetClientRect(state.window, &position->rcDocument)) {
    return 0;
  }
  MapWindowPoints(state.window, nullptr,
                  reinterpret_cast<POINT*>(&position->rcDocument), 2);
  return 1;
}

LRESULT StartComposition(WindowState& state, WPARAM parameter,
                         LPARAM payload) noexcept {
  ClearState(state);
  state.use_native_fallback = false;
  {
    const ExclusiveLock lock(state.lock);
    state.is_composing = true;
  }
  const ImeContext context(state.window);
  if (!context.get()) {
    ClearState(state);
    state.use_native_fallback = true;
    return DefSubclassProc(state.window, WM_IME_STARTCOMPOSITION, parameter,
                           payload);
  }
  state.has_saved_composition_form =
      ImmGetCompositionWindow(context.get(), &state.original_composition_form);
  state.has_saved_candidate_form =
      ImmGetCandidateWindow(context.get(), 0, &state.original_candidate_form);
  PositionIme(state, context.get());
  // Let IMM initialize candidates without Emacs replacing the hidden form.
  return DefWindowProcW(state.window, WM_IME_STARTCOMPOSITION, parameter,
                        payload);
}

LRESULT UpdateComposition(WindowState& state, WPARAM parameter,
                          LPARAM payload) noexcept {
  if (state.use_native_fallback) {
    return DefSubclassProc(state.window, WM_IME_COMPOSITION, parameter,
                           payload);
  }
  bool valid = false;
  {
    const ImeContext context(state.window);
    valid = context.get() != nullptr;
    if (valid && (payload & (GCS_COMPSTR | GCS_COMPATTR | GCS_CURSORPOS))) {
      valid = ReadComposition(state, context.get());
    } else if (valid && (!payload || (payload & GCS_RESULTSTR))) {
      ClearState(state);
    }
    if (valid) {
      PositionIme(state, context.get());
    }
  }
  if (!valid) {
    ClearState(state);
    state.use_native_fallback = true;
    RestoreIme(state);
    DefSubclassProc(state.window, WM_IME_SETCONTEXT, TRUE, ISC_SHOWUIALL);
    return DefSubclassProc(state.window, WM_IME_COMPOSITION, parameter,
                           payload);
  }
  // A result may contain a new preedit clause. Keep it, and let Emacs commit.
  if (payload & GCS_RESULTSTR) {
    return DefSubclassProc(state.window, WM_IME_COMPOSITION, parameter,
                           payload);
  }
  return 0;
}

LRESULT CALLBACK ImeWindowProcedure(HWND window, UINT message, WPARAM parameter,
                                    LPARAM payload, UINT_PTR,
                                    DWORD_PTR reference) {
  auto& state = *reinterpret_cast<WindowState*>(reference);
  if (message == WM_IME_STARTCOMPOSITION &&
      (state.updating_forms ||
       (state.is_composing && !state.use_native_fallback))) {
    return 0;
  }
  if (message == g_control_message &&
      parameter == reinterpret_cast<WPARAM>(&g_control_cookie)) {
    return HandleControl(state, static_cast<ControlOperation>(payload));
  }
  if (message == WM_IME_SETCONTEXT && !state.use_native_fallback) {
    return DefSubclassProc(window, message, parameter,
                           payload & ~(ISC_SHOWUICOMPOSITIONWINDOW | 0x0f));
  }
  if (message == WM_IME_NOTIFY && !state.use_native_fallback) {
    if (parameter == IMN_OPENCANDIDATE || parameter == IMN_CHANGECANDIDATE) {
      const ImeContext context(window);
      ReadCandidates(state, context.get());
    } else if (parameter == IMN_CLOSECANDIDATE) {
      ReadCandidates(state, nullptr);
    }
  }
  if (message == WM_IME_REQUEST && parameter == IMR_QUERYCHARPOSITION &&
      state.is_composing && !state.use_native_fallback) {
    return QueryCharacterPosition(state, payload);
  }
  switch (message) {
    case WM_IME_STARTCOMPOSITION:
      return StartComposition(state, parameter, payload);
    case WM_IME_COMPOSITION:
      return UpdateComposition(state, parameter, payload);
    case WM_IME_ENDCOMPOSITION:
    case WM_KILLFOCUS:
      ClearState(state);
      RestoreIme(state);
      break;
    case WM_NCDESTROY:
      ClearState(state);
      RemoveWindowSubclass(window, ImeWindowProcedure, kSubclassId);
      // Publish only after the subclass is removed; do not use state afterward.
      state.is_alive.store(false, std::memory_order_release);
      break;
    default:
      break;
  }
  return DefSubclassProc(window, message, parameter, payload);
}

LRESULT CALLBACK AttachHook(int code, WPARAM parameter, LPARAM payload) {
  if (code < 0 || !g_attach_request.state) {
    return CallNextHookEx(nullptr, code, parameter, payload);
  }
  const auto& message = *reinterpret_cast<const CWPSTRUCT*>(payload);
  if (message.message != g_control_message ||
      message.wParam != reinterpret_cast<WPARAM>(&g_control_cookie) ||
      message.lParam != static_cast<LPARAM>(ControlOperation::kAttach) ||
      message.hwnd != g_attach_request.state->window) {
    return CallNextHookEx(nullptr, code, parameter, payload);
  }
  g_attach_request.succeeded =
      SetWindowSubclass(message.hwnd, ImeWindowProcedure, kSubclassId,
                        reinterpret_cast<DWORD_PTR>(g_attach_request.state));
  if (g_attach_request.succeeded && GetFocus() == message.hwnd) {
    SendMessageW(message.hwnd, WM_IME_SETCONTEXT, TRUE, ISC_SHOWUIALL);
  }
  return CallNextHookEx(nullptr, code, parameter, payload);
}

class WindowHook {
 public:
  explicit WindowHook(DWORD thread_id) noexcept
      : hook_(SetWindowsHookExW(WH_CALLWNDPROC, AttachHook, nullptr,
                                thread_id)) {}
  ~WindowHook() {
    if (hook_) {
      UnhookWindowsHookEx(hook_);
    }
  }
  WindowHook(const WindowHook&) = delete;
  WindowHook& operator=(const WindowHook&) = delete;
  bool installed() const noexcept { return hook_ != nullptr; }

 private:
  HHOOK hook_;
};

bool AttachWindow(HWND window) noexcept {
  DWORD process_id = 0;
  const DWORD thread_id = GetWindowThreadProcessId(window, &process_id);
  WCHAR class_name[32]{};
  if (!thread_id || process_id != GetCurrentProcessId() ||
      !GetClassNameW(window, class_name, 32) ||
      std::wcscmp(class_name, L"Emacs") != 0) {
    return false;
  }
  if (FindState(window)) {
    return true;
  }
  if (!g_control_message) {
    g_control_message = RegisterWindowMessageW(L"neo-ime-native-v1");
  }
  if (!g_control_message) {
    return false;
  }
  std::unique_ptr<WindowState> state(new (std::nothrow) WindowState(window));
  if (!state) {
    return false;
  }
  g_attach_request = {state.get(), false};
  {
    // Subclass installation must execute on the HWND's GUI thread.
    const WindowHook hook(thread_id);
    if (hook.installed()) {
      SendMessageW(window, g_control_message,
                   reinterpret_cast<WPARAM>(&g_control_cookie),
                   static_cast<LPARAM>(ControlOperation::kAttach));
    }
  }
  const bool attached = g_attach_request.succeeded;
  g_attach_request = {};
  if (!attached) {
    return false;
  }
  state->next = g_window_states;
  g_window_states = state.release();
  return true;
}

bool DetachWindow(HWND window) noexcept {
  WindowState** link = &g_window_states;
  while (*link && (*link)->window != window) {
    link = &(*link)->next;
  }
  if (!*link) {
    return true;
  }
  WindowState& state = **link;
  if (state.is_alive.load(std::memory_order_acquire) &&
      !SendMessageW(window, g_control_message,
                    reinterpret_cast<WPARAM>(&g_control_cookie),
                    static_cast<LPARAM>(ControlOperation::kDetach))) {
    return false;
  }
  std::unique_ptr<WindowState> removed(*link);
  *link = removed->next;
  return true;
}

#ifndef NEO_IME_TEST
emacs_value SignalError(emacs_env* environment, const char* message) noexcept {
  if (environment->non_local_exit_check(environment) !=
      emacs_funcall_exit_return) {
    return nullptr;
  }
  emacs_value text = environment->make_string(
      environment, message, static_cast<ptrdiff_t>(std::strlen(message)));
  emacs_value arguments = environment->funcall(
      environment, environment->intern(environment, "list"), 1, &text);
  environment->non_local_exit_signal(
      environment, environment->intern(environment, "error"), arguments);
  return nullptr;
}

HWND WindowArgument(emacs_env* environment, emacs_value argument) noexcept {
  const intmax_t handle = environment->extract_integer(environment, argument);
  if (environment->non_local_exit_check(environment) !=
      emacs_funcall_exit_return) {
    return nullptr;
  }
  return reinterpret_cast<HWND>(static_cast<std::uintptr_t>(handle));
}

emacs_value NativeAttach(emacs_env* environment, ptrdiff_t,
                         emacs_value* arguments, void*) noexcept {
  const HWND window = WindowArgument(environment, arguments[0]);
  if (!window || !AttachWindow(window)) {
    return SignalError(environment,
                       "Cannot attach neo-ime to this Emacs GUI frame");
  }
  return environment->intern(environment, "t");
}

emacs_value NativeDetach(emacs_env* environment, ptrdiff_t,
                         emacs_value* arguments, void*) noexcept {
  const HWND window = WindowArgument(environment, arguments[0]);
  if (environment->non_local_exit_check(environment) !=
      emacs_funcall_exit_return) {
    return nullptr;
  }
  if (!DetachWindow(window)) {
    return SignalError(environment, "Cannot detach neo-ime from frame");
  }
  return environment->intern(environment, "t");
}

emacs_value NativePosition(emacs_env* environment, ptrdiff_t,
                           emacs_value* arguments, void*) noexcept {
  const HWND window = WindowArgument(environment, arguments[0]);
  const intmax_t anchor_x =
      environment->extract_integer(environment, arguments[1]);
  const intmax_t anchor_y =
      environment->extract_integer(environment, arguments[2]);
  const intmax_t line_height =
      environment->extract_integer(environment, arguments[3]);
  if (environment->non_local_exit_check(environment) !=
      emacs_funcall_exit_return) {
    return nullptr;
  }
  if (anchor_x < 0 || anchor_x >= INT_MAX || anchor_y < 0 ||
      anchor_y > INT_MAX || line_height < 1 || line_height > INT_MAX ||
      anchor_y > INT_MAX - line_height) {
    return SignalError(environment, "Invalid neo-ime candidate coordinates");
  }
  WindowState* state = FindState(window);
  if (!state) {
    return environment->intern(environment, "nil");
  }
  {
    const ExclusiveLock lock(state->lock);
    state->candidate_anchor = {static_cast<LONG>(anchor_x),
                               static_cast<LONG>(anchor_y)};
    state->line_height = static_cast<int>(line_height);
  }
  if (state->is_alive.load(std::memory_order_acquire)) {
    SendMessageW(window, g_control_message,
                 reinterpret_cast<WPARAM>(&g_control_cookie),
                 static_cast<LPARAM>(ControlOperation::kPosition));
  }
  return environment->intern(environment, "nil");
}

emacs_value NativeCancel(emacs_env* environment, ptrdiff_t,
                         emacs_value* arguments, void*) noexcept {
  const HWND window = WindowArgument(environment, arguments[0]);
  if (environment->non_local_exit_check(environment) !=
      emacs_funcall_exit_return) {
    return nullptr;
  }
  WindowState* state = FindState(window);
  if (state && state->is_alive.load(std::memory_order_acquire)) {
    SendMessageW(window, g_control_message,
                 reinterpret_cast<WPARAM>(&g_control_cookie),
                 static_cast<LPARAM>(ControlOperation::kCancel));
  }
  return environment->intern(environment, "nil");
}

struct Snapshot {
  std::uint64_t revision = 0;
  LONG units = 0;
  LONG cursor_units = 0;
  int utf8_bytes = 0;
  bool is_composing = false;
  OwnedBuffer<char> utf8_text;
  OwnedBuffer<BYTE> attributes;
  CandidateList candidates;
};

bool CopySnapshot(const WindowState& state, Snapshot& snapshot) noexcept {
  snapshot.revision = state.revision;
  snapshot.units = state.composition_units;
  snapshot.cursor_units = state.cursor_units;
  snapshot.is_composing =
      state.is_composing && state.is_alive.load(std::memory_order_acquire);
  snapshot.utf8_bytes =
      snapshot.units == 0
          ? 0
          : WideCharToMultiByte(CP_UTF8, 0, state.composition_text.get(),
                                snapshot.units, nullptr, 0, nullptr, nullptr);
  if (snapshot.units != 0 && snapshot.utf8_bytes == 0) {
    return false;
  }
  snapshot.utf8_text =
      AllocateBuffer<char>(static_cast<std::size_t>(snapshot.utf8_bytes) + 1);
  snapshot.attributes =
      AllocateBuffer<BYTE>(static_cast<std::size_t>(snapshot.units) + 1);
  if (!snapshot.utf8_text || !snapshot.attributes ||
      !state.candidates.CopyTo(snapshot.candidates)) {
    return false;
  }
  if (snapshot.units == 0) {
    return true;
  }
  if (WideCharToMultiByte(CP_UTF8, 0, state.composition_text.get(),
                          snapshot.units, snapshot.utf8_text.get(),
                          snapshot.utf8_bytes, nullptr,
                          nullptr) != snapshot.utf8_bytes) {
    return false;
  }
  std::memcpy(snapshot.attributes.get(), state.composition_attributes.get(),
              static_cast<std::size_t>(snapshot.units));
  return true;
}

emacs_value MakeCandidates(emacs_env* environment,
                           const CandidateList& candidates) noexcept {
  const CandidateHeader& header = candidates.header();
  const DWORD remaining = header.candidate_count - header.page_start;
  const DWORD page_size = header.page_size == 0 ? 9 : header.page_size;
  const DWORD count = std::min(remaining, page_size);
  auto values = AllocateBuffer<emacs_value>(count);
  if (!values) {
    return SignalError(environment, "Cannot allocate IME candidates");
  }
  for (DWORD index = 0; index < count; ++index) {
    const WCHAR* text = candidates.Text(header.page_start + index);
    const int length = static_cast<int>(std::wcslen(text));
    const int byte_size = WideCharToMultiByte(CP_UTF8, 0, text, length, nullptr,
                                              0, nullptr, nullptr);
    if (length != 0 && byte_size == 0) {
      return SignalError(environment, "Cannot convert IME candidate text");
    }
    auto utf8_text =
        AllocateBuffer<char>(static_cast<std::size_t>(byte_size) + 1);
    if (!utf8_text) {
      return SignalError(environment, "Cannot allocate candidate text");
    }
    if (byte_size != 0 &&
        WideCharToMultiByte(CP_UTF8, 0, text, length, utf8_text.get(),
                            byte_size, nullptr, nullptr) != byte_size) {
      return SignalError(environment, "Cannot convert IME candidate text");
    }
    values[index] =
        environment->make_string(environment, utf8_text.get(), byte_size);
    if (environment->non_local_exit_check(environment) !=
        emacs_funcall_exit_return) {
      return nullptr;
    }
  }
  return environment->funcall(environment,
                              environment->intern(environment, "vector"), count,
                              values.get());
}

emacs_value MakeSnapshot(emacs_env* environment,
                         const Snapshot& snapshot) noexcept {
  const auto attribute_count = static_cast<std::size_t>(snapshot.units);
  auto attributes = AllocateBuffer<emacs_value>(attribute_count);
  if (!attributes) {
    return SignalError(environment, "Cannot allocate IME attributes");
  }
  for (std::size_t index = 0; index < attribute_count; ++index) {
    attributes[index] =
        environment->make_integer(environment, snapshot.attributes[index]);
    if (environment->non_local_exit_check(environment) !=
        emacs_funcall_exit_return) {
      return nullptr;
    }
  }
  emacs_value result[9];
  result[0] = environment->make_integer(
      environment, static_cast<intmax_t>(snapshot.revision));
  result[1] = environment->make_string(environment, snapshot.utf8_text.get(),
                                       snapshot.utf8_bytes);
  result[2] = environment->make_integer(environment, snapshot.cursor_units);
  result[3] = environment->funcall(environment,
                                   environment->intern(environment, "vector"),
                                   snapshot.units, attributes.get());
  result[4] =
      environment->intern(environment, snapshot.is_composing ? "t" : "nil");
  result[5] = MakeCandidates(environment, snapshot.candidates);
  if (environment->non_local_exit_check(environment) !=
      emacs_funcall_exit_return) {
    return nullptr;
  }
  const CandidateHeader& header = snapshot.candidates.header();
  result[6] = environment->make_integer(environment, header.selection);
  result[7] = environment->make_integer(environment, header.page_start);
  result[8] = environment->make_integer(environment, header.candidate_count);
  return environment->funcall(
      environment, environment->intern(environment, "vector"), 9, result);
}

emacs_value NativeSnapshot(emacs_env* environment, ptrdiff_t argument_count,
                           emacs_value* arguments, void*) noexcept {
  const HWND window = WindowArgument(environment, arguments[0]);
  if (environment->non_local_exit_check(environment) !=
      emacs_funcall_exit_return) {
    return nullptr;
  }
  WindowState* state = FindState(window);
  if (!state) {
    return environment->intern(environment, "nil");
  }
  intmax_t previous_revision = -1;
  if (argument_count > 1 &&
      environment->is_not_nil(environment, arguments[1])) {
    previous_revision = environment->extract_integer(environment, arguments[1]);
    if (environment->non_local_exit_check(environment) !=
        emacs_funcall_exit_return) {
      return nullptr;
    }
  }
  std::optional<Snapshot> snapshot;
  bool copied = true;
  bool unchanged;
  {
    const SharedLock lock(state->lock);
    unchanged =
        previous_revision >= 0 &&
        static_cast<std::uint64_t>(previous_revision) == state->revision;
    if (!unchanged) {
      snapshot.emplace();
      copied = CopySnapshot(*state, *snapshot);
    }
  }
  if (unchanged) {
    // No buffers or Lisp values are allocated on the unchanged hot path.
    return environment->intern(environment, "nil");
  }
  if (!copied) {
    return SignalError(environment, "Cannot allocate or convert IME snapshot");
  }
  return MakeSnapshot(environment, *snapshot);
}

using NativeFunction = emacs_function;
void BindFunction(emacs_env* environment, const char* name, ptrdiff_t minimum,
                  ptrdiff_t maximum, NativeFunction function,
                  const char* documentation) noexcept {
  emacs_value arguments[] = {
      environment->intern(environment, name),
      environment->make_function(environment, minimum, maximum, function,
                                 documentation, nullptr)};
  environment->funcall(environment, environment->intern(environment, "fset"), 2,
                       arguments);
}
#endif

}  // namespace
}  // namespace neo_ime

#ifndef NEO_IME_TEST
extern "C" __declspec(dllexport) int emacs_module_init(
    emacs_runtime* runtime) noexcept {
  if (runtime->size < static_cast<ptrdiff_t>(sizeof(*runtime))) {
    return 1;
  }
  emacs_env* environment = runtime->get_environment(runtime);
  if (!environment ||
      environment->size < static_cast<ptrdiff_t>(sizeof(emacs_env_25))) {
    return 2;
  }
  neo_ime::BindFunction(environment, "neo-ime-native-attach", 1, 1,
                        neo_ime::NativeAttach, "Attach to an Emacs HWND.");
  neo_ime::BindFunction(environment, "neo-ime-native-detach", 1, 1,
                        neo_ime::NativeDetach,
                        "Restore native IME UI and detach HWND.");
  neo_ime::BindFunction(environment, "neo-ime-native-position", 4, 4,
                        neo_ime::NativePosition,
                        "Set HWND's candidate anchor X Y HEIGHT.");
  neo_ime::BindFunction(environment, "neo-ime-native-cancel", 1, 1,
                        neo_ime::NativeCancel, "Cancel composition in HWND.");
  neo_ime::BindFunction(
      environment, "neo-ime-native-snapshot", 1, 2, neo_ime::NativeSnapshot,
      "Return [revision text utf16-cursor attributes composing "
      "candidates selection page-start total] for HWND.\n"
      "With optional REVISION, return nil if unchanged.");
  emacs_value feature = environment->intern(environment, "neo-ime-native");
  environment->funcall(environment, environment->intern(environment, "provide"),
                       1, &feature);
  return environment->non_local_exit_check(environment) ==
                 emacs_funcall_exit_return
             ? 0
             : 3;
}
#endif
