// Copyright 2026 neo-ime contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#define NEO_IME_TEST
#include <windows.h>

#include <imm.h>

#include <cassert>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {
bool g_fail_composition = false;
bool g_empty_composition = false;
int g_invalid_attributes = 0;
HWND g_test_window = nullptr;
bool g_setting_position = false;
unsigned int g_default_start_count = 0;
int g_invalid_candidate = 0;
HANDLE g_window_ready = nullptr;
bool g_ime_open = false;
bool g_no_context = false;

HIMC FakeGetContext(HWND window) {
  return g_no_context ? nullptr : ImmGetContext(window);
}

BOOL FakeOpenStatus(HIMC) {
  assert(GetCurrentThreadId() ==
         GetWindowThreadProcessId(g_test_window, nullptr));
  return g_ime_open ? TRUE : FALSE;
}

DWORD FakeCandidates(HIMC, DWORD, CANDIDATELIST* output, DWORD) {
  struct CandidatePayload {
    DWORD byte_size;
    DWORD style;
    DWORD count;
    DWORD selection;
    DWORD page_start;
    DWORD page_size;
    DWORD offsets[2];
    WCHAR text[5];
  } payload = {
      0, IME_CAND_READ, 2, 1, 1, 1, {32, 36}, {0x4e9c, 0, 0xd83d, 0xde00, 0}};
  payload.byte_size = sizeof(payload);
  switch (g_invalid_candidate) {
    case 1:
      payload.offsets[0] = 1;
      break;
    case 2: {
      payload.offsets[1] = sizeof(payload) - 2;
      const WCHAR unterminated = 1;
      std::memcpy(reinterpret_cast<BYTE*>(&payload) + sizeof(payload) - 2,
                  &unterminated, sizeof(unterminated));
      break;
    }
    case 3:
      payload.count = 999;
      break;
    case 4:
      payload.count = 0;
      payload.page_start = 1;
      break;
    default:
      break;
  }
  if (output) {
    std::memcpy(output, &payload, sizeof(payload));
  }
  return sizeof(payload);
}

LRESULT CALLBACK FakeDefault(HWND window, UINT message, WPARAM parameter,
                             LPARAM payload) {
  if (message == WM_IME_STARTCOMPOSITION) {
    ++g_default_start_count;
  }
  return DefWindowProcW(window, message, parameter, payload);
}

BOOL FakeSetComposition(HIMC, COMPOSITIONFORM* form) {
  if (form->dwStyle == CFS_RECT) {
    assert(static_cast<std::int64_t>(form->rcArea.right) - form->rcArea.left <=
           INT_MAX);
    assert(static_cast<std::int64_t>(form->rcArea.bottom) - form->rcArea.top <=
           INT_MAX);
  }
  // Google IME can synchronously restart composition while setting its form.
  assert(!g_setting_position);
  g_setting_position = true;
  SendMessageW(g_test_window, WM_IME_STARTCOMPOSITION, 0, 0);
  g_setting_position = false;
  return TRUE;
}

LONG FakeComposition(HIMC, DWORD kind, LPVOID output, DWORD byte_capacity) {
  static constexpr WCHAR kText[] = {0x3042, 0xd83d, 0xde00, 0x3044};
  static constexpr BYTE kAttributes[] = {0, 1, 1, 0};
  if (g_fail_composition) {
    return IMM_ERROR_GENERAL;
  }
  if (g_empty_composition && kind == GCS_COMPSTR) {
    return 0;
  }
  if (g_empty_composition && kind == GCS_COMPATTR) {
    return IMM_ERROR_NODATA;
  }
  if (kind == GCS_COMPATTR && g_invalid_attributes) {
    return g_invalid_attributes == 1 ? IMM_ERROR_GENERAL
                                     : static_cast<LONG>(byte_capacity) + 1;
  }
  if (kind == GCS_CURSORPOS) {
    return 3;
  }
  const void* data = kind == GCS_COMPSTR
                         ? static_cast<const void*>(kText)
                         : static_cast<const void*>(kAttributes);
  const DWORD byte_size =
      kind == GCS_COMPSTR ? sizeof(kText) : sizeof(kAttributes);
  if (output) {
    std::memcpy(output, data,
                byte_capacity < byte_size ? byte_capacity : byte_size);
  }
  return static_cast<LONG>(byte_size);
}
}  // namespace

// Compile the implementation with deterministic IMM responses. Window threads,
// subclass dispatch, destruction and the standard fallback remain real Win32.
#define ImmGetCompositionStringW FakeComposition
#define ImmGetCandidateListW FakeCandidates
#define ImmSetCompositionWindow FakeSetComposition
#define ImmGetContext FakeGetContext
#define ImmGetOpenStatus FakeOpenStatus
#define DefWindowProcW FakeDefault
#include "neo_ime_native.cc"

namespace {
DWORD WINAPI WindowThread(void*) {
  WNDCLASSW window_class{};
  window_class.lpfnWndProc = DefWindowProcW;
  window_class.hInstance = GetModuleHandleW(nullptr);
  window_class.lpszClassName = L"Emacs";
  assert(RegisterClassW(&window_class));
  g_test_window =
      CreateWindowW(L"Emacs", L"neo-ime check", WS_OVERLAPPED, 0, 0, 320, 200,
                    nullptr, nullptr, window_class.hInstance, nullptr);
  assert(g_test_window);
  const HIMC context = ImmCreateContext();
  assert(context);
  const HIMC previous_context = ImmAssociateContext(g_test_window, context);
  SetEvent(g_window_ready);
  MSG message;
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    DispatchMessageW(&message);
  }
  ImmAssociateContext(g_test_window, previous_context);
  ImmDestroyContext(context);
  DestroyWindow(g_test_window);
  return 0;
}

void CheckCandidates(neo_ime::WindowState& state) {
  SendMessageW(g_test_window, WM_IME_NOTIFY, IMN_CHANGECANDIDATE, 1);
  assert(!state.candidates.empty());
  assert(state.candidates.header().candidate_count == 2);
  assert(state.candidates.header().selection == 1);
  assert(state.candidates.header().page_start == 1);
  assert(state.candidates.Text(1)[0] == 0xd83d);
  neo_ime::CandidateList copy;
  assert(state.candidates.CopyTo(copy));
  assert(copy.Text(1)[0] == 0xd83d);
  neo_ime::g_fail_buffer_allocation = true;
  assert(!state.candidates.CopyTo(copy));
  neo_ime::g_fail_buffer_allocation = false;
  assert(copy.Text(1)[0] == 0xd83d);
  for (g_invalid_candidate = 1; g_invalid_candidate <= 4;
       ++g_invalid_candidate) {
    SendMessageW(g_test_window, WM_IME_NOTIFY, IMN_CHANGECANDIDATE, 1);
    assert(state.candidates.empty());
  }
  g_invalid_candidate = 0;
  SendMessageW(g_test_window, WM_IME_NOTIFY, IMN_OPENCANDIDATE, 1);
  assert(!state.candidates.empty());
  SendMessageW(g_test_window, WM_IME_NOTIFY, IMN_CLOSECANDIDATE, 1);
  assert(state.candidates.empty() && state.is_composing);
}

void CheckCharacterPosition(neo_ime::WindowState& state) {
  state.candidate_anchor = {25, 40};
  state.line_height = 24;
  IMECHARPOSITION position{};
  position.dwSize = sizeof(position);
  position.dwCharPos = 1;
  POINT expected = state.candidate_anchor;
  assert(ClientToScreen(g_test_window, &expected));
  assert(SendMessageW(g_test_window, WM_IME_REQUEST, IMR_QUERYCHARPOSITION,
                      reinterpret_cast<LPARAM>(&position)));
  assert(position.pt.x == expected.x && position.pt.y == expected.y);
  assert(position.cLineHeight == 24);
  assert(position.rcDocument.left <= position.pt.x &&
         position.rcDocument.right > position.pt.x);
  assert(position.dwCharPos == 1);
  position.dwSize = 1;
  assert(!SendMessageW(g_test_window, WM_IME_REQUEST, IMR_QUERYCHARPOSITION,
                       reinterpret_cast<LPARAM>(&position)));
}

void CheckComposition(neo_ime::WindowState& state) {
  SendMessageW(g_test_window, WM_IME_STARTCOMPOSITION, 0, 0);
  assert(state.is_composing);
  assert(g_default_start_count == 1);
  SendMessageW(g_test_window, WM_IME_COMPOSITION, 0,
               GCS_COMPSTR | GCS_CURSORPOS | GCS_COMPATTR);
  assert(state.composition_units == 4 && state.cursor_units == 3 &&
         state.is_composing);
  assert(state.composition_text[1] == 0xd83d &&
         state.composition_attributes[1] == 1);
  g_empty_composition = true;
  SendMessageW(g_test_window, WM_IME_COMPOSITION, 0, GCS_COMPSTR);
  assert(state.is_composing && !state.use_native_fallback &&
         state.composition_units == 0);
  g_empty_composition = false;
  SendMessageW(g_test_window, WM_IME_COMPOSITION, 0, GCS_COMPSTR);
  CheckCandidates(state);
  CheckCharacterPosition(state);
  const std::uint64_t revision = state.revision;
  SendMessageW(g_test_window, WM_IME_STARTCOMPOSITION, 0, 0);
  assert(state.revision == revision && state.composition_units == 4);
  SendMessageW(g_test_window, WM_IME_COMPOSITION, 0,
               GCS_RESULTSTR | GCS_COMPSTR);
  assert(state.composition_units == 4 && state.is_composing);
  SendMessageW(g_test_window, WM_IME_COMPOSITION, 0, GCS_RESULTSTR);
  assert(!state.is_composing && state.composition_units == 0);
  SendMessageW(g_test_window, WM_IME_STARTCOMPOSITION, 0, 0);
  g_fail_composition = true;
  SendMessageW(g_test_window, WM_IME_COMPOSITION, 0, GCS_COMPSTR);
  assert(state.use_native_fallback && !state.is_composing);
  g_fail_composition = false;
  SendMessageW(g_test_window, WM_IME_STARTCOMPOSITION, 0, 0);
  assert(!state.use_native_fallback);
  for (g_invalid_attributes = 1; g_invalid_attributes <= 2;
       ++g_invalid_attributes) {
    SendMessageW(g_test_window, WM_IME_COMPOSITION, 0,
                 GCS_COMPSTR | GCS_COMPATTR);
    assert(state.use_native_fallback && !state.is_composing);
    SendMessageW(g_test_window, WM_IME_STARTCOMPOSITION, 0, 0);
  }
  g_invalid_attributes = 0;
  neo_ime::g_fail_buffer_allocation = true;
  SendMessageW(g_test_window, WM_IME_COMPOSITION, 0, GCS_COMPSTR);
  assert(state.use_native_fallback && !state.is_composing);
  neo_ime::g_fail_buffer_allocation = false;
  SendMessageW(g_test_window, WM_IME_STARTCOMPOSITION, 0, 0);
  SendMessageW(g_test_window, WM_KILLFOCUS, 0, 0);
  assert(!state.is_composing);
}

void CheckOpenStatus(neo_ime::WindowState& state) {
  assert(neo_ime::ReadOpenStatus(g_test_window) == 0);
  const std::uint64_t revision = state.revision;
  g_ime_open = true;
  SendMessageW(g_test_window, WM_IME_NOTIFY, IMN_SETOPENSTATUS, 0);
  assert(neo_ime::ReadOpenStatus(g_test_window) == 1);
  assert(state.revision == revision);  // Status does not rebuild composition.
  state.use_native_fallback = true;
  g_ime_open = false;
  SendMessageW(g_test_window, WM_IME_NOTIFY, IMN_SETOPENSTATUS, 0);
  assert(neo_ime::ReadOpenStatus(g_test_window) == 0);
  g_no_context = true;
  SendMessageW(g_test_window, WM_SETFOCUS, 0, 0);
  assert(neo_ime::ReadOpenStatus(g_test_window) == -1);
  g_no_context = false;
  g_ime_open = true;
  SendMessageW(g_test_window, WM_IME_SETCONTEXT, TRUE, ISC_SHOWUIALL);
  assert(neo_ime::ReadOpenStatus(g_test_window) == 1);
  state.use_native_fallback = false;
  g_ime_open = false;
}
}  // namespace

int main() {
  assert(!neo_ime::AllocateBuffer<WCHAR>(SIZE_MAX));
  g_window_ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  DWORD thread_id;
  HANDLE thread =
      CreateThread(nullptr, 0, WindowThread, nullptr, 0, &thread_id);
  assert(thread && WaitForSingleObject(g_window_ready, 3000) == WAIT_OBJECT_0);
  assert(!neo_ime::AttachWindow(nullptr));
  assert(neo_ime::AttachWindow(g_test_window));
  assert(neo_ime::AttachWindow(g_test_window));
  neo_ime::WindowState* state = neo_ime::FindState(g_test_window);
  assert(state && state->is_alive);
  CheckOpenStatus(*state);
  CheckComposition(*state);
  assert(neo_ime::DetachWindow(g_test_window));
  assert(!neo_ime::FindState(g_test_window));
  assert(neo_ime::ReadOpenStatus(g_test_window) == -1);
  assert(neo_ime::DetachWindow(g_test_window));
  assert(neo_ime::AttachWindow(g_test_window));
  PostThreadMessageW(thread_id, WM_QUIT, 0, 0);
  assert(WaitForSingleObject(thread, 3000) == WAIT_OBJECT_0);
  assert(!neo_ime::FindState(g_test_window)->is_alive);
  assert(neo_ime::ReadOpenStatus(g_test_window) == -1);
  assert(neo_ime::DetachWindow(g_test_window));
  CloseHandle(thread);
  CloseHandle(g_window_ready);
  std::puts(
      "NEO_IME_NATIVE=PASS cross-thread lifecycle synthetic-IMM utf16 "
      "partial-commit safe-fallback invalid-attributes");
}
