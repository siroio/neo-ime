;;; bench-ime.el --- Scoped bridge/display measurements -*- lexical-binding: t; -*-
(require 'cl-lib)
(require 'benchmark)
(defconst ime-bench-root (file-name-directory load-file-name))
(add-to-list 'load-path ime-bench-root)
(require 'neo-ime)
(make-directory (expand-file-name "var" ime-bench-root) t)
(module-load neo-ime-native-file)
(let* ((hwnd (neo-ime--hwnd (selected-frame)))
       (fast (getenv "NEO_IME_BENCH_FAST"))
       results)
  (neo-ime-native-attach hwnd)
  (unwind-protect
      (progn
        (let ((revision (aref (neo-ime-native-snapshot hwnd) 0)))
          (dotimes (_ 5)
            (garbage-collect)
            (push (cons 'snapshot
                        (benchmark-run 100000
                          (if fast (neo-ime-native-snapshot hwnd revision)
                            (neo-ime-native-snapshot hwnd)))) results)))
        (with-temp-buffer
          (switch-to-buffer (current-buffer))
          (neo-ime--display (selected-window) "にほんごにゅうりょく😀" 4 [0 0 1 1 1 1 0 0 0 0 0 0])
          (dotimes (_ 5)
            (garbage-collect)
            (push (cons 'display
                        (benchmark-run 10000
                          (neo-ime--display (selected-window) "にほんごにゅうりょく😀" 4 [0 0 1 1 1 1 0 0 0 0 0 0]))) results))
          (neo-ime--clear))
        (with-temp-file (expand-file-name (if fast "var/bench-after.log" "var/bench-before.log") ime-bench-root)
          (prin1 (nreverse results) (current-buffer))))
    (neo-ime-native-detach hwnd)))
(kill-emacs 0)
