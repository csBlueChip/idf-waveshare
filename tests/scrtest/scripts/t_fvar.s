# FSTAT / FDELETE on a VARS SAVE file (name.var)
LET $$name = "Tim"
VARS SAVE persist $$name
GET $s = FSTAT persist.var
PRINT "stat: $.ok $s"
FDELETE persist.var
GET $s = FSTAT persist.var
PRINT "after delete: $.ok $.err"
ONERROR CONT
VARS LOAD persist $$name
PRINT "load: $.ok $.err"
