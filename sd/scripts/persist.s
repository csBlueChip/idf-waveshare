# persist.s - remember a name between runs (VARS SAVE / LOAD)
#   RUN persist          asks for the name the first time, then says hello
#   RUN persist forget   forgets the name
# The name is kept in SD:/scripts/data/persist.var
# At the name prompt any text is a name ("stop" too); !!stop stops it.

VARS $$name $r

IF $arg ~= "forget"
	GET $r = FDELETE persist.var	# GET: no error if already forgotten
	PRINT "name forgotten"
	EXIT
END

ONERROR CONT			# no persist.var yet on the first run
VARS LOAD persist $$name
ONERROR STOP

IF $$name == ""
	PRINT "name? (30 s)"
	INPUT $$name 30000
	IF $$name == ""
		PRINT "no answer"
		EXIT
	END
	VARS SAVE persist $$name
END

PRINT "hello $$name"
