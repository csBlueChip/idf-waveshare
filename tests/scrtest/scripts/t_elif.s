# an error in an ELIF condition is reported at the ELIF's line
LET $$a = 2
IF $$a == 1
	PRINT "one"
ELIF $nope == 2
	PRINT "two"
END
