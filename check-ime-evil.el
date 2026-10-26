;;; check-ime-evil.el --- Evil transitions and native IME policy -*- lexical-binding: t; -*-
;; Add installed Evil and goto-chg directories with -L; no user config is loaded.
(require 'ert)
(require 'cl-lib)
(setq load-prefer-newer t)
(add-to-list 'load-path (file-name-directory load-file-name))
(require 'neo-ime)
(require 'evil)

(defmacro neo-ime-evil-check (&rest body)
  "Run BODY with real Evil and an isolated native IME boundary."
  (declare (indent 0))
  `(let ((neo-ime-input-backend 'imm32)
         (neo-ime-candidate-backend 'child-frame)
         (neo-ime-cursor-color-on nil) (neo-ime-cursor-color-off nil)
         (status 'on) (cancelled nil) (reject-close nil))
     (cl-letf (((symbol-function 'window-system) (lambda (&optional _) 'w32))
               ((symbol-function 'module-load) #'ignore)
               ((symbol-function 'neo-ime-native-attach) #'ignore)
               ((symbol-function 'neo-ime-native-detach) #'ignore)
               ((symbol-function 'neo-ime-native-position) #'ignore)
               ((symbol-function 'neo-ime-native-status) (lambda (_) status))
               ((symbol-function 'neo-ime-native-cancel) (lambda (_) (setq cancelled t)))
               ((symbol-function 'neo-ime-native-control)
                (lambda (_ command &rest arguments)
                  (pcase command
                    ('open (when arguments
                             (when reject-close (error "IME setter unavailable"))
                             (setq status (if (car arguments) 'on 'off)))
                           status)
                    ('read [[] [] -1 0 0])
                    ('cancel (setq cancelled t))
                    ((or 'reset 'ack) t)
                    (_ (error "Unexpected native command: %S" command))))))
       (save-window-excursion
         (with-temp-buffer
           (switch-to-buffer (current-buffer))
           (buffer-enable-undo)
           (insert "confirmed")
           (evil-local-mode 1)
           (unwind-protect
               (progn (neo-ime-mode 1) ,@body)
             (neo-ime-mode -1)
             (evil-local-mode -1)))))))

(ert-deftest neo-ime-evil-normal-entry-closes-and-cancels-imm ()
  (neo-ime-evil-check
    (evil-insert-state)
    (setq status 'on cancelled nil buffer-undo-list nil)
    (goto-char (point-max))
    (neo-ime--display (selected-window) "未確定" 3 [0 0 0])
    (evil-normal-state)
    (should (eq evil-state 'normal))
    (should (eq status 'off))
    (should cancelled)
    (should (equal (buffer-string) "confirmed"))
    (should-not neo-ime--overlay)
    (should-not buffer-undo-list)
    (evil-insert-state)
    (should (eq status 'off))))

(ert-deftest neo-ime-evil-normal-entry-cancels-tsf-preview ()
  (neo-ime-evil-check
    (evil-insert-state)
    (setq status 'on cancelled nil buffer-undo-list nil)
    (goto-char (point-max))
    (set-frame-parameter nil 'neo-ime--tsf t)
    (neo-ime--tsf-prepare (selected-frame))
    (neo-ime--tsf-apply-event (cdr (assq (selected-frame) neo-ime--tsf-sessions))
                            [1 1 "未確定" 3 0 3 t])
    (evil-normal-state)
    (should (eq status 'off))
    (should cancelled)
    (should (equal (buffer-string) "confirmed"))
    (should-not buffer-undo-list)))

(ert-deftest neo-ime-evil-buffer-switch-closes-without-state-entry ()
  (neo-ime-evil-check
    (let ((normal-buffer (current-buffer)))
      (with-temp-buffer
        (switch-to-buffer (current-buffer))
        (evil-local-mode 1)
        (evil-insert-state)
        (setq status 'on)
        (switch-to-buffer normal-buffer)
        (neo-ime--after-command)
        (should (eq status 'off))))))

(ert-deftest neo-ime-evil-input-states-and-background-buffers-keep-ime ()
  (neo-ime-evil-check
    (dolist (state '(insert replace emacs))
      (evil-change-state state)
      (setq status 'on)
      (neo-ime--after-command)
      (should (eq status 'on)))
    (with-temp-buffer
      (evil-local-mode 1)
      (evil-insert-state)
      (evil-normal-state))
    (should (eq status 'on))))

(ert-deftest neo-ime-evil-command-states-close-on-enable-and-entry ()
  (neo-ime-evil-check
    (should (eq status 'off))
    (dolist (state '(visual motion operator normal))
      (evil-insert-state)
      (setq status 'on)
      (evil-change-state state)
      (should (eq status 'off)))))

(ert-deftest neo-ime-evil-disabled-mode-leaves-ime-alone ()
  (neo-ime-evil-check
    (neo-ime-mode -1)
    (setq status 'on)
    (evil-insert-state)
    (evil-normal-state)
    (should (eq status 'on))))

(ert-deftest neo-ime-evil-setter-error-does-not-abort-normal-transition ()
  (neo-ime-evil-check
    (evil-insert-state)
    (setq status 'unknown reject-close t)
    (let ((warning-minimum-level :emergency)) (evil-normal-state))
    (should (eq evil-state 'normal))
    (should (eq status 'unknown))))

(ert-run-tests-batch-and-exit)
