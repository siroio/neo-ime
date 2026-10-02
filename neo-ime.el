;;; neo-ime.el --- Inline Windows IME composition -*- lexical-binding: t; -*-
;; Version: 0.1.0
;; Author: SIRO
;; URL: https://github.com/siroio/neo-ime
;; Package-Requires: ((emacs "29.1"))
;; Keywords: i18n
;; SPDX-License-Identifier: GPL-3.0-or-later

;;; Commentary:
;; Display Windows IME preedit using the buffer's colors.  Requires the
;; companion neo-ime-native.dll; no Emacs patch or alternative IME is used.
;; Put this file and the DLL on load-path, then (neo-ime-mode 1).
;; The native candidate list and Emacs's normal commit path are preserved.

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
(defvar neo-ime--owner nil "Frame owning the current preedit overlay.")

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
  "Remove only our composition display."
  (when (overlayp neo-ime--overlay) (delete-overlay neo-ime--overlay))
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
  (let* ((position (neo-ime--character-offset text cursor))
         (display (propertize (if (= position (length text)) (concat text " ") text)
                              'face 'neo-ime-preedit))
         (unit 0))
    (dotimes (index (length text))
      (when (and (< unit (length attributes))
                 (memq (aref attributes unit) '(1 3)))
        (put-text-property index (1+ index) 'face 'neo-ime-target display))
      (setq unit (+ unit (if (> (aref text index) #xffff) 2 1))))
    (put-text-property position (1+ position) 'cursor t display)
    (overlay-put neo-ime--overlay 'window window)
    (overlay-put neo-ime--overlay 'priority 1000)
    (overlay-put neo-ime--overlay 'before-string display)
    (setq neo-ime--owner (window-frame window))))

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
  (when (and (eq (window-system frame) 'w32) (not (assq frame neo-ime--frames)))
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
                (neo-ime--display (frame-selected-window frame)
                                  (aref state 1) (aref state 2) (aref state 3))
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
  (dolist (entry (copy-sequence neo-ime--frames))
    (neo-ime--detach (car entry)))
  (neo-ime--clear))

;;;###autoload
(define-minor-mode neo-ime-mode
  "Render Windows IME preedit inline, retaining the system's candidate list.
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
          (unless (timerp neo-ime--timer)
            ;; ponytail: poll snapshots at 50 Hz; native event transport if latency matters.
            (setq neo-ime--timer (run-at-time 0 neo-ime-poll-interval #'neo-ime--poll))))
      (error
       (setq neo-ime-mode nil)
       (neo-ime--stop)
       (signal (car error-data) (cdr error-data))))))

(provide 'neo-ime)
;;; neo-ime.el ends here
