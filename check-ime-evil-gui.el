;;; check-ime-evil-gui.el --- Evil with real Windows IME control -*- lexical-binding: t; -*-
;; Run with -Q and installed Evil/goto-chg on load-path, in a separate GUI Emacs.
(require 'cl-lib)
(setq load-prefer-newer t native-comp-jit-compilation nil)
(defconst neo-ime-evil-gui-root (file-name-directory load-file-name))
(setq user-emacs-directory (expand-file-name "var/evil-gui/" neo-ime-evil-gui-root))
(add-to-list 'load-path neo-ime-evil-gui-root)
(require 'neo-ime)
(require 'evil)
(setq neo-ime-candidate-backend 'child-frame)
(defvar neo-ime-evil-gui-error nil)

(condition-case error-data
    (progn
      (switch-to-buffer (get-buffer-create "*neo-ime Evil check*"))
      (evil-local-mode 1)
      (dolist (backend '(imm32 tsf))
        (setq neo-ime-input-backend backend)
        (neo-ime-mode 1)
        (unwind-protect
            (progn
              (evil-insert-state)
              (cl-assert (eq (neo-ime-open) 'on))
              (evil-normal-state)
              (cl-assert (eq (neo-ime--control (selected-frame) 'open) 'off))
              (evil-insert-state)
              (cl-assert (eq (neo-ime--control (selected-frame) 'open) 'off))
              (evil-normal-state)
              (let ((setup
                     (lambda ()
                       (run-at-time 0 nil
                         (lambda ()
                           (condition-case err
                               (progn
                                 ;; Even if Evil's Normal state is inherited,
                                 ;; a prompt must permit Japanese input.
                                 (setq-local evil-local-mode t evil-state 'normal)
                                 (cl-assert (eq (neo-ime-open) 'on))
                                 (neo-ime--after-command)
                                 (cl-assert (eq (neo-ime--control (selected-frame) 'open) 'on)))
                             (error (setq neo-ime-evil-gui-error err)))
                           (exit-minibuffer))))))
                (unwind-protect
                    (progn (add-hook 'minibuffer-setup-hook setup t)
                           (read-string "Evil IME check: "))
                  (remove-hook 'minibuffer-setup-hook setup)))
              (when neo-ime-evil-gui-error
                (signal (car neo-ime-evil-gui-error) (cdr neo-ime-evil-gui-error)))
              (neo-ime--after-command)
              (cl-assert (eq (neo-ime--control (selected-frame) 'open) 'off)))
          (neo-ime-mode -1)))
      (with-temp-file (expand-file-name "var/check-ime-evil-gui.log" neo-ime-evil-gui-root)
        (insert "NEO_IME_EVIL_GUI=PASS IMM32 TSF open-normal-close insert-stays-off minibuffer-on return-close\n"))
      (kill-emacs 0))
  (error
   (when neo-ime-mode (neo-ime-mode -1))
   (with-temp-file (expand-file-name "var/check-ime-evil-gui.log" neo-ime-evil-gui-root)
     (insert (format "NEO_IME_EVIL_GUI=FAIL %S\n" error-data)))
   (kill-emacs 1)))
