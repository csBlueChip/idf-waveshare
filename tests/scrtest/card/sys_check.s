# /sys/ script: data in SD/sys/data, libraries still from SD/scripts (run in a copy of sd/)
FAPP syslog.txt first line
LET $$n = 7
VARS SAVE sysvars $$n
LET $$n = 0
VARS LOAD sysvars $$n
PRINT "sysvars.var: $$n"
FWRITE sysdata hello
FSEEK sysdata START
FREAD $s sysdata EOL
PRINT "sysdata.txt: $s"
CALL shapes.drawPoly 3 10 10 5
GET $s = FSTAT sysdata.txt
PRINT "home data: $s"
GET $s = FSTAT /sys/sysdata.txt
PRINT "folder in a script: $.ok $.err"
FAPP sysnote.nvram # appended by a /sys/ script
GET $s = FSTAT sysnote.nvram
PRINT "nvram from /sys/: $s"
FDELETE sysnote.nvram
GET $s = FSTAT sysnote.nvram
PRINT "nvram deleted: $.ok"
