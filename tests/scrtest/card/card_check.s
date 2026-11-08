# card layout: real data files in SD/scripts/data (run in a copy of sd/)
FSEEK boast TAG B=3
FREAD $p boast EOL
PRINT "boast.txt B=3: $p"
LET $$n = 42
VARS SAVE card $$n
LET $$n = 0
VARS LOAD card $$n
PRINT "card.var: $$n"
FLOG cardlog first line
FLOG cardlog second line
GET $s = FSTAT cardlog
PRINT "cardlog: $s"
FWRITE scratch hello
FDELETE scratch
GET $s = FSTAT scratch
PRINT "scratch deleted: $.ok"
CALL shapes.drawPoly 3 10 10 5
