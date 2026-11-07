# FSEEK file TAG text: first line equal to text (trailing blanks ignored)
FWRITE tg "I=2\n#begin\nB=1  \nP=one\nB=10\nP=ten\nB=2\nP=two\n B=3\nB=3x\nlast"
LET $n = 2
FSEEK tg TAG B=$n
FTELL $p tg
FREAD $v tg EOL
PRINT "B=$n at $p: $v"
FSEEK tg TAG B=1
FREAD $v tg EOL
PRINT "B=1: $v"
FSEEK tg TAG "B=10   "
FREAD $v tg EOL
PRINT "B=10: $v"
FSEEK tg TAG last
FTELL $p tg
FSTAT tg
PRINT "after last: $p (size $.ret)"
FSEEK tg TAG I=2
FREAD $v tg EOL
PRINT "I=2 then: $v"
ONERROR CONT
FSEEK tg START
FSEEK tg TAG B=3
FTELL $p tg
PRINT "B=3: $.ok $.err, pos $p"
FSEEK tg TAG
PRINT "empty: $.err"
FSEEK nofile TAG x
PRINT "nofile: $.err"
