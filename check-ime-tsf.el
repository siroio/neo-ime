;;; check-ime-tsf.el --- TSF session and controls tests -*- lexical-binding: t; -*-
(require 'ert)
(require 'cl-lib)
(add-to-list 'load-path (file-name-directory load-file-name))
(require 'neo-ime)
(require 'neo-ime-tsf)

(ert-deftest neo-ime-tsf-confirm-and-undo ()
  (with-temp-buffer
    (switch-to-buffer (current-buffer))
    (buffer-enable-undo)
    (insert "before:")
    (setq buffer-undo-list nil)
    (let ((session (neo-ime--tsf-new-session (selected-frame) "")))
      (neo-ime--tsf-apply-event session [1 1 "にほん" 3 0 3 t])
      (should (equal (buffer-string) "before:にほん"))
      (should-not buffer-undo-list)
      (neo-ime--tsf-apply-event session [2 1 "日本" 2 0 0 nil])
      (should (equal (buffer-string) "before:日本"))
      (undo-only 1)
      (should (equal (buffer-string) "before:")))))

(ert-deftest neo-ime-tsf-partial-commit-cancel-keeps-prefix ()
  (with-temp-buffer
    (switch-to-buffer (current-buffer))
    (buffer-enable-undo)
    (let ((session (neo-ime--tsf-new-session (selected-frame) "")))
      (neo-ime--tsf-apply-event session [1 1 "日本ご" 3 2 1 t])
      (neo-ime--clear)
      (should (equal (buffer-string) "日本")))))

(ert-deftest neo-ime-tsf-reconversion-cancel-restores-original ()
  (with-temp-buffer
    (switch-to-buffer (current-buffer))
    (buffer-enable-undo)
    (insert "日本")
    (goto-char (point-min))
    (setq buffer-undo-list nil)
    (set-buffer-modified-p nil)
    (let ((session (neo-ime--tsf-new-session (selected-frame) "日本")))
      (neo-ime--tsf-apply-event session [1 1 "にほん" 3 0 3 t])
      (neo-ime--clear)
      (should (equal (buffer-string) "日本"))
      (should-not buffer-undo-list)
      (should-not (buffer-modified-p)))))

(ert-deftest neo-ime-tsf-stale-generation-does-not-edit ()
  (with-temp-buffer
    (switch-to-buffer (current-buffer))
    (let ((session (neo-ime--tsf-new-session (selected-frame) "")))
      (should-error (neo-ime--tsf-apply-event session [1 99 "lost" 4 0 0 nil]))
      (should (equal (buffer-string) "")))))

(ert-deftest neo-ime-tsf-preview-rejects-uncoordinated-edit ()
  (with-temp-buffer
    (switch-to-buffer (current-buffer))
    (let ((session (neo-ime--tsf-new-session (selected-frame) "")))
      (unwind-protect
          (progn
            (neo-ime--tsf-apply-event session [1 1 "にほん" 3 0 3 t])
            (should-error (insert "external"))
            (should (equal (buffer-string) "にほん")))
        (neo-ime--clear)))))

(ert-deftest neo-ime-tsf-commit-followed-by-new-preedit-retains-undo ()
  (with-temp-buffer
    (switch-to-buffer (current-buffer))
    (buffer-enable-undo)
    (let ((session (neo-ime--tsf-new-session (selected-frame) "")))
      (neo-ime--tsf-apply-event session [1 1 "日本" 2 0 0 nil])
      (neo-ime--tsf-apply-event session [2 1 "日本ご" 3 2 1 t])
      (neo-ime--clear)
      (should (equal (buffer-string) "日本"))
      (undo-only 1)
      (should (equal (buffer-string) "")))))

(ert-deftest neo-ime-conversion-names-preserve-separation-from-open ()
  (should (= (alist-get 'hiragana neo-ime--conversion-modes) 9))
  (should (= (alist-get 'full-katakana neo-ime--conversion-modes) 11))
  (should (= (alist-get 'half-katakana neo-ime--conversion-modes) 3))
  (should (= (alist-get 'full-alphanumeric neo-ime--conversion-modes) 8))
  (should (= (alist-get 'half-alphanumeric neo-ime--conversion-modes) 0)))

(ert-deftest neo-ime-controls-read-back-rejected-and-unknown-state ()
  (let ((calls nil))
    (cl-letf (((symbol-function 'neo-ime--control)
               (lambda (_frame command &rest args)
                 (push (cons command args) calls)
                 (if (eq command 'open) 'off 9)))
              ((symbol-function 'neo-ime--update-status) #'ignore))
      (should (eq (neo-ime-open) 'off))
      (should (eq (neo-ime-set-conversion-mode 'full-katakana) 'hiragana))
      (should-not (member '(open t) (butlast calls 2)))
      (should-error (neo-ime-set-conversion-mode 'invalid)))
    (cl-letf (((symbol-function 'neo-ime--control) (lambda (&rest _) 'unknown)))
      (should-error (neo-ime-toggle)))))

(ert-deftest neo-ime-tsf-attributes-follow-utf16-clause ()
  (with-temp-buffer
    (switch-to-buffer (current-buffer))
    (let ((session (neo-ime--tsf-new-session (selected-frame) "")))
      (unwind-protect
          (progn
            (neo-ime--tsf-apply-event session [1 1 "済😀あ" 4 1 3 t [0 1 1 0]])
            (should-not (get-text-property 1 'face))
            (should (eq (get-text-property 2 'face) 'neo-ime-target))
            (should (eq (get-text-property 3 'face) 'neo-ime-preedit)))
        (neo-ime--clear)))))

(ert-deftest neo-ime-tsf-reconversion-confirm-undo-restores-original ()
  (with-temp-buffer
    (switch-to-buffer (current-buffer))
    (buffer-enable-undo)
    (insert "日本語")
    (goto-char (point-min))
    (setq buffer-undo-list nil)
    (let ((session (neo-ime--tsf-new-session (selected-frame) "日本語")))
      (neo-ime--tsf-apply-event session [1 1 "にほんご" 4 0 4 t])
      (neo-ime--tsf-apply-event session [2 1 "二本語" 3 0 0 nil])
      (should (equal (buffer-string) "二本語"))
      (undo-only 1)
      (should (equal (buffer-string) "日本語")))))

(ert-deftest neo-ime-tsf-reconversion-provider-error-preserves-buffer ()
  (with-temp-buffer
    (switch-to-buffer (current-buffer))
    (insert "original")
    (let ((neo-ime--tsf-sessions nil))
      (unwind-protect
          (cl-letf (((symbol-function 'neo-ime--control)
                     (lambda (frame command &rest _)
                       (pcase command
                         ('read [[] [] -1 0 0])
                         ('reconvert
                          (let ((session (cdr (assq frame neo-ime--tsf-sessions))))
                            (neo-ime--tsf-apply-event session
                              (vector 1 (neo-ime--tsf-session-generation session) "preview" 7 0 7 t)))
                          (error "Provider rejected reconversion"))
                         (_ t)))))
            (set-frame-parameter nil 'neo-ime--tsf t)
            (should-error (neo-ime-reconvert (point-min) (point-max)))
            (should (equal (buffer-string) "original"))
            (should-not neo-ime--overlay))
        (set-frame-parameter nil 'neo-ime--tsf nil)
        (neo-ime--clear)))))

(ert-run-tests-batch-and-exit)
