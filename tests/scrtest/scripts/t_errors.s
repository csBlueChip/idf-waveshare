# ONERROR GOTO across calls, run-time errors
QUIET 1
ONERROR GOTO oops
LET $n = 0
:retry
$n++
IF $n == 1 ; CALL deep ; END
IF $n == 2 ; FAIL from a command ; END
IF $n == 3 ; LET $x = SQRT(-1) ; END
IF $n == 4 ; RUN other ; END
IF $n == 5 ; PAUSE ; END
IF $n == 6 ; LET $y = 1 / 0 ; END
IF $n == 7 ; LET $z = NOPE(1) ; END
IF $n == 8 ; CALL nofn ; END
IF $n == 9 ; ON BTN h ; EVT BTN 1 SHORT ; WAIT 1 ; END
IF $n == 10 ; ONERROR STOP ; LET $q = $undefined ; END
PRINT never
:oops
PRINT caught $n: $.err
IF $n < 12 ; GOTO retry ; END
FN deep
  CALL deeper
END
FN deeper
  LET $a = 1 + "x"
END
FN h
  WAIT EVENT
END
