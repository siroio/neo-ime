;;; check-ime.el --- Inline IME regression checks -*- lexical-binding: t; -*-
;; emacs -Q --batch -l check-ime.el
(require 'cl-lib)
(add-to-list 'load-path (file-name-directory load-file-name))
(require 'neo-ime)

;; Preedit must never change the buffer, its modified flag or undo history.
(with-temp-buffer
  (switch-to-buffer (current-buffer))
  (insert "before after")
  (goto-char 8)
  (set-buffer-modified-p nil)
  (let ((undo buffer-undo-list))
    (neo-ime--display (selected-window) "にほんご" 2 [0 0 1 1])
    (cl-assert (equal (buffer-string) "before after"))
    (cl-assert (not (buffer-modified-p)))
    (cl-assert (equal undo buffer-undo-list))
    (cl-assert (= (overlay-start neo-ime--overlay) 8))
    (let ((text (overlay-get neo-ime--overlay 'before-string)))
      (cl-assert (equal (substring-no-properties text) "にほんご"))
      (cl-assert (eq (get-text-property 2 'cursor text) t))
      (cl-assert (eq (get-text-property 2 'face text) 'neo-ime-target))
      (cl-assert (eq (face-attribute 'neo-ime-preedit :background) 'unspecified) nil
                 "Preedit must not force a background"))
    ;; Updating preedit must not leave multiple overlays.
    (neo-ime--display (selected-window) "日本語" 3 [1 1 0])
    (cl-assert (= (length (overlays-in (point-min) (point-max))) 1))
    (neo-ime--clear)
    (cl-assert (null neo-ime--overlay))
    (cl-assert (null (overlays-in (point-min) (point-max))))))

;; IMM cursor/attribute offsets are UTF-16, Emacs positions are characters.
(cl-assert (= (neo-ime--character-offset "あ😀い" 3) 2))
(cl-assert (= (neo-ime--character-offset "あ😀い" 99) 3))
(cl-assert (= (neo-ime--character-offset "あ😀い" -1) 0))
(with-temp-buffer
  (switch-to-buffer (current-buffer))
  (neo-ime--display (selected-window) "あ😀い" 3 [0 1 1 0])
  (cl-assert (eq (get-text-property 1 'face
                                   (overlay-get neo-ime--overlay 'before-string))
                 'neo-ime-target))
  (neo-ime--clear))

;; A native partial commit is a self-insert command in Emacs.  Cancelling it
;; would discard the next unconfirmed clause from the IME.
(with-temp-buffer
  (switch-to-buffer (current-buffer))
  (let ((cancelled nil)
        (neo-ime--frames (list (cons (selected-frame) 10))))
    (cl-letf (((symbol-function 'neo-ime-native-cancel)
               (lambda (_) (setq cancelled t))))
      (neo-ime--display (selected-window) "次" 0 [0])
      (let ((this-command 'self-insert-command)) (neo-ime--before-command))
      (cl-assert (not cancelled))
      (cl-assert (= (cdar neo-ime--frames) -1))
      (neo-ime--display (selected-window) "次" 0 [0])
      (let ((this-command 'switch-to-buffer)) (neo-ime--before-command))
      (cl-assert cancelled)
      (cl-assert (not neo-ime--overlay)))))
(princ "NEO_IME_EL=PASS buffer-preserved theme-inherited utf16 cleanup\n")
