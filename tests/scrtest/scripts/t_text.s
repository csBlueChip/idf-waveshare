# PRINT / DEBUG / ECHO text rules, substitution, .CONST, INSERT
.CONST WHITE 0xFF 0xFF 0xFF
.CONST MAXN 10
LET $x = 42
LET $q = 'say "hi" #1 \n $x'
PRINT   two spaces kept
PRINT    "quoted $x"
PRINT "say \"hi\""
PRINT 'it\'s'
PRINT \"lead" quote $x
PRINT a\nb \\ \# \$x \5gone
PRINT value kept: $q
DEBUG debug $x
ECHO 1 x=$x value=$q
ECHO 1 "q=$q"
APPEND log $x and \$
LED WHITE
LED 1 WHITE MAXN
IF $x > MAXN ; PRINT const in expr ok ; END
NV SET a $x#notacomment
FBOX ${x}mm $.w
INSERT inc.s
PRINT inserted: $$fromInc
QUIET 1
CLS 0 0 0
QUIET 0
CLS 1 1 1
TRACE 1
LET $y = 1 ; LET $z = 2
TRACE 0
PRINT "bad" x
PRINT never
