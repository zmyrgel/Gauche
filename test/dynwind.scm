;;
;; Test dynamic-wind, call/cc and related stuff
;;

;; $Id: dynwind.scm,v 1.10 2001-07-08 08:24:25 shirok Exp $

(use gauche.test)

(test-start "dynamic-wind and call/cc")

;;------------------------------------------------------------------------

(define c #f)

;; An example in R5RS
(define (dynwind-test1)
  (let ((path '()))
    (let ((add (lambda (s) (set! path (cons s path)))))
      (dynamic-wind
       (lambda () (add 'connect))
       (lambda ()
         (add (call-with-current-continuation
               (lambda (c0) (set! c c0) 'talk1))))
       (lambda () (add 'disconnect)))
      (if (< (length path) 4)
          (c 'talk2)
          (reverse path)))))

(test "dynamic-wind"
      '(connect talk1 disconnect connect talk2 disconnect)
      dynwind-test1)

;; Test for handler stack.
(define (dynwind-test2)
  (let ((path '()))
    (dynamic-wind
     (lambda () (set! path (cons 1 path)))
     (lambda () (set! path (append (dynwind-test1) path)))
     (lambda () (set! path (cons 3 path))))
    path))

(test "dynamic-wind"
      '(3 connect talk1 disconnect connect talk2 disconnect 1)
      dynwind-test2)

;;-----------------------------------------------------------------------
;; Test for continuation

(define (callcc-test1)
  (let ((r '()))
    (let ((w (let ((v 1))
               (set! v (+ (call-with-current-continuation
                           (lambda (c0) (set! c c0) v))
                          v))
               (set! r (cons v r))
               v)))
      (if (<= w 1024) (c w) r))))

(test "call/cc (env)" '(2048 1024 512 256 128 64 32 16 8 4 2)
      callcc-test1)

;; continuation with multiple values

(test "call/cc (values)" '(1 2 3)
      (lambda () (receive x (call-with-current-continuation
                             (lambda (c) (c 1 2 3)))
                          x)))

(define (callcc-test2)
  (let ((cc #f)
        (r '()))
    (let ((s (list 1 2 3 4 (call/cc (lambda (c) (set! cc c) 5)) 6 7 8)))
      (if (null? r)
          (begin (set! r s) (cc -1))
          (list r s)))))
    
(test "call/cc (inline)" '((1 2 3 4 5 6 7 8) (1 2 3 4 -1 6 7 8))
      callcc-test2)

;;-----------------------------------------------------------------------
;; Test for stack overflow handling

;; Single call of fact-rec consumes
;;  5 (continuation) + 1 (n) + 4 (argframe) = 10
;; words.  With the default stack size 10000, n=1000 is enough to generate
;; the stack overflow.  There's no way to obtain compiled-in stack size
;; right now, so you need to adjust the parameters if you change the stack
;; size.

(define (sum-rec n)
  (if (> n 0)
      (+ n (sum-rec (- n 1)))
      0))

(test "stack overflow" (/ (* 1000 1001) 2)
      (lambda () (sum-rec 1000)))

(test "stack overflow" (/ (* 4000 4001) 2)
      (lambda () (sum-rec 4000)))

;;-----------------------------------------------------------------------
;; See if port stuff is cleaned up properly

(test "call-with-output-file -> port-closed?"
      #t
      (lambda ()
        (let ((p #f))
          (call-with-output-file
              "tmp1.o"
              (lambda (port)
                (write '(a b c d e) port)
                (set! p port)))
          (port-closed? p))))

(test "call-with-input-file -> port-closed?"
      '(#t a b c d e)
      (lambda ()
        (let* ((p #f)
               (r (call-with-input-file "tmp1.o"
                    (lambda (port)
                      (set! p port)
                      (read port)))))
          (cons (port-closed? p) r))))

(test "with-output-to-file -> port-closed?"
      '(#t #f)
      (lambda ()
        (let ((p #f))
          (with-output-to-file "tmp1.o"
            (lambda ()
              (set! p (current-output-port))
              (write '(a b c d e))))
          (list (port-closed? p)
                (eq? p (current-output-port))))))

(test "with-input-from-file -> port-closed?"
      '(#t #f)
      (lambda ()
        (let* ((p #f)
               (r (with-input-from-file "tmp1.o"
                    (lambda ()
                      (set! p (current-input-port))
                      (read)))))
          (list (port-closed? p)
                (eq? p (current-input-port))))))

(test-end)
