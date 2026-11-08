# INPUT "prompt" $v [ms]: the prompt is sent without a line end
XINPUT Tim
INPUT "name? " $n
PRINT "got $n"
LET $q = "age"
XINPUT 42
INPUT 'your $q: ' $a 5000
PRINT "got $a"
XINPUT plain
INPUT $p
PRINT "no prompt: $p"
INPUT "too slow? " $s 1000
PRINT "timed out: [$s] $.ok"
