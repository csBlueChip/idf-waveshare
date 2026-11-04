# variables, words, NV, built-ins, ONERROR CONT
ONERROR CONT
LET $l = "a b:c|d"
PRINT n=$l[0] w2=$l[2] w4=$l[4]
LET $i = 1 + 1
PRINT expr index: $l[$i + 1]
LET $l[2] = "X"
PRINT replaced: $l
LET $l[2] = "two words"
PRINT err: $.ok $.err
DEL $l[1]
PRINT del1: [$l]
DEL $l[2]
PRINT del2: [$l]
INS $l[1] = "first"
PRINT ins: [$l]
LET $w = ""
LET $w += "x"
LET $w += "y"
PRINT words: [$w]
LET $c = ""
$c++
$c++
$c--
LET $c *= 10
LET $c /= 4
LET $c -= 0.5
PRINT counter 2: $c
LET $c /= 0
PRINT div0: $.err
PRINT unset: $nope
PRINT after unset: $.ok $.err
LET $$g = 5
$$g++
PRINT global $$g nv $$scr.bl ${l} $${g}
LET $$scr.bl = 1
PRINT nv write: $.err
LET $.ok = 1
PRINT builtin write: $.err
PRINT built-ins: $.ax $.ay $.temp $.bat $.orient $.w $.h $.rot $.fw $.time
LET $id = "007"
LET $n = $id + 0
PRINT 007+0=$n text kept=$id
LET $t = "abc" + 1
PRINT plus text: $.err
LET $l[9] = "z"
PRINT range: $.err
FAIL some reason
PRINT cmd fail: $.ok $.err
GET $r = REPLY one two three
PRINT get: [$r] ok=$.ok
GET $r = FAIL bad thing
PRINT get fail: [$r] ok=$.ok err=$.err
ACC $x $y $z
GYR $gx $gy $gz
PRINT acc $x $y $z gyr $gx $gy $gz
