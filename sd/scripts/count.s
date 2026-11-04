# count.s [n] - counts, one line per 500 ms (for STOP / PAUSE / STEP tests)
VARS $i $n
LET $n = 10
IF $arg[0] > 0 ; LET $n = $arg[1] ; END
LOOP $i = 1 TO $n
  PRINT "count $i of $n"
  WAIT 500
END
PRINT done
