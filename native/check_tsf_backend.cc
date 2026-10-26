// Copyright 2026 neo-ime contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "tsf_backend.h"

#include <cassert>
#include <cstdio>

int main() {
  auto* unrelated = new neo_ime::TsfBackend(nullptr);
  BOOL show = FALSE;
  assert(unrelated->BeginUIElement(0, &show) == S_OK);
  assert(show == FALSE);  // Another sink's decision must survive this callback.
  unrelated->Release();

  constexpr wchar_t kWindowClass[] = L"neo-ime-tsf-backend-test";
  WNDCLASSW window_class{};
  window_class.lpfnWndProc = DefWindowProcW;
  window_class.hInstance = GetModuleHandleW(nullptr);
  window_class.lpszClassName = kWindowClass;
  assert(RegisterClassW(&window_class));
  HWND window =
      CreateWindowW(kWindowClass, L"TSF backend test", WS_OVERLAPPED, 0, 0, 100,
                    100, nullptr, nullptr, window_class.hInstance, nullptr);
  assert(window);
  auto* backend = new neo_ime::TsfBackend(window);
  assert(SUCCEEDED(backend->Initialize()));
  neo_ime::ComPtr<ITfThreadMgrEx> manager;
  assert(SUCCEEDED(CoCreateInstance(CLSID_TF_ThreadMgr, nullptr,
                                    CLSCTX_INPROC_SERVER, IID_ITfThreadMgrEx,
                                    reinterpret_cast<void**>(manager.put()))));
  DWORD flags = 0;
  assert(SUCCEEDED(manager->GetActiveFlags(&flags)));
  assert(flags & TF_TMF_UIELEMENTENABLEDONLY);
  manager.Reset();
  backend->Shutdown();
  backend->Release();
  DestroyWindow(window);
  UnregisterClassW(kWindowClass, window_class.hInstance);
  std::puts(
      "NEO_IME_TSF_BACKEND=PASS UI-decision-preserved UI-less-activation");
}
