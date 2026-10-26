# TSF and IME control design

## Approved direction

The user selected the recommended session-sized TSF text store with IMM32
compatibility retained, followed by reconversion, explicit open/close control,
and conversion-mode query/control. Native code must use readable idiomatic
C++20, Google formatting, named constants, and explicit resource ownership.

## Approaches

A full-buffer TSF mirror would expose more context but requires synchronizing
every Lisp edit and large buffers. A session-sized text store exposes the input
or reconversion range and keeps integration bounded; this is the selected
approach. Using TSF only to supplement IMM32 would not provide a TSF input path
and is excluded.

## Boundaries

Only neo-ime files and its local test artifacts may change. No Emacs patches,
installed-copy changes, configuration changes, commits, or pushes. Existing
IMM32, candidate renderers, minibuffers, cursor feedback, and Undo stay usable.
Windows GUI Emacs with modules remains the platform. Existing MinGW is used.

## Ownership and input

TSF COM objects and text stores belong to the window's GUI thread. Lisp alone
edits Emacs buffers. GUI callbacks never call Emacs module APIs. Initialization,
focus association, key handling, cancellation, and release run on the owning
thread. Only one backend owns a composition. Fallback is selected outside an
active composition, not by replaying already-consumed keys.

The session identifies its buffer/range and generation. UTF-16 indices are
validated at boundaries. Pending confirmed edits cannot be discarded by a
new display snapshot. Conflicting Lisp edits invalidate the session rather
than applying stale edits to another buffer. TSF lock upgrades and reentrant
requests follow the documented ITextStoreACP contract.

## Features

TSF feeds preedit and candidate data to existing Lisp rendering. Unavailable
candidate interception retains native candidate UI. Reconversion uses the
selected text and checks the provider's supported range before beginning.
Cancellation restores original text; confirmation creates an undoable replace.
Unsupported operations report failure without deleting text.

Explicit Lisp commands expose open, close, toggle, and conversion-mode changes.
Open state and conversion flags remain separate. Supported Japanese mode names
are hiragana, full-width/half-width katakana, full-width/half-width alphanumeric.
Unrelated conversion flags are preserved; unknown is distinct from off.

## Verification

Test text-store locks, edits, selection/UTF-16 boundaries, pending results,
reconversion cancellation/conflicts, mode bit preservation, and lifecycle.
Run existing Lisp/native regressions and isolated GUI loading. Actual physical
IME input must be distinguished from synthetic COM/message tests. First prove
the unpatched Emacs key path and TSF activation before claiming live support.

## References

- https://learn.microsoft.com/en-us/windows/win32/api/textstor/nn-textstor-itextstoreacp
- https://learn.microsoft.com/en-us/windows/win32/tsf/document-locks
- https://learn.microsoft.com/en-us/windows/win32/api/ctffunc/nn-ctffunc-itffnreconversion
- https://learn.microsoft.com/en-us/windows/win32/tsf/predefined-compartments
