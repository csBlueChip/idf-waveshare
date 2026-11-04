# functions, CALL file.fn, RETURN, LET = CALL, recursion, stack
.STACK 5
LET $$total = 0
CALL add 3 4
PRINT total=$$total ret=$.ret
LET $v = CALL twice 21
PRINT twice=$v
LET $f = CALL fact 4
PRINT fact4=$f
CALL lib.greet World
LET $w = CALL lib.wrap abc
PRINT wrap=$w
CALL show a b:c|d
LET $d = CALL depth 1
PRINT never
FN add $a $b
  LET $$total = $a + $b
  RETURN $$total * 10
END
FN twice $x
  RETURN $x * 2
END
FN fact $n
  IF $n <= 1 ; RETURN 1 ; END
  LET $k = $n - 1
  LET $m = CALL fact $k
  RETURN $n * $m
END
FN show
  PRINT arg=[$arg] n=$arg[0] 1=$arg[1] 2=$arg[2] 3=$arg[3] 4=$arg[4]
END
FN depth $n
  PRINT depth $n
  LET $k = $n + 1
  LET $x = CALL depth $k
END
