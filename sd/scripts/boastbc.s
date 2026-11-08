.CONST FN_BOAST "boastX"

# --- parse CLI args ... ~lowercase ... no fallthrough
# $$global $local variables
# winN - number required to win
# prob - probability of pirate giving a bad retort {1:prob}
# secs - seconds you get to retort [0->inf]
# porc - Pirate or Captain
SWITCH ~$arg
	a)  LET $$winN = 2 ; LET $$prob = 4 ; LET $$secs =  0 ; LET $$porc = "Pirate"
	b)  LET $$winN = 3 ; LET $$prob = 3 ; LET $$secs =  0 ; LET $$porc = "Pirate"
	c)  LET $$winN = 3 ; LET $$prob = 4 ; LET $$secs = 20 ; LET $$porc = "Pirate"
	d)  LET $$winN = 5 ; LET $$prob = 0 ; LET $$secs = 15 ; LET $$porc = "Captain"
	forget|reset)
		GET $r = FDELETE boast.var
		PRINT "Game is reset"
		EXIT
	*)
		PRINT "usage: RUN boast a|b|c|d|forget|reset"
		EXIT
END

# --- Number of boasts/"rows" [/data/boast.txt] ... first line: I=n
FSEEK FN_BOAST START
FREAD $l FN_BOAST EOL
IF MID($l, 1, 2) != "I="
	PRINT "${fnboast}.txt: first line must be I=n"
	EXIT
END
LET $$bCnt = MID($l, 3, LEN($l))

# --- Default State
LET $$kP = ""    # known Pirate boasts
LET $$kC = ""    # known Pirate boasts
LET $$kR = "0"   # known Retorts
LET $$uB = ""    # used boasts (this round)

# --- Load last State
ONERROR CONT     # no data/boast.var on the first run
VARS LOAD FN_BOAST
ONERROR STOP

# --- Initilalise game counters
LET $$scPl = 0   # score: Player
LET $$scPC = 0   # score: Pirate/Captain
LET $turn  = 0   # 0=pirate, 1=player

IF $$porc == "Pirate"
	LET $adj = "bedraggled|disheveled|dashing"
	LET $r = RND(1,$adj[0])
	PRINT "A $adj[$r] looking pirate lands near you."

	IF $arg ~= "b"  # b gets more wrong, but needs more wins
		PRINT "This crewman looks less educated, but more egocentric."
	END
	IF $arg ~= "c"  # c gets a time limit
		PRINT "This crewman looks rather impatient."
	END

	LET $r = RND(1,201)  # he/she global statistics!

ELSE
	PRINT "The resplendent captain lands near you."
	PRINT '"So. You have bested my crew. But did you truly master the art?"'
	LET $r = 10          # don't worry, he's a transvestite ;)
END

IF $r <= 3               # they/them global statistics!
	PRINT 'They draws their sword... "En garde!"'  # <sic>
ELIF $r <= 101
	PRINT 'He draws his sword... "En garde!"'
ELSE
	PRINT 'She draws her sword... "En garde!"'
END

# --- Play until we have a winner
LOOP FOREVER
	IF $turn == 0 ; CALL gameBoasts
	ELSE          ; CALL youBoast
	END

	VARS SAVE FN_BOAST $$kP $$kC $$kR  # save the Game State

	IF   $$scPl > $$scPC ; PRINT "[Score] $$scPl:$$scPC to you"
	ELIF $$scPl < $$scPC ; PRINT "[Score] $$scPC:$$scPl to the $$porc"
	ELSE                 ; PRINT "[Score] drawing $$scPC all"
	END
	PRINT "-------------------"
	WAIT 500

	LET $turn = 1 - $turn  # swap players

	# Do we have a winner?
	switch ~$arg
		a|b|c)
			IF ABS($$scPl - $$scPC) >= $$winN  # winner is set by a points difference
BPT!
				IF $$scPl > $$scPC
					LET $win = 1
					PRINT "The Pirate looks at you in fear, dives overboard, and swims back to the ship."
				ELSE
					LET $win = 0
					PRINT "The Pirate looks at you with pity. Turn around now, go back to where ye came from."
				END
				BREAK
			END
		d)
			IF $$scPl >= $$winN  # you can never win, you can just survive long enough
				LET $win = 1
				PRINT "The Captain stutters, lost for words. The crew panic, dive in to the sea, and turn in to dolphins."
				BREAK
			ELIF $$scPC > $$winN
				LET $win = 0
				PRINT 'The captain scoffs at you: "You are not ready for what lies ahead. Go back to whence you came."'
				BREAK
			END
		*)
			LET $win = 0
	END
END

LET $b = $$kP[0] - $$kC[0]
LET $r = $$kR[0] - 1        # -1 cos we don't count the 'free' (losing) retort
PRINT "You now know $b boasts and $r retorts."
EXIT

###############################################################################
# Retrieve the text of field $f={P, C, R} of duel $n
#
FN getText $n $f
	LET $f = MID($f,1,1)                   # boast.txt uses the first letter only
	FSEEK FN_BOAST TAG B=$n                # find the teg entry
	LOOP 4 TIMES                           # check the next few lines
		FREAD $l FN_BOAST EOL              # read a line
		IF MID($l, 1, 2) ~= "$f="          # starts with $f=
			RETURN MID($l, 3, LEN($l))     # return the text
		END
	END
	RETURN "I've forgotten how to boast!"  # entry not found
END

###############################################################################
# the game boasts, you retort (timed in modes c and d)
#
FN gameBoasts
	# --- Pirate boasts!
	IF $$uB[0] == $$bCnt ; LET $$uB = "" ; END    # sanity check in case we use up all the boasts
	LOOP FOREVER
		LET $b = RND(1, $$bCnt)                   # pick a random boast
		IF !FIND(" $$uB ", " $b ") ; BREAK ; END  # make sure it has not already been used this round
	END
	LET $$uB = $$uB $b                            # track used boasts

	LET $t = CALL getText $b $$porc               # get the Pirate/Captain boast text
	PRINT \"$t"

	# --- Track the number of every known boast
	# --- You just heard boast $b - add it to the list now
	IF $$porc == "Pirate"
		IF $$kP == "" ; LET $$kP = $b ; ELIF !FIND(" $$kP ", " $b ") ; LET $$kP = $$kP $b ; END
	ELSE
		IF $$kC == "" ; LET $$kC = $b ; ELIF !FIND(" $$kC ", " $b ") ; LET $$kC = $$kC $b ; END
	END

	# --- List the player's known retorts [$$kR]
	LOOP $i = 1 TO $$kR[0]
		LET $r = CALL getText $$kR[$i] Retort
		PRINT "  $i) $r"
	END

	# --- Get player's retort choice
	LET $end = $.runtime + ($$secs * 1000)  # some rounds are timed
	LOOP FOREVER
		IF $$secs
			LET $left = $end - $.runtime
			IF $left <= 0 ; BREAK ; END
			LET $secL = INT($left / 1000)
			INPUT "Choose your retort ($secL secs): " $in $left
		ELSE
			INPUT "Choose your retort: " $in
		END

		IF !$.ok
			IF $$secs AND $.runtime >= $end ; BREAK ; END  # Timer ran out
			PRINT "!! UI lost - exit" ; EXIT
		END

		# did they enter a valid number?
		IF $in > 0 AND $in <= $$kR[0] ; BREAK ; END
		PRINT "(enter 1..$$kR[0])"
	END

	# --- Out of time!
	IF !$.ok
		PRINT "You hesitate too long, and your ego is wounded!"
		LET $$scPC += 1
		RETURN
	END

	# --- Announce your retort
	LET $r = $$kR[$in]
	LET $t = CALL getText $r R
	PRINT 'You parry and retort: "$t"'

	# --- Decide the outcome
	IF $r == $b
		LET $$scPl += 1
	ELSE
		PRINT 'The $$porc scoffs: "Is that the best you\'ve got - LOL"'
		LET $$scPC += 1
	END
END

###############################################################################
# you boast, the game retorts (wrong 1:$$prob; never if $$prob is 0)
#
FN youBoast
	LET $known = $$kP $$kC

	# --- list player options
	PRINT "You look the $$porc in the eye and say:"
	LET $rCnt = 0
	LOOP $i = 1 TO $$kP[0]  # Pirate boasts
		$rCnt++
		LET $t = CALL getText $$kP[$i] Pirate
		PRINT "  $rCnt) $t"
	END
	LOOP $i = 1 TO $$kC[0]  # Captain boasts
		$rCnt++
		LET $t = CALL getText $$kC[$i] Captain
		PRINT "  $rCnt) $t"
	END

	# --- get player input
	LOOP FOREVER
		INPUT "Choose your boast: "$in
		IF !$.ok ; PRINT "!! UI lost - exit" ; EXIT ; END  # sanity check

		# did they enter a valid number?
		IF $in > 0 AND $in <= $known[0] ; BREAK ; END
		PRINT "(enter 1..$known[0])"
	END
	LET $b = $known[$in]  # input value -> boast number

	LET $txt = CALL getText $b $$porc
	PRINT 'You reprise and boast: "$txt"'

	# --- There is a 1-in-$$prob that a Pirate will get the wrong retort (captain never does)
	LET $r = $b
	IF $$prob AND (RND(1, $$prob) == 1)
		LOOP WHILE $r == $b ; LET $r = RND(0, $$bCnt) ; END
	END

	LET $txt = CALL getText $r Retort
	PRINT 'The $$porc retorts: "$txt"'

	# --- If you just learned a new retort, add it to $$kR [known retorts]
	IF $$kR == "" ; LET $$kR = $r ; ELIF !FIND(" $$kR ", " $r ") ; LET $$kR = $$kR $r ; END

	# --- Adjust the score
	IF $r == $b
		LET $$scPC += 1
	ELSE
		PRINT 'You look at the Pirate, confused. "What? And I though I was new to this! LOL"'
		LET $$scPl += 1
	END
END
