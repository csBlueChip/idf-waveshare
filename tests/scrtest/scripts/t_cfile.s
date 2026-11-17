# file names from constants and variables; quoted (text) constants
.CONST FN_BOAST "boast"
.CONST FN_PLAIN quiz
.CONST FN_BOAST_T "boast.txt"
.CONST MSG "hello there"
.CONST ID "007"
FWRITE FN_BOAST "B=1\nfirst\nB=2\nsecond\n"
FSEEK FN_BOAST TAG B=2
FREAD $a FN_BOAST EOL
PRINT "constant: $a"
LET $f = FN_BOAST
PRINT "expression: [$f]"
LET $g = "boast"
FSEEK $g START
FREAD $b $g EOL
PRINT "variable: $b"
LET $$h = "quiz"
FWRITE $$h "q"
FTELL $p FN_PLAIN
PRINT "plain constant + global: $p"
LET $i = ID
PRINT "text constant kept exactly: $i"
TEXT 1 MSG
FAPP FN_BOAST_T appended
GET $s = FSTAT FN_BOAST_T
PRINT "command line: $s"
VARS SAVE FN_BOAST $$h
ONERROR CONT
LET $bad = "a.b"
FSEEK $bad START
PRINT "bad name: $.err"
