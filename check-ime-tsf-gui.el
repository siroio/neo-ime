;;; check-ime-tsf-gui.el --- Isolated TSF module lifecycle -*- lexical-binding: t; -*-
(require 'cl-lib)
(setq load-prefer-newer t native-comp-jit-compilation nil)
(defconst neo-ime-tsf-check-root (file-name-directory load-file-name))
(setq user-emacs-directory (expand-file-name "var/tsf-gui/" neo-ime-tsf-check-root))
(add-to-list 'load-path neo-ime-tsf-check-root)
(require 'neo-ime)
(setq neo-ime-input-backend 'tsf)
(condition-case err
    (progn
      (neo-ime-mode 1)
      (cl-assert (frame-parameter nil 'neo-ime--tsf))
      (cl-assert (memq (neo-ime--control (selected-frame) 'open) '(on off unknown)))
      (let ((state (neo-ime--control (selected-frame) 'read)))
        (cl-assert (= (length state) 5)))
      (neo-ime--tsf-prepare (selected-frame))
      ;; The first IME-consumed key in a recursive minibuffer runs no command.
      (let ((original (buffer-string))
            (inject (lambda ()
                      (run-at-time 0 nil
                        (lambda ()
                          (let ((session (cdr (assq (selected-frame) neo-ime--tsf-sessions))))
                            (cl-assert (eq (marker-buffer (neo-ime--tsf-session-start session)) (current-buffer)))
                            (neo-ime--tsf-apply-event session
                              (vector 1 (neo-ime--tsf-session-generation session) "日本" 2 0 0 nil)))
                          (exit-minibuffer))))))
        (unwind-protect
            (progn
              (add-hook 'minibuffer-setup-hook inject t)
              (cl-assert (equal (read-string "TSF: ") "日本"))
              (cl-assert (equal (buffer-string) original)))
          (remove-hook 'minibuffer-setup-hook inject)))
      (neo-ime--tsf-prepare (selected-frame))
      (let ((second (make-frame '((window-system . w32) (visibility . nil)))))
        (unwind-protect
            (progn
              (cl-assert (frame-parameter second 'neo-ime--tsf))
              (neo-ime--tsf-prepare second)
              (neo-ime--detach second)
              (cl-assert (not (frame-parameter second 'neo-ime--tsf)))
              (cl-assert (vectorp (neo-ime--control (selected-frame) 'read))))
          (delete-frame second t)))
      (neo-ime-mode -1)
      (neo-ime-mode 1)
      (neo-ime-mode -1)
      (with-temp-file (expand-file-name "var/check-ime-tsf-gui.log" neo-ime-tsf-check-root)
        (insert "NEO_IME_TSF_GUI=PASS activate reset read minibuffer-routing multiple-frames deactivate reactivate\n"))
      (kill-emacs 0))
  (error
   (with-temp-file (expand-file-name "var/check-ime-tsf-gui.log" neo-ime-tsf-check-root)
     (insert (format "NEO_IME_TSF_GUI=FAIL %S\n" err)))
   (kill-emacs 1)))
