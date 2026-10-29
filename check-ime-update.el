;;; check-ime-update.el --- DLL cache and update checks -*- lexical-binding: t; -*-
(setq load-prefer-newer t
      native-comp-jit-compilation nil
      native-comp-enable-subr-trampolines nil)
(require 'ert)
(require 'cl-lib)
(require 'package-vc)
(defconst neo-ime-check-update-root (file-name-directory load-file-name))
(add-to-list 'load-path neo-ime-check-update-root)
(require 'neo-ime)
(defvar neo-ime--loaded-native-file)
(defvar neo-ime--native-cache-cleaned)
(defconst neo-ime-check-native-functions
  '(neo-ime-native-attach neo-ime-native-detach neo-ime-native-position
    neo-ime-native-cancel neo-ime-native-status neo-ime-native-snapshot
    neo-ime-native-control))

(defmacro neo-ime-check-with-cache (&rest body)
  (declare (indent 0))
  `(let* ((root (make-temp-file
                (expand-file-name "var/update-check-" neo-ime-check-update-root) t))
          (user-emacs-directory (file-name-as-directory root))
          (temporary-file-directory root)
          (neo-ime-native-file (expand-file-name "source.dll" root))
          (neo-ime--loaded-native-file nil)
          (neo-ime--native-cache-cleaned nil)
          (neo-ime--frames nil)
          (neo-ime--overlay nil)
          (neo-ime--tsf-sessions nil)
          (neo-ime-mode nil)
          (features (copy-sequence features))
          (saved-functions
           (mapcar (lambda (symbol) (cons symbol (symbol-function symbol)))
                   neo-ime-check-native-functions)))
     (unwind-protect
         (progn
           (with-temp-file neo-ime-native-file (insert "old"))
           (dolist (file '("neo-ime.el" "neo-ime-tsf.el"))
             (with-temp-file (expand-file-name file root)
               (insert ";;; -*- lexical-binding: t; -*-\n")))
           ,@body)
       (dolist (entry saved-functions)
         (if (cdr entry) (fset (car entry) (cdr entry)) (fmakunbound (car entry))))
       (unless (file-in-directory-p root (expand-file-name "var/" neo-ime-check-update-root))
         (error "Unexpected test directory: %s" root))
       (delete-directory root t))))

(defun neo-ime-check-module-load (file)
  "Register a complete fake module; actual DLL behavior is tested in subprocesses."
  (dolist (symbol neo-ime-check-native-functions)
    (fset symbol (lambda (&rest _) file)))
  (provide 'neo-ime-native))

(ert-deftest neo-ime-cache-reuses-content-and-preserves-the-source ()
  (neo-ime-check-with-cache
    (let ((first (neo-ime--cache-native-file)))
      (should (file-in-directory-p first (expand-file-name "neo-ime/native/" root)))
      (should-not (equal first neo-ime-native-file))
      (should (equal first (neo-ime--cache-native-file)))
      (with-temp-file neo-ime-native-file (insert "new"))
      (let ((second (neo-ime--cache-native-file)))
        (should-not (equal first second))
        (should (equal (with-temp-buffer (insert-file-contents-literally first)
                                        (buffer-string)) "old"))
        (should (equal (with-temp-buffer (insert-file-contents-literally second)
                                        (buffer-string)) "new"))))))

(ert-deftest neo-ime-cache-rejects-corruption ()
  (neo-ime-check-with-cache
    (let ((file (neo-ime--cache-native-file)))
      (with-temp-file file (insert "corrupt"))
      (should-error (neo-ime--cache-native-file))
      (should (equal (with-temp-buffer (insert-file-contents-literally file)
                                      (buffer-string)) "corrupt")))))

(ert-deftest neo-ime-cache-refuses-linked-copies ()
  (neo-ime-check-with-cache
    (let ((file (neo-ime--cache-native-file))
          (real-link (symbol-function 'file-symlink-p)))
      (cl-letf (((symbol-function 'file-symlink-p)
                 (lambda (path)
                   (if (equal path file) "foreign.dll" (funcall real-link path)))))
        (should-error (neo-ime--cache-native-file)))
      (should (file-exists-p file)))))

(ert-deftest neo-ime-cache-refuses-a-linked-managed-parent ()
  (neo-ime-check-with-cache
    (let ((parent (expand-file-name "neo-ime" root))
          (real-link (symbol-function 'file-symlink-p)))
      (cl-letf (((symbol-function 'file-symlink-p)
                 (lambda (path)
                   (if (equal (directory-file-name path) parent)
                       "foreign-directory" (funcall real-link path)))))
        (should-error (neo-ime--cache-native-file)))
      (should-not (file-exists-p (expand-file-name "neo-ime/native/" root))))))

(ert-deftest neo-ime-cache-failed-publication-leaves-no-dll-or-temporary-file ()
  (neo-ime-check-with-cache
    (cl-letf (((symbol-function 'rename-file)
               (lambda (&rest _) (signal 'file-error '("publication failed")))))
      (should-error (neo-ime--cache-native-file)))
    (should-not (directory-files (expand-file-name "neo-ime/native/" root)
                                nil directory-files-no-dot-files-regexp))))

(ert-deftest neo-ime-cache-cleanup-keeps-current-and-unowned-files ()
  (neo-ime-check-with-cache
    (let* ((old (neo-ime--cache-native-file))
           (directory (file-name-directory old))
           (unrelated (expand-file-name "other.dll" directory))
           (subdirectory (expand-file-name
                          (concat "neo-ime-native-" (make-string 64 ?f) ".dll") directory)))
      (with-temp-file unrelated (insert "keep"))
      (make-directory subdirectory)
      (with-temp-file neo-ime-native-file (insert "current"))
      (let ((current (neo-ime--cache-native-file)))
        (neo-ime--clean-native-cache current)
        (should-not (file-exists-p old))
        (should (file-exists-p current))
        (should (file-exists-p unrelated))
        (should (file-directory-p subdirectory))
        (should (file-exists-p neo-ime-native-file))))))

(ert-deftest neo-ime-cache-cleanup-skips-locked-and-linked-files ()
  (neo-ime-check-with-cache
    (let ((locked (neo-ime--cache-native-file)))
      (with-temp-file neo-ime-native-file (insert "linked"))
      (let ((linked (neo-ime--cache-native-file)))
        (with-temp-file neo-ime-native-file (insert "obsolete"))
        (let ((obsolete (neo-ime--cache-native-file)))
          (with-temp-file neo-ime-native-file (insert "current"))
          (let ((current (neo-ime--cache-native-file))
                (real-delete (symbol-function 'delete-file))
                (real-link (symbol-function 'file-symlink-p)))
            (cl-letf (((symbol-function 'delete-file)
                       (lambda (file &rest args)
                         (if (equal file locked) (signal 'file-error '("in use"))
                           (apply real-delete file args))))
                      ((symbol-function 'file-symlink-p)
                       (lambda (file)
                         (if (equal file linked) "unrelated-target" (funcall real-link file)))))
              (neo-ime--clean-native-cache current))
            (should (file-exists-p locked))
            (should (file-exists-p linked))
            (should-not (file-exists-p obsolete))
            (should (file-exists-p current))))))))

(ert-deftest neo-ime-native-load-cleans-once-and-keeps-live-update-copies ()
  (neo-ime-check-with-cache
    (let ((stale (neo-ime--cache-native-file))
          (loads nil))
      (with-temp-file neo-ime-native-file (insert "first"))
      (cl-letf (((symbol-function 'module-load)
                 (lambda (file) (push file loads) (neo-ime-check-module-load file))))
        (neo-ime--load-native)
        (let ((first neo-ime--loaded-native-file))
          (should-not (file-exists-p stale))
          (neo-ime--load-native)
          (should (= (length loads) 1))
          (with-temp-file neo-ime-native-file (insert "second"))
          (neo-ime--load-native)
          (should (= (length loads) 2))
          (should (file-exists-p first))
          (should-not (equal first neo-ime--loaded-native-file)))))))

(ert-deftest neo-ime-native-load-rolls-back-partial-initialization ()
  (neo-ime-check-with-cache
    (cl-letf (((symbol-function 'module-load) #'neo-ime-check-module-load))
      (neo-ime--load-native))
    (let ((old-file neo-ime--loaded-native-file)
          (old-status (symbol-function 'neo-ime-native-status)))
      (with-temp-file neo-ime-native-file (insert "bad module"))
      (cl-letf (((symbol-function 'module-load)
                 (lambda (_)
                   (fset 'neo-ime-native-status #'ignore)
                   (error "Initialization failed"))))
        (should-error (neo-ime--load-native)))
      (should (eq (symbol-function 'neo-ime-native-status) old-status))
      (should (equal neo-ime--loaded-native-file old-file))
      (should (featurep 'neo-ime-native)))))

(ert-deftest neo-ime-native-load-rejects-an-incomplete-api ()
  (neo-ime-check-with-cache
    (let ((old-status (symbol-function 'neo-ime-native-status))
          (called nil))
      (cl-letf (((symbol-function 'module-load)
                 (lambda (_) (setq called t) (fset 'neo-ime-native-status #'ignore)
                   (provide 'neo-ime-native))))
        (should-error (neo-ime--load-native)))
      (should called)
      (should (eq old-status (symbol-function 'neo-ime-native-status)))
      (should-not neo-ime--loaded-native-file))))

(ert-deftest neo-ime-native-load-recovers-from-concurrent-cache-cleanup ()
  (neo-ime-check-with-cache
    (let ((attempts 0))
      (cl-letf (((symbol-function 'module-load)
                 (lambda (file)
                   (cl-incf attempts)
                   (if (= attempts 1)
                       (progn (delete-file file)
                              (signal 'module-open-failed (list file "File not found")))
                     (should (file-exists-p file))
                     (neo-ime-check-module-load file)))))
        (neo-ime--load-native))
      (should (= attempts 2))
      (should (file-exists-p neo-ime--loaded-native-file)))))

(ert-deftest neo-ime-update-rebuilds-while-stopped-and-restores-enabled-state ()
  (neo-ime-check-with-cache
    (let ((neo-ime-mode t)
          (descriptor (package-desc-create :name 'neo-ime :kind 'vc :dir root))
          (events nil))
      (cl-letf (((symbol-function 'neo-ime-mode)
                 (lambda (arg) (setq neo-ime-mode (> arg 0)) (push arg events))))
        (should (eq (neo-ime--package-vc-rebuild
                     (lambda (desc)
                       (should (eq desc descriptor))
                       (should-not neo-ime-mode)
                       'rebuilt)
                     descriptor) 'rebuilt))
        (should neo-ime-mode)
        (should (equal events '(1 -1)))))))

(ert-deftest neo-ime-update-failure-stays-disabled ()
  (neo-ime-check-with-cache
    (let ((neo-ime-mode t))
      (cl-letf (((symbol-function 'neo-ime-mode)
                 (lambda (arg) (setq neo-ime-mode (> arg 0)))))
        (should-error (neo-ime--package-vc-rebuild
                       (lambda (_) (error "Rebuild failed"))
                       (package-desc-create :name 'neo-ime :kind 'vc)))
        (should-not neo-ime-mode)))))

(ert-deftest neo-ime-update-supports-older-package-vc-rebuild-signature ()
  (neo-ime-check-with-cache
    (let ((neo-ime-mode t)
          (descriptor (package-desc-create :name 'neo-ime :kind 'vc :dir root)))
      (cl-letf (((symbol-function 'neo-ime-mode)
                 (lambda (arg) (setq neo-ime-mode (> arg 0))))
                ((symbol-function 'package-vc--main-file)
                 (lambda (_) (expand-file-name "neo-ime.el" root)))
                ((symbol-function 'package-vc--checkout-dir) nil))
        (should (eq (neo-ime--package-vc-rebuild
                     (lambda (desc directory)
                       (should (eq desc descriptor))
                       (should (equal directory root))
                       (should-not neo-ime-mode)
                       'rebuilt)
                     descriptor root) 'rebuilt))
        (should neo-ime-mode)))))

(ert-deftest neo-ime-update-does-not-resume-after-a-demoted-lisp-load-error ()
  (neo-ime-check-with-cache
    (let* ((neo-ime-mode t)
           (file (expand-file-name "neo-ime-tsf.el" root))
           (load-path (cons root load-path))
           (load-history (cons (list file) load-history))
           (descriptor (package-desc-create :name 'neo-ime :kind 'vc :dir root)))
      (with-temp-file file
        (insert ";;; -*- lexical-binding: t; -*-\n(error \"Broken updated Lisp\")\n"))
      (cl-letf (((symbol-function 'neo-ime-mode)
                 (lambda (arg) (setq neo-ime-mode (> arg 0)))))
        (should-error
         (neo-ime--package-vc-rebuild
          (lambda (desc) (package--reload-previously-loaded desc) 'rebuilt)
          descriptor))
        (should-not neo-ime-mode)))))

(ert-deftest neo-ime-update-leaves-disabled-modes-and-other-packages-alone ()
  (neo-ime-check-with-cache
    (cl-letf (((symbol-function 'neo-ime-mode)
               (lambda (&rest _) (error "Must not change the mode"))))
      (should (eq (neo-ime--package-vc-rebuild
                   (lambda (_) 'rebuilt) (package-desc-create :name 'neo-ime :kind 'vc))
                  'rebuilt))
      (let ((neo-ime-mode t))
        (should (eq (neo-ime--package-vc-rebuild
                     (lambda (_) 'other) (package-desc-create :name 'other :kind 'vc))
                    'other))))))

(ert-deftest neo-ime-update-does-not-discard-composition ()
  (neo-ime-check-with-cache
    (with-temp-buffer
      (insert "keep")
      (let ((neo-ime-mode t)
            (neo-ime--overlay (make-overlay 1 5)))
        (should-error
         (neo-ime--package-vc-rebuild
          (lambda (_) (error "Must not rebuild during composition"))
          (package-desc-create :name 'neo-ime :kind 'vc)) :type 'user-error)
        (should neo-ime-mode)
        (should (equal (buffer-string) "keep"))
        (should (overlay-buffer neo-ime--overlay))))))

(ert-deftest neo-ime-update-checks-native-composition-before-the-next-poll ()
  (neo-ime-check-with-cache
    (let ((neo-ime-mode t)
          (neo-ime--frames (list (cons (selected-frame) -1))))
      (cl-letf (((symbol-function 'neo-ime--hwnd) (lambda (_) 0))
                ((symbol-function 'neo-ime-native-snapshot)
                 (lambda (_) [1 "input" 0 [] t [] 0 -1 0])))
        (should-error
         (neo-ime--package-vc-rebuild
          (lambda (_) (error "Must not rebuild"))
          (package-desc-create :name 'neo-ime :kind 'vc)) :type 'user-error)
        (should neo-ime-mode)))))

(ert-deftest neo-ime-update-does-not-rebuild-after-failed-detach ()
  (neo-ime-check-with-cache
    (let ((neo-ime-mode t)
          (rebuilt nil))
      (cl-letf (((symbol-function 'neo-ime-mode)
                 (lambda (_) (error "Cannot detach frame"))))
        (should-error
         (neo-ime--package-vc-rebuild
          (lambda (_) (setq rebuilt t))
          (package-desc-create :name 'neo-ime :kind 'vc)))
        (should-not rebuilt)))))

(ert-deftest neo-ime-update-real-dll-switch-and-next-startup-cleanup ()
  (skip-unless (eq system-type 'windows-nt))
  (neo-ime-check-with-cache
    (copy-file (expand-file-name "neo-ime-native.dll" neo-ime-check-update-root)
               neo-ime-native-file t)
    (let* ((manifest (expand-file-name "copies.el" root))
           (setup `(progn
                     (setq load-prefer-newer t
                           native-comp-jit-compilation nil
                           native-comp-enable-subr-trampolines nil
                           user-emacs-directory ,user-emacs-directory
                           temporary-file-directory ,root
                           native-comp-eln-load-path (list ,root))
                     (add-to-list 'load-path ,neo-ime-check-update-root)
                     (require 'neo-ime)
                     (setq neo-ime-native-file ,neo-ime-native-file)))
           (first `(progn
                     ,setup
                     (neo-ime-mode 1)
                     (let ((old neo-ime--loaded-native-file)
                           (status (symbol-function 'neo-ime-native-status)))
                       ;; A PE overlay changes the content hash without rebuilding the DLL.
                       (with-temp-buffer
                         (set-buffer-multibyte nil)
                         (insert-file-contents-literally neo-ime-native-file)
                         (goto-char (point-max))
                         (insert "neo-ime update check")
                         (let ((coding-system-for-write 'no-conversion))
                           (write-region (point-min) (point-max) neo-ime-native-file nil 'silent)))
                       (neo-ime-mode -1)
                       (neo-ime-mode 1)
                       (cl-assert (not (eq status (symbol-function 'neo-ime-native-status))))
                       (cl-assert (not (equal old neo-ime--loaded-native-file)))
                       (cl-assert (eq (neo-ime-native-status 0) 'unknown))
                       (neo-ime--clean-native-cache neo-ime--loaded-native-file)
                       (cl-assert (file-exists-p old))
                       (with-temp-file ,manifest
                         (prin1 (list old neo-ime--loaded-native-file) (current-buffer))))
                     (neo-ime-mode -1)))
           (second `(progn
                      ,setup
                      (let ((copies (with-temp-buffer (insert-file-contents ,manifest)
                                                     (read (current-buffer)))))
                        (cl-assert (file-exists-p (car copies)))
                        (neo-ime-mode 1)
                        (cl-assert (not (file-exists-p (car copies))))
                        (cl-assert (equal neo-ime--loaded-native-file (cadr copies)))
                        (cl-assert (file-exists-p (cadr copies)))
                        (neo-ime-mode -1)))))
      (dolist (form (list first second))
        (with-temp-buffer
          (let ((exit-code (call-process
                            (expand-file-name invocation-name invocation-directory)
                            nil t nil "-Q" "--batch" "--eval" (prin1-to-string form))))
            (ert-info ((buffer-string)) (should (equal exit-code 0)))))))))

(make-directory (expand-file-name "var/" neo-ime-check-update-root) t)
(ert-run-tests-batch-and-exit "^neo-ime-\\(cache\\|native-load\\|update\\)-")
