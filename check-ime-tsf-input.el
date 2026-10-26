;;; check-ime-tsf-input.el --- Isolated IME diagnostic; inspect results manually -*- lexical-binding: t; -*-
(require 'cl-lib)
(setq load-prefer-newer t native-comp-jit-compilation nil)
(defconst neo-ime-input-check-root (file-name-directory load-file-name))
(setq user-emacs-directory (expand-file-name "var/tsf-input/" neo-ime-input-check-root))
(add-to-list 'load-path neo-ime-input-check-root)
(require 'neo-ime)
(setq neo-ime-input-backend 'tsf)
(switch-to-buffer (get-buffer-create "*neo-ime test*"))
(erase-buffer)
(buffer-enable-undo)
(defvar neo-ime-input-check-events nil)
(defvar neo-ime-input-check-old-status nil)
(defvar neo-ime-input-check-old-mode nil)
(advice-add 'neo-ime--tsf-apply-event :before
            (lambda (_session event) (push (copy-sequence event) neo-ime-input-check-events)))
(defun neo-ime-input-check-finish ()
  (condition-case err
      (progn
        (neo-ime--poll)
        (with-temp-file (expand-file-name "var/check-ime-tsf-input.log" neo-ime-input-check-root)
          (insert "NEO_IME_TSF_INPUT=DIAGNOSTIC (exit status alone does not prove conversion)\n")
          (insert (format "mode=%S status=%S backend=%S events=%S text=%S\n"
                          neo-ime-mode (neo-ime-status) (frame-parameter nil 'neo-ime--tsf)
                          (reverse neo-ime-input-check-events)
                          (with-current-buffer "*neo-ime test*" (buffer-string))))
          (insert (format "conversion=%S\n" (neo-ime-conversion-mode))))
        (when neo-ime-mode
          (when (integerp neo-ime-input-check-old-mode)
            (neo-ime--control (selected-frame) 'conversion
                              (logand neo-ime-input-check-old-mode neo-ime--conversion-mask)))
          (when (memq neo-ime-input-check-old-status '(on off))
            (neo-ime--control (selected-frame) 'open (eq neo-ime-input-check-old-status 'on)))
          (neo-ime-mode -1))
        (kill-emacs 0))
    (error
     (with-temp-file (expand-file-name "var/check-ime-tsf-input.log" neo-ime-input-check-root)
       (insert (format "FAIL %S events=%S" err neo-ime-input-check-events)))
     (kill-emacs 1))))
(condition-case err
    (progn
      (neo-ime-mode 1)
      (setq neo-ime-input-check-old-status (neo-ime--control (selected-frame) 'open)
            neo-ime-input-check-old-mode (neo-ime--control (selected-frame) 'conversion))
      (neo-ime-open)
      (neo-ime-set-conversion-mode 'hiragana)
      (with-temp-file (expand-file-name "var/check-ime-tsf-input.ready" neo-ime-input-check-root)
        (insert (format "%d %d" (neo-ime--hwnd (selected-frame)) (emacs-pid))))
      (run-at-time 12 nil #'neo-ime-input-check-finish))
  (error
   (with-temp-file (expand-file-name "var/check-ime-tsf-input.log" neo-ime-input-check-root)
     (insert (format "START_FAIL %S" err)))
   (kill-emacs 1)))
