;;; check-ime-update-gui.el --- Live package-vc/native switch -*- lexical-binding: t; -*-
;; Run in a dedicated GUI Emacs: emacs -Q -l check-ime-update-gui.el
(require 'cl-lib)
(require 'package-vc)
(defconst neo-ime-update-gui-root (file-name-directory load-file-name))
(setq load-prefer-newer t
      native-comp-jit-compilation nil
      native-comp-enable-subr-trampolines nil
      package-native-compile nil
      package-quickstart nil
      package-archives nil
      package-selected-packages '(neo-ime))
(make-directory (expand-file-name "var/" neo-ime-update-gui-root) t)
(setq user-emacs-directory
      (file-name-as-directory
       (make-temp-file (expand-file-name "var/update-gui-" neo-ime-update-gui-root) t))
      temporary-file-directory user-emacs-directory
      native-comp-eln-load-path (list user-emacs-directory)
      package-user-dir (expand-file-name "elpa/" user-emacs-directory)
      custom-file (expand-file-name "custom.el" user-emacs-directory))

(defun neo-ime-update-gui-step (step)
  (write-region (format "STEP=%s\n" step) nil
                (expand-file-name "var/check-ime-update-gui.log" neo-ime-update-gui-root)
                t 'silent))

(defun neo-ime-update-gui-check ()
  (with-temp-file (expand-file-name "var/check-ime-update-gui.log" neo-ime-update-gui-root)
    (insert (format "NEO_IME_UPDATE_GUI=RUNNING\nsettings=%s\n" user-emacs-directory)))
  (condition-case error-data
      (let* ((directory (expand-file-name "neo-ime/" package-user-dir))
             (descriptor (package-desc-create :name 'neo-ime :kind 'vc
                                               :version '(0 2 1) :dir directory))
             (frame (make-frame '((window-system . w32) (visibility . nil)))))
        (make-directory directory t)
        (dolist (file '("neo-ime.el" "neo-ime-tsf.el" "neo-ime-native.dll"))
          (copy-file (expand-file-name file neo-ime-update-gui-root)
                     (expand-file-name file directory)))
        (add-to-list 'load-path directory)
        (require 'neo-ime)
        (setq neo-ime-input-backend 'auto)
        (select-frame frame)
        (neo-ime-mode 1)
        (neo-ime-update-gui-step "enabled")
        (cl-assert (>= (length neo-ime--frames) 2))
        (let ((old neo-ime--loaded-native-file)
              (snapshot (symbol-function 'neo-ime-native-snapshot))
              (hwnd (neo-ime--hwnd frame)))
          (cl-assert (file-in-directory-p old user-emacs-directory))
          (cl-assert (not (file-in-directory-p old directory)))
          (with-temp-buffer
            (set-buffer-multibyte nil)
            (insert-file-contents-literally neo-ime-native-file)
            (goto-char (point-max))
            (insert "neo-ime GUI update check")
            (let ((coding-system-for-write 'no-conversion))
              (write-region (point-min) (point-max) neo-ime-native-file nil 'silent)))
          (package-vc--unpack-1 descriptor)
          (neo-ime-update-gui-step "first-rebuild")
          (cl-assert neo-ime-mode)
          (cl-assert (assq frame neo-ime--frames))
          (cl-assert (not (equal old neo-ime--loaded-native-file)))
          (cl-assert (not (eq snapshot (symbol-function 'neo-ime-native-snapshot))))
          (cl-assert (null (funcall snapshot hwnd)))
          (cl-assert (vectorp (neo-ime-native-snapshot hwnd)))
          (cl-assert (file-exists-p old))
          (neo-ime--clean-native-cache neo-ime--loaded-native-file)
          (cl-assert (file-exists-p old))
          (let ((current neo-ime--loaded-native-file))
            (package-vc--unpack-1 descriptor)
            (neo-ime-update-gui-step "repeat-rebuild")
            (cl-assert neo-ime-mode)
            (cl-assert (equal current neo-ime--loaded-native-file))))
        (with-temp-file (expand-file-name "neo-ime-tsf.el" directory)
          (insert ";;; -*- lexical-binding: t; -*-\n(error \"Broken updated Lisp\")\n"))
        (cl-assert (condition-case nil (progn (package-vc--unpack-1 descriptor) nil)
                     (error t)))
        (neo-ime-update-gui-step "failed-rebuild-stopped")
        (cl-assert (not neo-ime-mode))
        (cl-assert (null neo-ime--frames))
        (copy-file (expand-file-name "neo-ime-tsf.el" neo-ime-update-gui-root)
                   (expand-file-name "neo-ime-tsf.el" directory) t)
        (package-vc--unpack-1 descriptor)
        (neo-ime-update-gui-step "recovery-rebuild")
        (cl-assert (not neo-ime-mode))
        (neo-ime-mode 1)
        (neo-ime-update-gui-step "recovery-enabled")
        (let ((tsf (frame-parameter frame 'neo-ime--tsf)))
          (neo-ime-mode -1)
          (delete-frame frame t)
          (with-temp-file (expand-file-name "var/check-ime-update-gui.log" neo-ime-update-gui-root)
            (insert (format "NEO_IME_UPDATE_GUI=PASS two-frames real-package-vc-rebuild native-switch old-callbacks-detached old-copy-retained reload-failure-disabled tsf=%S\nsettings=%s\n"
                            tsf user-emacs-directory))))
        (kill-emacs 0))
    (error
     (when (fboundp 'neo-ime--stop) (ignore-errors (neo-ime--stop)))
     (with-temp-file (expand-file-name "var/check-ime-update-gui.log" neo-ime-update-gui-root)
       (insert (format "NEO_IME_UPDATE_GUI=FAIL %S\nsettings=%s\n" error-data user-emacs-directory)))
     (kill-emacs 1))))

(add-hook 'emacs-startup-hook #'neo-ime-update-gui-check)
