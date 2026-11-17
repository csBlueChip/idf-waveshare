# card layout: real data files in SD/scripts/data (run in a copy of sd/)
FSEEK boast TAG B=3
FREAD $p boast EOL
PRINT "boast.txt B=3: $p"
LET $$n = 42
VARS SAVE card $$n
LET $$n = 0
VARS LOAD card $$n
PRINT "card.var: $$n"
FAPP cardlog.txt first line
FAPP cardlog.txt second line
GET $s = FSTAT cardlog.txt
PRINT "cardlog: $s"
FWRITE scratch hello
FDELETE scratch.txt
GET $s = FSTAT scratch.txt
PRINT "scratch deleted: $.ok"
CALL shapes.drawPoly 3 10 10 5
GET $s = FSTAT cardlog
PRINT "no extension: $.ok $.err"
GET $s = FSTAT /sys/default.nvram
PRINT "folder in a script: $.ok $.err"
GET $s = FSTAT default.nvram
PRINT "nvram from /scripts: $.ok $.err"
