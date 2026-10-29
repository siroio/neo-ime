# Native DLL Update Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans and test-driven-development. The user has selected the approach and requested implementation. Work in the existing checkout; do not commit or create worktrees. Smart Brain is the progress record.

**Goal:** Activate DLL updates without restarting Emacs and remove obsolete copies on the next startup.

**Architecture:** Load a verified DLL copy addressed by its contents. Keep loaded copies for the current process. Wrap neo-ime's package-vc rebuild so old frame callbacks stop before Lisp/native replacement.

**Tech Stack:** Emacs Lisp, existing Windows Emacs dynamic module, ERT.

**Spec:** ../specs/2026-10-29-native-update-design.md

## Global Constraints

- Emacs 29.1+, Windows GUI; no additional dependencies or native rebuild.
- Only neo-ime is editable; isolate all test writes under neo-ime/var/.
- Preserve settings, active buffers, unrelated files, and other Emacs processes.

## Review Focus

- Another Emacs still has an old cache DLL open: cleanup must continue safely.
- Cache corruption or symlinks: reject loading and avoid deleting unrelated data.
- Composition or failed detach: never replace native callbacks underneath it.
- Rebuild or partial module initialization failure: never resume mixed bindings.
- Repeated package reloads and mode toggles: no duplicate advice or native load.

## Task 1: Native cache and startup cleanup

**Files:** neo-ime.el, check-ime-update.el.

**Interfaces:** `neo-ime--cache-native-file` returns a verified local path;
`neo-ime--load-native` validates/replaces native bindings;
`neo-ime--clean-native-cache` takes the current copy to preserve.

- [x] Add tests for content reuse/change, scoped cleanup, corrupt/locked copies,
  partial initialization rollback and actual DLL locking across processes.
- [x] Run the new checks; confirm failure before implementation.
- [x] Implement cache publication, loading, rollback and first-load cleanup.
- [x] Run the new checks and existing Lisp regression checks.

## Task 2: Live activation and documentation

**Files:** neo-ime.el, check-ime-update.el, check-ime-update-gui.el, README.md, build-ime.ps1.

**Interface:** `neo-ime--package-vc-rebuild` wraps the original rebuild and descriptor.

- [x] Add checks for enabled/disabled modes, composition and failed rebuilds.
- [x] Implement stop-before-rebuild and enable-after-success for neo-ime only.
- [x] Exercise real GUI frames, native switching and package activation with isolated settings.
- [x] Update the documented update procedure, cache policy and packaged checks.
- [x] Run scoped regressions, byte compilation, diff checks and independent review.
- [x] Record results and limitations in Smart Brain; leave changes uncommitted.

## Verification results

- Final Lisp suite: 45 ERT checks passed, plus the buffer/undo assertion script.
- Real DLL copies: live function replacement, loaded-old-copy retention and removal in a subsequent process passed.
- Dedicated GUI: real package-vc rebuild, two frames, TSF enabled, old callbacks detached, repeated rebuild and failed-load-disabled checks passed.
- Strict byte compilation with the runtime dependencies loaded, PowerShell parse and git diff checks passed.
- Independent review found demoted Lisp load errors, a linked cache ancestor and a publish/load cleanup race. All three were reproduced with failing tests and fixed; the startup failure race was treated as important because it disables IME. No findings were deferred.
- The Emacs 29/30 package-vc source has no `package-vc--checkout-dir`; use the older `package-vc--main-file` interface and test forwarding the legacy rebuild directory argument. The compatibility regression was reproduced and fixed. Runtime verification uses Emacs 31.1.
- Review did not rerun runtime checks; the implementer verified them afterward. Real typing/conversion, installed settings and release artifacts were not changed or tested.
