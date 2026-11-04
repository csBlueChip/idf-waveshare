# debugger (answers come from t_debug.opt -go)
QUIET 2
LET $a = 1
CALL f 5
LET $b = 2 ; LET $c = 3
BPT!
PRINT after BPT
XPAUSE
LET $d = 4
PRINT end
FN f $p
  LET $in = $p
  LET $in2 = $p * 2
END
