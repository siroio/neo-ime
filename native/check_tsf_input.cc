// Copyright 2026 neo-ime contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Sends input only while the explicitly identified isolated test Emacs is
// focused.
#define UNICODE
#define _UNICODE
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <iterator>

int main(int count, char** arguments) {
  constexpr int kArgumentCount = 3;
  constexpr DWORD kKeyIntervalMs = 60;
  constexpr DWORD kConversionWaitMs = 300;
  constexpr SHORT kKeyPressed = static_cast<SHORT>(0x8000);
  constexpr std::size_t kWindowClassCapacity = 32;
  if (count != kArgumentCount) return EXIT_FAILURE;
  HWND window =
      reinterpret_cast<HWND>(std::strtoull(arguments[1], nullptr, 10));
  const DWORD expected_process = std::strtoul(arguments[2], nullptr, 10);
  DWORD process = 0;
  WCHAR name[kWindowClassCapacity]{};
  if (!GetWindowThreadProcessId(window, &process) ||
      process != expected_process ||
      !GetClassNameW(window, name, static_cast<int>(std::size(name))) ||
      std::wcscmp(name, L"Emacs") != 0)
    return EXIT_FAILURE;
  ShowWindow(window, SW_SHOWNORMAL);
  SetForegroundWindow(window);
  Sleep(kConversionWaitMs);
  std::printf("target=%p foreground=%p\n", static_cast<void*>(window),
              static_cast<void*>(GetForegroundWindow()));
  const auto send = [&](WPARAM key) {
    if (GetForegroundWindow() != window ||
        (GetAsyncKeyState(VK_CONTROL) & kKeyPressed) ||
        (GetAsyncKeyState(VK_MENU) & kKeyPressed) ||
        (GetAsyncKeyState(VK_SHIFT) & kKeyPressed))
      return false;
    INPUT input[2]{};
    input[0].type = input[1].type = INPUT_KEYBOARD;
    input[0].ki.wScan = input[1].ki.wScan = static_cast<WORD>(
        MapVirtualKeyW(static_cast<UINT>(key), MAPVK_VK_TO_VSC));
    input[0].ki.dwFlags = KEYEVENTF_SCANCODE;
    input[1].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
    if (SendInput(static_cast<UINT>(std::size(input)), input, sizeof(INPUT)) !=
        std::size(input))
      return false;
    Sleep(kKeyIntervalMs);
    return true;
  };
  for (const char key : "NIHONGO")
    if (key && !send(static_cast<WPARAM>(key))) return EXIT_FAILURE;
  if (!send(VK_SPACE)) return EXIT_FAILURE;
  Sleep(kConversionWaitMs);
  if (!send(VK_RETURN)) return EXIT_FAILURE;
  std::puts("NEO_IME_TSF_INPUT=SENT isolated-window NIHONGO space enter");
}
