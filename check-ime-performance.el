;;; check-ime-performance.el --- Rendering regressions -*- lexical-binding: t; -*-
(require 'ert)
(require 'cl-lib)
(add-to-list 'load-path (file-name-directory load-file-name))
(require 'neo-ime)

(ert-deftest neo-ime-cursor-update-keeps-buffer-tick ()
  (with-temp-buffer
    (switch-to-buffer (current-buffer))
    (unwind-protect
        (progn
          (neo-ime--display (selected-window) "あ😀い" 3 [0 1 1 0])
          (let ((tick (buffer-modified-tick)))
            (neo-ime--display (selected-window) "あ😀い" 1 [0 1 1 0])
            (should (= tick (buffer-modified-tick)))
            (should (= (point) 2)))
          ;; Changing only attributes must still update the highlight.
          (neo-ime--display (selected-window) "あ😀い" 1 [1 0 0 0])
          (should (eq (get-text-property 1 'face) 'neo-ime-target))
          (should (eq (get-text-property 2 'face) 'neo-ime-preedit))
          ;; Same text after cleanup must get fresh highlights.
          (neo-ime--clear)
          (neo-ime--display (selected-window) "あ😀い" 1 [0 1 1 0])
          (should (eq (get-text-property 2 'face) 'neo-ime-target)))
      (neo-ime--clear))))

(ert-deftest neo-ime-candidate-update-keeps-buffer-tick ()
  (with-temp-buffer
    (switch-to-buffer (current-buffer))
    (let* ((frame (selected-frame))
           (neo-ime--frames (list (cons frame -1)))
           (state [1 "あ" 1 [1] t ["亜"] 0 0 1]))
      (cl-letf (((symbol-function 'neo-ime-native-snapshot)
                 (lambda (&rest _) state))
                ((symbol-function 'neo-ime--show-candidates) #'ignore))
        (unwind-protect
            (progn
              (neo-ime--poll)
              (let ((tick (buffer-modified-tick)))
                (setq state [2 "あ" 1 [1] t ["阿"] 0 0 1])
                (neo-ime--poll)
                (should (= tick (buffer-modified-tick)))))
          (neo-ime--clear))))))

(ert-deftest neo-ime-attribute-cache-keeps-independent-values ()
  (with-temp-buffer
    (switch-to-buffer (current-buffer))
    (let ((attributes (vector 1 0)))
      (unwind-protect
          (progn
            (neo-ime--display (selected-window) "あい" 1 attributes)
            (aset attributes 0 0)
            (aset attributes 1 1)
            (neo-ime--display (selected-window) "あい" 1 attributes)
            (should (eq (get-text-property 1 'face) 'neo-ime-preedit))
            (should (eq (get-text-property 2 'face) 'neo-ime-target))
            ;; New characters with unchanged attributes need their faces too.
            (neo-ime--display (selected-window) "うえ" 1 attributes)
            (should (eq (get-text-property 1 'face) 'neo-ime-preedit))
            (should (eq (get-text-property 2 'face) 'neo-ime-target)))
        (neo-ime--clear)))))

(ert-run-tests-batch-and-exit)
