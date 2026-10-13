// Copyright 2026 neo-ime contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef NEO_IME_NATIVE_NEO_IME_NATIVE_H_
#define NEO_IME_NATIVE_NEO_IME_NATIVE_H_

#ifndef NEO_IME_TEST
#include <emacs-module.h>

extern "C" {
__declspec(dllexport) extern int plugin_is_GPL_compatible;
__declspec(dllexport) int emacs_module_init(emacs_runtime* runtime) noexcept;
}
#endif

#endif  // NEO_IME_NATIVE_NEO_IME_NATIVE_H_
