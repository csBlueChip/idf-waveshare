# PRINT / DEBUG / TEXT / TLEN text rules, substitution, .CONST, INSERT
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
TEXT 1 x=$x value=$q
TEXT 1 "q=$q"
TLEN 1 "q=$q"
GET $w = TLEN 1 x=$x
PRINT tlen ok=$.ok
FAPP log.txt $x and \$
FCAT log.txt
LED WHITE
LED 1 WHITE MAXN
IF $x > MAXN ; PRINT const in expr ok ; END
NV SET a $x#notacomment
FBOX ${x}mm $.w
INSERT inc.s
PRINT inserted: $$fromInc
UI EVENTS 2
CLS 0,0,0
UI EVENTS 3
CLS 1,1,1
TRACE 1
LET $y = 1 ; LET $z = 2
TRACE 0
PRINT "bad" x
PRINT never
