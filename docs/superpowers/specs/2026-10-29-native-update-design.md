# DLL updates without restarting Emacs

The user selected loading a new DLL under a different name, retaining old loaded
DLLs until Emacs exits, and deleting obsolete copies on the next startup.

## Approaches

- Copy only and restart to activate: fixes package-file locking but does not meet
  the requested live activation.
- Load distinct copies and clean up at the next startup: selected; old module
  code stays mapped until process exit.
- Permanent bridge plus unloadable backend: unnecessary for the selected
  cleanup policy and requires changing the native interface and build.

## Behavior and boundaries

- Read the trusted `neo-ime-native-file`; load a content-addressed copy under
  `user-emacs-directory/neo-ime/native/`, outside the package checkout.
- Publish complete cache files atomically and validate existing copies. Never
  overwrite a loaded cache DLL or follow a cache-file symlink.
- After the first successful native load in a process, delete only recognized
  obsolete cache DLLs. Preserve the current copy, directories, unrelated files,
  symlinks, and files Windows refuses to delete because they are still in use.
- Preserve old copies during live updates. Failed native initialization restores
  previous native function bindings and leaves the mode disabled.
- Stop the old mode before `package-vc--unpack-1` reloads Lisp, then enable the
  updated mode. Reject updates during composition. A failed rebuild must not
  enable a mixed Lisp/native version. Disabled modes remain disabled.
- All frames must detach before another DLL is loaded. Do not force-unload DLLs.
- The first migration from directly loading the distributed DLL can require one
  restart: that original module cannot be unloaded by this change.

## Scope and verification

Only `neo-ime/` changes: loader/lifecycle code, regression checks, README and test
packaging. No native rebuild, installed-copy changes, commits, or worktrees.
Use isolated settings and cache directories under `neo-ime/var/`. Test actual
Windows DLL locking, switching and cleanup across separate Emacs processes;
cover cache corruption, unrelated files, locked files, update failure and active
composition. Run existing relevant checks and a fresh independent review.
