;;; neo-ime.el --- Inline Windows IME composition -*- lexical-binding: t; -*-
;; Version: 0.1.3
;; Author: SIRO
;; URL: https://github.com/siroio/neo-ime
;; Package-Requires: ((emacs "29.1"))
;; Keywords: i18n
;; SPDX-License-Identifier: GPL-3.0-or-later

;;; Commentary:
;; Display Windows IME preedit using the buffer's colors.  Requires the
;; companion neo-ime-native.dll; no Emacs patch or alternative IME is used.
;; Put this file and the DLL on load-path, then (neo-ime-mode 1).
;; Candidates are drawn in an Emacs child frame; commits use Emacs's normal path.

;;; Code:
(require 'cl-lib)
(require 'subr-x)

(defgroup neo-ime nil "Inline Windows IME composition." :group 'i18n)
(defface neo-ime-preedit '((t (:underline t)))
  "Preedit text, inheriting the buffer's foreground and background."
  :group 'neo-ime)
(defface neo-ime-target '((t (:inherit neo-ime-preedit :weight bold)))
  "The clause currently selected for conversion." :group 'neo-ime)
(defcustom neo-ime-poll-interval 0.02
  "Seconds between native composition snapshots.  Restart the mode to apply."
  :type 'number :group 'neo-ime)
(defcustom neo-ime-native-file
  (expand-file-name "neo-ime-native.dll"
                    (file-name-directory (or load-file-name buffer-file-name)))
  "Path to the trusted companion DLL, compiled for this Emacs architecture."
  :type 'file :group 'neo-ime)

(declare-function neo-ime-native-attach "neo-ime-native" (hwnd))
(declare-function neo-ime-native-detach "neo-ime-native" (hwnd))
(declare-function neo-ime-native-snapshot "neo-ime-native" (hwnd))
(declare-function neo-ime-native-position "neo-ime-native" (hwnd x y height))
(declare-function neo-ime-native-cancel "neo-ime-native" (hwnd))

(defvar neo-ime--frames nil "Attached frames and last seen snapshot revisions.")
(defvar neo-ime--timer nil)
(defvar neo-ime--overlay nil)
(defvar neo-ime--owner nil "Frame owning the current preedit text.")
(defvar neo-ime--candidate-frame nil)

(defun neo-ime--hide-candidates ()
  "Hide the reusable candidate frame."
  (when (frame-live-p neo-ime--candidate-frame)
    (make-frame-invisible neo-ime--candidate-frame)))

(defun neo-ime--candidate-text (candidates selection start total)
  "Format CANDIDATES, highlighting absolute SELECTION in the current page."
  (concat
   (mapconcat
    (lambda (index)
      (let ((row (format " %d  %s " (1+ index) (aref candidates index))))
        (if (= (+ start index) selection)
            (propertize row 'face 'highlight) row)))
    (number-sequence 0 (1- (length candidates))) "\n")
   (format "\n %d / %d " (1+ selection) total)))

(defun neo-ime--show-candidates (frame state)
  "Draw STATE's candidate page below preedit in FRAME."
  (if (or (< (length state) 9) (= (length (aref state 5)) 0)
          (not (eq frame neo-ime--owner)))
      (neo-ime--hide-candidates)
    (let* ((window (overlay-get neo-ime--overlay 'window))
           (position (posn-at-point (overlay-start neo-ime--overlay) window))
           (text (neo-ime--candidate-text (aref state 5) (aref state 6)
                                          (aref state 7) (aref state 8)))
           (buffer (get-buffer-create " *neo-ime candidates*")))
      (when position
        (unless (and (frame-live-p neo-ime--candidate-frame)
                     (eq (frame-parent neo-ime--candidate-frame) frame))
          (when (frame-live-p neo-ime--candidate-frame)
            (delete-frame neo-ime--candidate-frame t))
          (setq neo-ime--candidate-frame
                (make-frame `((parent-frame . ,frame) (minibuffer . nil)
                              (name . "neo-ime candidates") (visibility . nil)
                              (no-accept-focus . t) (no-focus-on-map . t)
                              (undecorated . t) (skip-taskbar . t)
                              (no-other-frame . t) (desktop-dont-save . t)
                              (font . ,(frame-parameter frame 'font))
                              (foreground-color . ,(frame-parameter frame 'foreground-color))
                              (background-color . ,(frame-parameter frame 'background-color))
                              (internal-border-width . 2)
                              (menu-bar-lines . 0) (tool-bar-lines . 0)
                              (vertical-scroll-bars . nil) (horizontal-scroll-bars . nil)
                              (cursor-type . nil)))))
        (with-current-buffer buffer
          (setq-local mode-line-format nil header-line-format nil
                      cursor-type nil buffer-undo-list t truncate-lines t)
          (let ((inhibit-read-only t))
            (unless (equal-including-properties (buffer-string) text)
              (erase-buffer) (insert text)))
          (setq buffer-read-only t))
        (set-window-buffer (frame-root-window neo-ime--candidate-frame) buffer)
        (let* ((xy (posn-x-y position)) (edges (window-inside-pixel-edges window))
               (width (apply #'max (mapcar #'string-width (split-string text "\n"))))
               (height (1+ (length (aref state 5))))
               (x (+ (car edges) (car xy)))
               (y (+ (cadr edges) (cdr xy) (frame-char-height frame))))
          (set-frame-size neo-ime--candidate-frame (max 8 width) height)
          (when (> (+ y (frame-pixel-height neo-ime--candidate-frame))
                   (frame-pixel-height frame))
            (setq y (- y (frame-char-height frame)
                       (frame-pixel-height neo-ime--candidate-frame))))
          (set-frame-position neo-ime--candidate-frame
                              (max 0 (min x (- (frame-pixel-width frame)
                                               (frame-pixel-width neo-ime--candidate-frame))))
                              (max 0 y)))
        (make-frame-visible neo-ime--candidate-frame)))))

(defmacro neo-ime--temporary-edit (&rest body)
  "Run BODY without recording preedit edits or marking the buffer modified."
  (declare (indent 0) (debug t))
  `(let ((buffer-undo-list t)
         (inhibit-modification-hooks t)
         (deactivate-mark nil)
         (modified (buffer-modified-p)))
     (unwind-protect (progn ,@body)
       (restore-buffer-modified-p modified))))

(defun neo-ime--hwnd (frame)
  "Return FRAME's native window handle."
  (let ((id (frame-parameter frame 'window-id)))
    (if (stringp id) (string-to-number id) id)))

(defun neo-ime--character-offset (text units)
  "Convert the UTF-16 offset UNITS in TEXT to an Emacs character offset."
  (let ((index 0) (offset 0))
    (while (and (< index (length text)) (< offset (max 0 units)))
      (setq offset (+ offset (if (> (aref text index) #xffff) 2 1))
            index (1+ index)))
    index))

(defun neo-ime--clear ()
  "Remove temporary preedit text without adding Undo history."
  (neo-ime--hide-candidates)
  (when (and (overlayp neo-ime--overlay) (overlay-buffer neo-ime--overlay))
    (with-current-buffer (overlay-buffer neo-ime--overlay)
      (save-restriction
        (widen)
        (let ((start (overlay-start neo-ime--overlay))
              (end (overlay-end neo-ime--overlay))
              (window (overlay-get neo-ime--overlay 'window)))
          (let ((inhibit-read-only t))
            (neo-ime--temporary-edit (delete-region start end)))
          (when (and (window-live-p window)
                     (eq (window-buffer window) (current-buffer)))
            (set-window-point window start)))))
    (delete-overlay neo-ime--overlay))
  (setq neo-ime--overlay nil neo-ime--owner nil))

(defun neo-ime--display (window text cursor attributes)
  "Display TEXT in WINDOW; CURSOR and ATTRIBUTES use native UTF-16 offsets."
  (unless (and (overlayp neo-ime--overlay)
               (eq (overlay-buffer neo-ime--overlay) (window-buffer window))
               (eq (overlay-get neo-ime--overlay 'window) window))
    (neo-ime--clear)
    (setq neo-ime--overlay
          (make-overlay (window-point window) (window-point window)
                        (window-buffer window))))
  (with-current-buffer (window-buffer window)
    (let ((start (overlay-start neo-ime--overlay))
          (end (overlay-end neo-ime--overlay))
          (unit 0))
      (neo-ime--temporary-edit
        ;; Cursor and attribute changes should not delete/reinsert the text.
        (unless (equal (buffer-substring-no-properties start end) text)
          (goto-char start)
          (delete-region start end)
          (insert text)
          (move-overlay neo-ime--overlay start (+ start (length text))))
        (put-text-property start (+ start (length text)) 'face 'neo-ime-preedit)
        (dotimes (index (length text))
          (when (and (< unit (length attributes))
                     (memq (aref attributes unit) '(1 3)))
            (put-text-property (+ start index) (+ start index 1)
                               'face 'neo-ime-target))
          (setq unit (+ unit (if (> (aref text index) #xffff) 2 1)))))
      (set-window-point window (+ start (neo-ime--character-offset text cursor)))
      (overlay-put neo-ime--overlay 'window window)
      (setq neo-ime--owner (window-frame window)))))

(defun neo-ime--before-save ()
  "Cancel preedit before manual or automatic saving."
  ;; auto-save-hook is global and can run with a different current buffer.
  (when neo-ime--owner
    (when (frame-live-p neo-ime--owner)
      (neo-ime-native-cancel (neo-ime--hwnd neo-ime--owner)))
    (neo-ime--clear)))

(defun neo-ime--position (frame)
  "Update FRAME's candidate anchor from its selected window."
  (let* ((window (frame-selected-window frame))
         (position (posn-at-point (window-point window) window)))
    (when position
      (let ((xy (posn-x-y position))
            (edges (window-inside-pixel-edges window)))
        (neo-ime-native-position
         (neo-ime--hwnd frame) (+ (car edges) (car xy))
         (+ (cadr edges) (cdr xy)) (frame-char-height frame))))))

(defun neo-ime--attach (frame)
  "Attach a Windows GUI FRAME once."
  (when (and (eq (window-system frame) 'w32) (not (frame-parent frame))
             (not (assq frame neo-ime--frames)))
    (neo-ime-native-attach (neo-ime--hwnd frame))
    (push (cons frame -1) neo-ime--frames)
    (neo-ime--position frame)))

(defun neo-ime--detach (frame)
  "Detach FRAME and release its native state."
  (when (assq frame neo-ime--frames)
    (neo-ime-native-detach (neo-ime--hwnd frame))
    (setq neo-ime--frames (assq-delete-all frame neo-ime--frames)))
  (when (eq frame neo-ime--owner) (neo-ime--clear)))

(defun neo-ime--before-command ()
  "Cancel unfinished composition before an Emacs command changes its owner."
  (when neo-ime--owner
    ;; A partial IME commit may also start the next clause.  Its confirmed
    ;; WM_IME_CHAR events become self-insert-command; do not cancel that clause.
    (when (and (not (eq this-command 'self-insert-command))
               (frame-live-p neo-ime--owner))
      (neo-ime-native-cancel (neo-ime--hwnd neo-ime--owner)))
    (when-let* ((entry (assq neo-ime--owner neo-ime--frames)))
      (setcdr entry -1))
    (neo-ime--clear)))

(defun neo-ime--poll ()
  "Render changed snapshots on the Lisp thread."
  (condition-case error-data
      (dolist (entry neo-ime--frames)
        (let* ((frame (car entry))
               (state (neo-ime-native-snapshot (neo-ime--hwnd frame))))
          (when (and state (/= (aref state 0) (cdr entry)))
            (setcdr entry (aref state 0))
            (if (and (aref state 4) (not (string-empty-p (aref state 1))))
                (condition-case nil
                    (progn
                      (neo-ime--display (frame-selected-window frame)
                                        (aref state 1) (aref state 2) (aref state 3))
                      (neo-ime--show-candidates frame state))
                  (buffer-read-only
                   ;; Read-only views must not disable IME in every frame.
                   (neo-ime-native-cancel (neo-ime--hwnd frame))
                   (neo-ime--clear)))
              (when (eq frame neo-ime--owner) (neo-ime--clear))))))
    (error
     (neo-ime-mode -1)
     (display-warning 'neo-ime (error-message-string error-data) :error))))

(defun neo-ime--after-command ()
  "Keep the native candidate anchor aligned with point."
  (when (assq (selected-frame) neo-ime--frames)
    (neo-ime--position (selected-frame))))

(defun neo-ime--stop ()
  "Release timers, hooks, overlays and native subclasses."
  (when (timerp neo-ime--timer) (cancel-timer neo-ime--timer))
  (setq neo-ime--timer nil)
  (remove-hook 'after-make-frame-functions #'neo-ime--attach)
  (remove-hook 'delete-frame-functions #'neo-ime--detach)
  (remove-hook 'pre-command-hook #'neo-ime--before-command)
  (remove-hook 'post-command-hook #'neo-ime--after-command)
  (remove-hook 'kill-emacs-hook #'neo-ime--stop)
  (remove-hook 'before-save-hook #'neo-ime--before-save)
  (remove-hook 'auto-save-hook #'neo-ime--before-save)
  (dolist (entry (copy-sequence neo-ime--frames))
    (neo-ime--detach (car entry)))
  (neo-ime--clear)
  (when (frame-live-p neo-ime--candidate-frame)
    (delete-frame neo-ime--candidate-frame t))
  (setq neo-ime--candidate-frame nil))


;;;###autoload
(define-minor-mode neo-ime-mode
  "Render Windows IME preedit inline and its candidates in a child frame.
This global mode needs a Windows GUI and the companion native module.
It does not select an IME or change `default-input-method'."
  :global t :group 'neo-ime
  (if (not neo-ime-mode)
      (neo-ime--stop)
    (condition-case error-data
        (progn
          (unless (and (eq system-type 'windows-nt) (fboundp 'module-load))
            (user-error "neo-ime needs native Windows Emacs with module support"))
          (unless (and (numberp neo-ime-poll-interval) (>= neo-ime-poll-interval 0.005))
            (user-error "neo-ime-poll-interval must be at least 0.005 seconds"))
          (unless (featurep 'neo-ime-native) (module-load neo-ime-native-file))
          (dolist (frame (frame-list)) (neo-ime--attach frame))
          (add-hook 'after-make-frame-functions #'neo-ime--attach)
          (add-hook 'delete-frame-functions #'neo-ime--detach)
          (add-hook 'pre-command-hook #'neo-ime--before-command)
          (add-hook 'post-command-hook #'neo-ime--after-command)
          (add-hook 'kill-emacs-hook #'neo-ime--stop)
          (add-hook 'before-save-hook #'neo-ime--before-save)
          (add-hook 'auto-save-hook #'neo-ime--before-save)
          (unless (timerp neo-ime--timer)
            ;; ponytail: poll snapshots at 50 Hz; native event transport if latency matters.
            (setq neo-ime--timer (run-at-time 0 neo-ime-poll-interval #'neo-ime--poll))))
      (error
       (setq neo-ime-mode nil)
       (neo-ime--stop)
       (signal (car error-data) (cdr error-data))))))

(provide 'neo-ime)
;;; neo-ime.el ends here
