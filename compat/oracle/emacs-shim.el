;;; emacs-shim.el --- canonical-record oracle shim for the Fe compat corpus  -*- lexical-binding: t; -*-

;; Not part of Fe or kg's own Lisp; this only runs under the pinned
;; Emacs oracle via `emacs -Q --batch -l emacs-shim.el CASE.json`, driven
;; by utils/run-emacs-oracle.py.  See ../README.md for the record
;; protocol this emits.
;;
;; `kind` is always decided by *which branch below ran* -- the
;; `condition-case` clause that caught control, or the plain return path
;; -- never by inspecting the text of an error message.  `quit` and
;; `error` are handled as separate clauses because in batch Emacs `quit`
;; is reachable (e.g. a case that signals it directly) and is not a
;; subtype of `error`; a `condition-case` that only names `error` would
;; not catch it at all, and the process would abort instead of reporting
;; a record.

(require 'json)

(defun kg-compat--read-case (path)
  "Read the JSON case object at PATH as an alist."
  (with-temp-buffer
    (insert-file-contents path)
    (goto-char (point-min))
    (json-parse-buffer :object-type 'alist :array-type 'list)))

(defun kg-compat--eval-string (source)
  "Read one form from SOURCE and evaluate it with lexical binding on.

The explicit LEXICAL argument to `eval' is what turns lexical binding on
here, rather than a file-local variable or a wrapping `-*-' header per
case: one mechanism for every case, chosen because case bodies are data
read at run time, not files Emacs loads on its own."
  (eval (car (read-from-string source)) t))

(defun kg-compat--run-case (case)
  "Evaluate CASE's setup forms then its expression, and return one record."
  (let ((setup (alist-get 'setup case))
        (expr (alist-get 'expr case)))
    (condition-case err
        (progn
          (dolist (form setup)
            (kg-compat--eval-string form))
          (kg-compat--eval-string expr))
      (:success
       `((kind . "value")
         (type . ,(symbol-name (type-of err)))
         (printed . ,(prin1-to-string err))))
      (quit
       '((kind . "quit")))
      (error
       `((kind . "condition")
         (condition . ,(symbol-name (car err)))
         (data . ,(prin1-to-string (cdr err)))
         (condition_source . "structured"))))))

(let* ((path (car command-line-args-left))
       (case (kg-compat--read-case path))
       (record (kg-compat--run-case case)))
  ;; Exactly one canonical record on stdout, nothing else; anything
  ;; Emacs prints incidentally (e.g. `message') goes to stderr in batch
  ;; mode already and is left unparsed.
  (princ (json-serialize record))
  (princ "\n"))
