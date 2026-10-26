;;; neo-ime-tsf.el --- TSF sessions and Windows IME controls -*- lexical-binding: t; -*-
;; SPDX-License-Identifier: GPL-3.0-or-later
(require 'cl-lib)
(declare-function neo-ime-native-control "neo-ime-native" (hwnd command &rest arguments))

(defcustom neo-ime-input-backend 'auto
  "Input transport.  Auto tries TSF and retains IMM32 on initialization failure.
Use `tsf' to require TSF, or `imm32' for the compatibility transport.
Restart `neo-ime-mode' after changing this setting."
  :type '(choice (const auto) (const tsf) (const imm32)) :group 'neo-ime)

(cl-defstruct (neo-ime--tsf-session (:constructor neo-ime--tsf-session-create))
  frame window start end original committed generation tick preview composing)
(defvar neo-ime--tsf-sessions nil)
(defconst neo-ime--conversion-native #x0001)
(defconst neo-ime--conversion-katakana #x0002)
(defconst neo-ime--conversion-fullshape #x0008)
(defconst neo-ime--conversion-mask
  (logior neo-ime--conversion-native neo-ime--conversion-katakana neo-ime--conversion-fullshape))
(defconst neo-ime--conversion-modes
  `((hiragana . ,(logior neo-ime--conversion-native neo-ime--conversion-fullshape))
    (full-katakana . ,neo-ime--conversion-mask)
    (half-katakana . ,(logior neo-ime--conversion-native neo-ime--conversion-katakana))
    (full-alphanumeric . ,neo-ime--conversion-fullshape)
    (half-alphanumeric . 0)))

(defun neo-ime--control (frame command &rest arguments)
  "Execute COMMAND on FRAME's GUI thread."
  (unless (and neo-ime-mode (assq frame neo-ime--frames))
    (user-error "neo-ime is not active in this frame"))
  (apply #'neo-ime-native-control (neo-ime--hwnd frame) command arguments))

(defun neo-ime-open (&optional frame)
  "Request that Windows IME open in FRAME.  Return the observed state."
  (interactive)
  (setq frame (or frame (selected-frame)))
  (neo-ime--control frame 'open t)
  (let ((status (neo-ime--control frame 'open)))
    (neo-ime--update-status frame status)
    status))

(defun neo-ime-close (&optional frame)
  "Cancel unfinished input and request IME close in FRAME.
Return the observed state.  Already confirmed text is retained."
  (interactive)
  (setq frame (or frame (selected-frame)))
  (if (frame-parameter frame 'neo-ime--tsf)
      (neo-ime--tsf-cancel frame)
    (when (and neo-ime-mode (assq frame neo-ime--frames))
      (neo-ime-native-cancel (neo-ime--hwnd frame))
      (when (eq frame neo-ime--owner) (neo-ime--clear))))
  (neo-ime--control frame 'open nil)
  (let ((status (neo-ime--control frame 'open)))
    (neo-ime--update-status frame status)
    status))

(defun neo-ime-toggle (&optional frame)
  "Toggle Windows IME in FRAME using its current observed state."
  (interactive)
  (setq frame (or frame (selected-frame)))
  (pcase (neo-ime--control frame 'open)
    ('on (neo-ime-close frame)) ('off (neo-ime-open frame))
    (_ (user-error "Windows IME state is unknown"))))

(defun neo-ime-conversion-mode (&optional frame)
  "Return FRAME's conversion mode symbol, or `unknown'.
This describes conversion flags independently of IME open/close state."
  (let ((bits (neo-ime--control (or frame (selected-frame)) 'conversion)))
    (if (integerp bits)
        (or (car (rassq (logand bits neo-ime--conversion-mask) neo-ime--conversion-modes)) 'unknown)
      'unknown)))

(defun neo-ime-set-conversion-mode (mode &optional frame)
  "Request MODE in FRAME, preserving unrelated conversion flags.
Return the observed mode; an IME may reject or delay a requested change."
  (interactive (list (intern (completing-read "IME mode: " neo-ime--conversion-modes nil t))))
  (setq frame (or frame (selected-frame)))
  (let ((entry (assq mode neo-ime--conversion-modes)))
    (unless entry (user-error "Unsupported IME mode: %S" mode))
    (neo-ime--control frame 'conversion (cdr entry))
    (neo-ime-conversion-mode frame)))

(defun neo-ime--tsf-new-session (frame original &optional generation)
  "Describe the input range at point; ORIGINAL is nonempty for reconversion."
  (neo-ime--tsf-session-create
   :frame frame :window (selected-window) :start (copy-marker (point))
   :end (copy-marker (+ (point) (length original)) t)
   :original original :committed original :generation (or generation 1)
   :tick (buffer-chars-modified-tick)))

(defun neo-ime--tsf-restore-preview (session position)
  "Restore SESSION's committed text after removing its temporary display."
  (when (neo-ime--tsf-session-preview session)
    (remove-hook 'before-change-functions #'neo-ime--tsf-protect-preview t)
    (setf (neo-ime--tsf-session-preview session) nil)
    (goto-char position)
    (neo-ime--temporary-edit (insert (neo-ime--tsf-session-committed session)))
    (set-marker (neo-ime--tsf-session-end session) (point))
    (setf (neo-ime--tsf-session-tick session) (buffer-chars-modified-tick))))

(defun neo-ime--tsf-protect-preview (_start _end)
  "Prevent asynchronous writers from overwriting temporary conversion text."
  (when (and (overlayp neo-ime--overlay)
             (eq (overlay-buffer neo-ime--overlay) (current-buffer))
             (overlay-get neo-ime--overlay 'neo-ime-tsf-session))
    (user-error "Finish or cancel IME conversion before editing this buffer")))

(defun neo-ime--tsf-apply-event (session event)
  "Apply EVENT to SESSION, leaving unconfirmed edits outside Undo history."
  (pcase-let* ((`[,revision ,generation ,text ,cursor ,composition-start ,composition-length ,composing] (cl-subseq event 0 7))
               (start-marker (neo-ime--tsf-session-start session))
               (window (neo-ime--tsf-session-window session)))
    (ignore revision)
    (unless (and (= generation (neo-ime--tsf-session-generation session))
                 (marker-buffer start-marker) (window-live-p window)
                 (eq (window-buffer window) (marker-buffer start-marker)))
      (error "Stale TSF session; refusing to edit another buffer"))
    (with-current-buffer (marker-buffer start-marker)
      (unless (= (buffer-chars-modified-tick) (neo-ime--tsf-session-tick session))
        (error "Buffer changed outside the TSF session"))
      (neo-ime--clear)
      (setf (neo-ime--tsf-session-composing session) composing)
      (let* ((start (marker-position start-marker))
             (end (marker-position (neo-ime--tsf-session-end session)))
             (preedit-start (neo-ime--character-offset text composition-start))
             (preedit-end (neo-ime--character-offset text (+ composition-start composition-length)))
             (committed (if composing
                            (if (string-empty-p (neo-ime--tsf-session-original session))
                                (concat (substring text 0 preedit-start) (substring text preedit-end))
                              (neo-ime--tsf-session-committed session))
                          text)))
        (unless (equal committed (neo-ime--tsf-session-committed session))
          (goto-char start)
          (undo-boundary)
          (atomic-change-group (delete-region start end) (insert committed))
          (undo-boundary)
          (setf (neo-ime--tsf-session-committed session) committed)
          (set-marker (neo-ime--tsf-session-end session) (+ start (length committed))))
        (if (and composing (not (string-empty-p text)))
            (progn
              (neo-ime--temporary-edit
                (delete-region start (neo-ime--tsf-session-end session)))
              (set-window-point window start)
              (neo-ime--display window text cursor (make-vector (/ (string-bytes (encode-coding-string text 'utf-16-le t)) 2) 0))
              (overlay-put neo-ime--overlay 'neo-ime-tsf-session session)
              (setf (neo-ime--tsf-session-preview session) t)
              (add-hook 'before-change-functions #'neo-ime--tsf-protect-preview nil t)
              (neo-ime--temporary-edit
                (remove-text-properties start (+ start (length text)) '(face nil))
                (put-text-property (+ start preedit-start) (+ start preedit-end) 'face 'neo-ime-preedit)
                (when (> (length event) 7)
                  (let ((attributes (aref event 7)) (unit 0))
                    (dotimes (index (length text))
                      (when (and (<= preedit-start index) (< index preedit-end)
                                 (< unit (length attributes)) (memq (aref attributes unit) '(1 3)))
                        (put-text-property (+ start index) (+ start index 1) 'face 'neo-ime-target))
                      (cl-incf unit (if (> (aref text index) #xffff) 2 1)))))))
          (set-window-point window (+ start (neo-ime--character-offset text cursor))))
      (setf (neo-ime--tsf-session-tick session) (buffer-chars-modified-tick))))))

(defun neo-ime--tsf-poll (frame)
  "Apply and acknowledge all pending TSF events for FRAME."
  (let* ((session (cdr (assq frame neo-ime--tsf-sessions)))
         (state (neo-ime--control frame 'read)))
    (seq-doseq (event (aref state 0))
      (when session (neo-ime--tsf-apply-event session event))
      (neo-ime--control frame 'ack (aref event 0)))
    (when (and session (neo-ime--tsf-session-preview session)
               (eq frame neo-ime--owner))
      (neo-ime--show-candidates frame
        (vector 0 "" 0 [] t (aref state 1) (aref state 2) (aref state 3) (aref state 4))))))

(defun neo-ime--tsf-cancel (frame)
  "Keep already confirmed edits, discard unfinished conversion in FRAME."
  (when (frame-parameter frame 'neo-ime--tsf)
    (neo-ime--tsf-poll frame)
    (neo-ime--control frame 'cancel)
    ;; TerminateComposition can confirm the text.  Explicit cancellation must
    ;; not apply that synthetic final result to the buffer.
    (let ((state (neo-ime--control frame 'read)))
      (seq-doseq (event (aref state 0))
        (neo-ime--control frame 'ack (aref event 0))))
    (when (eq frame neo-ime--owner) (neo-ime--clear))
    (neo-ime--tsf-prepare frame)))

(defun neo-ime--tsf-prepare (frame &optional original)
  "Start a new TSF session at FRAME's selected writable buffer."
  (when (frame-parameter frame 'neo-ime--tsf)
    (with-selected-frame frame
      (let* ((previous (cdr (assq frame neo-ime--tsf-sessions)))
             (generation (1+ (if previous (neo-ime--tsf-session-generation previous) 0)))
             (session (neo-ime--tsf-new-session frame (or original "") generation)))
        (neo-ime--control frame 'reset (or original "") generation
                          (if (or buffer-read-only (get-text-property (point) 'read-only))
                              'read-only (and original t)))
        (setf (alist-get frame neo-ime--tsf-sessions) session)))))

(defun neo-ime--tsf-attach (frame)
  "Activate the selected input backend in FRAME."
  (unless (eq neo-ime-input-backend 'imm32)
    (unless (memq neo-ime-input-backend '(auto tsf)) (user-error "Invalid IME input backend"))
    (let ((started (neo-ime--control frame 'start)))
      (when (and (not started) (eq neo-ime-input-backend 'tsf))
        (user-error "TSF initialization failed"))
      (set-frame-parameter frame 'neo-ime--tsf started)
      (when started (neo-ime--tsf-prepare frame)))))

(defun neo-ime-reconvert (start end)
  "Reconvert the selected region using the current TSF provider."
  (interactive "r")
  (barf-if-buffer-read-only)
  (unless (< start end) (user-error "Select text to reconvert"))
  (unless (frame-parameter nil 'neo-ime--tsf) (user-error "Reconversion requires the TSF backend"))
  (neo-ime--tsf-cancel (selected-frame))
  (goto-char start)
  (neo-ime--tsf-prepare (selected-frame) (buffer-substring start end))
  (condition-case error-data
      (neo-ime--control (selected-frame) 'reconvert)
    (error (neo-ime--tsf-cancel (selected-frame))
           (signal (car error-data) (cdr error-data)))))

(provide 'neo-ime-tsf)
;;; neo-ime-tsf.el ends here
