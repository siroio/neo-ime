;;; check-ime-minibuffer.el --- M-x candidate rendering regression -*- lexical-binding: t; -*-
;; emacs -Q -L /path/to/vertico -L /path/to/corfu -l check-ime-minibuffer.el
(require 'cl-lib)
(defconst neo-ime-mini-check-root (file-name-directory load-file-name))
(add-to-list 'load-path neo-ime-mini-check-root)
(require 'neo-ime)
(require 'vertico)
(require 'corfu)
(defvar neo-ime-minibuffer-candidate-backend)
(defvar neo-ime-mini-check-error nil)
(defvar neo-ime-mini-check-backend nil)
(defconst neo-ime-mini-check-state [1 "亜" 1 [1] t ["亜" "阿" "あ"] 10 9 100])

(defun neo-ime-mini-check-popup ()
  (cl-assert (frame-live-p neo-ime--candidate-frame))
  (cl-assert (frame-visible-p neo-ime--candidate-frame))
  (cl-assert (= 0 (frame-parameter neo-ime--candidate-frame 'tab-bar-lines)))
  (with-current-buffer " *neo-ime candidates*"
    (cl-assert (null tab-line-format))))

(defun neo-ime-mini-check-in-mx ()
  (condition-case err
      (let* ((window (selected-window))
             (frame (selected-frame))
             (ordinary-candidates vertico--candidates)
             (ordinary-index vertico--index))
        (cl-assert (window-minibuffer-p window))
        (cl-assert (overlayp vertico--candidates-ov))
        (neo-ime--display window "亜" 1 [1])
        (redisplay t)
        (neo-ime--show-candidates frame neo-ime-mini-check-state)
        (redisplay t)
        (pcase neo-ime-mini-check-backend
          ('vertico
           (let ((drawing (overlay-get vertico--candidates-ov 'before-string)))
             (cl-assert (string-match-p "1  亜" drawing))
             (cl-assert (string-match-p "11 / 100" drawing))
             (cl-assert (equal vertico--candidates ordinary-candidates))
             (cl-assert (= vertico--index ordinary-index))
             (cl-assert (eq (get-text-property (string-match " 2  阿" drawing) 'face drawing)
                            'highlight))
             (vertico--exhibit)
             (cl-assert (equal drawing (overlay-get vertico--candidates-ov 'before-string))))
           (cl-assert (not neo-ime--corfu-visible)))
          ('child-frame (neo-ime-mini-check-popup)))
        ;; No candidates yet/anymore must restore normal command completion.
        (neo-ime--show-candidates frame [2 "亜" 1 [1] t [] 0 0 0])
        (cl-assert (not (string-match-p "11 / 100"
                                       (overlay-get vertico--candidates-ov 'before-string))))
        (neo-ime--show-candidates frame neo-ime-mini-check-state)
        (neo-ime--clear)
        (cl-assert (equal (minibuffer-contents-no-properties) ""))
        (cl-assert (not (string-match-p "11 / 100"
                                       (overlay-get vertico--candidates-ov 'before-string))))
        ;; Exiting with a preedit must clear it via minibuffer-exit-hook too.
        (neo-ime--display window "亜" 1 [1])
        (neo-ime--show-candidates frame neo-ime-mini-check-state)
        (cl-assert neo-ime-mode))
    (error (setq neo-ime-mini-check-error err)))
  (abort-recursive-edit))

(defun neo-ime-mini-check-read-string ()
  (condition-case err
      (progn
        (neo-ime--display (selected-window) "亜" 1 [1])
        (redisplay t)
        (neo-ime--show-candidates (selected-frame) neo-ime-mini-check-state)
        (neo-ime-mini-check-popup))
    (error (setq neo-ime-mini-check-error err)))
  (abort-recursive-edit))

(condition-case err
    (progn
      (tab-bar-mode 1)
      (global-tab-line-mode 1)
      (vertico-mode 1)
      (setq neo-ime-candidate-backend 'corfu
            neo-ime-minibuffer-candidate-backend 'vertico)
      (neo-ime-mode 1)
      (cancel-timer neo-ime--timer)
      (setq neo-ime--timer nil)
      (dolist (backends '((corfu vertico) (corfu child-frame)
                          (child-frame vertico) (child-frame child-frame)))
        (setq neo-ime-candidate-backend (car backends)
              neo-ime-mini-check-backend (cadr backends)
              neo-ime-minibuffer-candidate-backend (cadr backends))
        (let ((minibuffer-setup-hook
               (append minibuffer-setup-hook
                       (list (lambda () (run-at-time 0.2 nil #'neo-ime-mini-check-in-mx))))))
          (condition-case nil (execute-extended-command nil) (quit nil)))
        (when neo-ime-mini-check-error (signal (car neo-ime-mini-check-error)
                                              (cdr neo-ime-mini-check-error)))
        (cl-assert (null neo-ime--overlay))
        (cl-assert (null neo-ime--owner)))
      ;; A plain minibuffer has no Vertico session to borrow.
      (setq neo-ime-minibuffer-candidate-backend 'vertico)
      (let ((minibuffer-setup-hook
             (list (lambda () (run-at-time 0.2 nil #'neo-ime-mini-check-read-string)))))
        (condition-case nil (read-string "Plain input: ") (quit nil)))
      (when neo-ime-mini-check-error (signal (car neo-ime-mini-check-error)
                                            (cdr neo-ime-mini-check-error)))
      (cl-assert (null neo-ime--overlay))
      ;; Normal-buffer selection stays independent from minibuffer selection.
      (setq neo-ime-candidate-backend 'corfu)
      (switch-to-buffer (get-buffer-create " *neo-ime normal check*"))
      (neo-ime--display (selected-window) "亜" 1 [1])
      (redisplay t)
      (neo-ime--show-candidates (selected-frame) neo-ime-mini-check-state)
      (cl-assert neo-ime--corfu-visible)
      (cl-assert (frame-visible-p corfu--frame))
      (neo-ime--clear)
      (setq neo-ime-candidate-backend 'child-frame)
      (neo-ime--display (selected-window) "亜" 1 [1])
      (redisplay t)
      (neo-ime--show-candidates (selected-frame) neo-ime-mini-check-state)
      (neo-ime-mini-check-popup)
      (neo-ime-mode -1)
      (cl-assert (not (advice-member-p #'neo-ime--vertico-exhibit 'vertico--exhibit)))
      (with-temp-file (expand-file-name "var/check-ime-minibuffer.log" neo-ime-mini-check-root)
        (insert "NEO_IME_MINIBUFFER=PASS M-x vertico-restoration independent-backends child-frame-no-tabs\n"))
      (kill-emacs 0))
  (error
   (when (bound-and-true-p neo-ime-mode) (neo-ime-mode -1))
   (with-temp-file (expand-file-name "var/check-ime-minibuffer.log" neo-ime-mini-check-root)
     (insert (format "NEO_IME_MINIBUFFER=FAIL %S\n" err)))
   (kill-emacs 1)))
