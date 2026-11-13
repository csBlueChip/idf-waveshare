#
# Boasts and Retorts
# ==================
# Homer's Iliad is full of of warriors trading speeches before single combat.
# They boast of their ancestry and strength and mock the other man's.
# E.G. Aeneas and Achilles do it at length before fighting in Book 20, and Aeneas even says they could trade insults all day.
#
# Set with a side-order of swashbuckling metaphor, you will battle the pirates 4 (or more) times,
# each battle more intense that the last.
# Remember the boasts thrown out by the pirates, and pick the correct retort from those you have learned.
# To learn the retorts, use the boasts you can remember, and learn the corrrect retort (probably).
# A good retort will win a point; a bad retort will give a point to the boaster.
# The exact rules are different for each round, but the pattern is consistent.
# In round #4 you will meet the Captain, he has different boasts,
# but each can be beaten by a retort you (may have) learned from a crewman.  [qv. data/boasts.txt]
# If you missed a crewman retort, the only way to learn it now is by using the Captain's boast back at him.
# The Captain is never wrong, so you cannot "beat" the Captain per-se, and retort badly, even once, and you will lose.
# However, if you stand your ground long enough, he will acknowledge your guile, and you will "win"
#

.CONST FN_BOAST "boastbc"  # txt and var file

# RUN boastbc <mode> [seed]
# If a seed is provided, use the PRNG (not TRNG) and seed it as requested
# ...the means the emulator can produce identical results
IF $arg[0] >= 2 ; RNG PRNG $arg[2] ; END

# --- parse CLI args ... ~lowercase ... no fallthrough
# $$global $local variables
# winN - advantage required to win
# prob - probability of pirate giving a bad retort {1:prob}
# secs - seconds you get to retort [0->inf]
# porc - Pirate or Captain
SWITCH ~$arg[1]
	a)  LET $$winN = 2 ; LET $$prob = 3 ; LET $$secs =  0 ; LET $$porc = "Pirate"
	b)  LET $$winN = 3 ; LET $$prob = 3 ; LET $$secs =  0 ; LET $$porc = "Pirate"
	c)  LET $$winN = 3 ; LET $$prob = 4 ; LET $$secs = 15 ; LET $$porc = "Pirate"
	d)  LET $$winN = 5 ; LET $$prob = 0 ; LET $$secs = 15 ; LET $$porc = "Captain"
	forget|reset)
		LET $f = FN_BOAST
		GET $r = FDELETE ${f}.var
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
	PRINT "ERROR: ${fnboast}.txt - missing I=n"
	EXIT
END
LET $$bCnt = MID($l, 3, LEN($l))

# --- Default State
LET $$kP = ""    # known Pirate boasts
LET $$kC = ""    # known Pirate boasts
LET $$kR = "0"   # known Retorts (0 is the free one, so you can play round #1 - and lose)
LET $$uB = ""    # used boasts (this round) just to avoid over-repetition

# --- Load last State
ONERROR CONT     # no data/boast.var on the first run
VARS LOAD FN_BOAST
ONERROR STOP

# --- Initilalise game counters
LET $$scPl = 0   # score: Player
LET $$scPC = 0   # score: Pirate/Captain
LET $turn  = 0   # 0=pirate, 1=player

# --- Choose a pirate
IF $$porc == "Pirate"
	LET $adj = "bedraggled|disheveled|dashing"
	LET $r   = RND(1,$adj[0])
	PRINT "* A $adj[$r] looking pirate lands near you."

	IF $arg ~= "c"       # c gets a time limit
		PRINT "  This crewman seems more eager."
	END

	LET $r = RND(1,201)  # random gender

ELSE
	PRINT "* The resplendent captain lands near you."
	PRINT '* "So. You have bested my crew. But did you truly master the art?"'
	LET $r = 10          # don't worry, he's a sweet transvestite ;)
END

# Gender by global statistics!
# They were actually all men in this era, but I was exercising the scripting language.
IF   $r <= 3   ; LET $$pg = "They|their"
ELIF $r <= 101 ; LET $$pg = "He|his"
ELSE           ; LET $$pg = "She|her"
END
PRINT '  $$pg[1] draws $$pg[2] sword...'  # <sic>

# --- Play until we have a winner
LOOP FOREVER
	IF $turn == 0 ; CALL gameBoasts
	ELSE          ; CALL youBoast
	END

	VARS SAVE FN_BOAST $$kP $$kC $$kR  # save the Game State

	IF   $$scPl > $$scPC ; PRINT "\n----- [Score] $$scPl:$$scPC to you -----\n"
	ELIF $$scPl < $$scPC ; PRINT "\n----- [Score] $$scPC:$$scPl to the $$porc -----\n"
	ELSE                 ; PRINT "\n----- [Score] drawing $$scPC all -----\n"
	END

	WAIT 500

	# Do we have a winner?
	switch ~$arg
		a|b|c)
			IF ABS($$scPl - $$scPC) >= $$winN  # winner is set by a points difference
				IF $$scPl > $$scPC
					LET $win = 1
					PRINT "*** The Pirate looks at you in fear, dives overboard, and swims back to the ship."
				ELSE
					LET $win = 0
					PRINT "!!! The Pirate looks at you with pity. Turn around now, go back to where ye came from."
				END
				BREAK
			END
		d)
			IF $$scPl >= $$winN  # you can never win, you can just survive long enough
				LET $win = 1
				PRINT "*** The Captain stutters, lost for words. The crew panic, dive in to the sea, and turn in to dolphins."
				BREAK
			ELIF $$scPC >= $$scPl+2
				LET $win = 0
				PRINT '!!! The captain scoffs at you: "You are not ready for what lies ahead. Enjoy your life on Keos."'
				BREAK
			END
		*)
			LET $win = 0
	END

	LET $turn = !$turn  # swap players
END

LET $b = $$kP[0] + $$kC[0]
LET $r = $$kR[0] - 1        # -1 cos we don't count the 'free' (losing) retort
PRINT "(You now know $b boasts and $r retorts)"
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
#	PRINT 'The $$porc stares you down: "$t"'
	LET $r = RND(1,3)
	IF   $r == 1   ; LET $r = "Allez..."
	ELIF $r == 2   ; LET $r = "Defend your ego..."
	ELSE           ; LET $r = "En Garde.."
	END
	PRINT '> "$r $t"'

	# --- Track the number of every known boast
	# --- You just heard boast $b - add it to the list now
	IF $$porc == "Pirate"
		IF   $$kP == ""              ; LET $$kP = $b
		ELIF !FIND(" $$kP ", " $b ") ; LET $$kP = $$kP $b
		END
	ELSE
		IF   $$kC == ""              ; LET $$kC = $b
		ELIF !FIND(" $$kC ", " $b ") ; LET $$kC = $$kC $b
		END
	END

	# --- Maximum 5 retort options
	LET $krx = $$kR                             # take a copy of known-retorts
	IF $krx[0] > 5 ; DEL $krx[1] ; END          # more than 5, the first we ditch is the default
	LOOP WHILE $krx[0] > 5                      # maybe ditch more until we're down to 5
		LET $r = RND(1,$krx[0])
		IF $krx[$r] != $b ; DEL $krx[$r] ; END  # only remove wrong answers
	END

	# --- List the player's known retorts [$krx]
	PRINT "* Choose your retort: "
	LOOP $i = 1 TO $krx[0]
		LET $r = CALL getText $krx[$i] Retort
		PRINT "   $i) $r"
	END

	# --- Get player's retort choice
	LET $end = $.runtime + ($$secs * 1000)  # some rounds are timed
	LOOP FOREVER
		IF $$secs
			LET $left = $end - $.runtime
			IF $left <= 0 ; BREAK ; END
			LET $secL = INT($left / 1000)
			INPUT "($secL secs)>" $in $left
		ELSE
			INPUT ">" $in  # choose retort
		END

		IF !$.ok
			IF $$secs AND $.runtime >= $end ; BREAK ; END  # Timer ran out
			PRINT "!! UI lost - exit" ; EXIT
		END

		# did they enter a valid number?
		IF $in > 0 AND $in <= $krx[0] ; BREAK ; END  # yes
#		PRINT "(enter 1..$krx[0])"
	END

	# --- Out of time!
	IF !$.ok
		PRINT "   You hesitate too long, and your ego is wounded!"
		LET $$scPC += 1
		RETURN
	END

	# --- Announce your retort
	LET $r = $krx[$in]
	LET $t = CALL getText $r R
	PRINT '   You parry and retort:'
	PRINT '   "$t"'

	# --- Decide the outcome
	IF $r == $b
		LET $$scPl += 1
	ELSE
		PRINT '...The $$porc scoffs "Is that the best you\'ve got - LOL"'
		LET $$scPC += 1
	END
END

###############################################################################
# you boast, the game retorts (wrong 1:$$prob; never if $$prob is 0)
#
FN youBoast
	LET $known = $$kP $$kC

	# --- list player options
	PRINT "* You look the $$porc in the eye and say:"
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
		INPUT ">"$in
		IF !$.ok ; PRINT "!! UI lost - exit" ; EXIT ; END  # sanity check

		# did they enter a valid number?
		IF $in > 0 AND $in <= $known[0] ; BREAK ; END
#		PRINT "(enter 1..$known[0])"
	END
	LET $b = $known[$in]  # input value -> boast number

	LET $txt = CALL getText $b $$porc
#	PRINT 'You reprise and boast: "$txt"'
	PRINT '   "$txt"'

	# --- There is a 1-in-$$prob that a Pirate will get the wrong retort (captain never does)
	LET $r = $b

	# This makes your choice of boast meaningful (albeit essentially still random)
	LOOP $r TIMES ; LET $xx = RND(1, $$prob) ; END

	IF $$prob AND (RND(1, $$prob) == 1)
		LOOP WHILE $r == $b ; LET $r = RND(0, $$bCnt) ; END
	END

	LET $txt = CALL getText $r Retort
	PRINT '   $$pg[1] retorts: "$txt"'

	# --- If you just learned a new retort, add it to $$kR [known retorts]
	IF   $$kR == ""              ; LET $$kR = $r
	ELIF !FIND(" $$kR ", " $r ") ; LET $$kR = $$kR $r
	END

	# --- Adjust the score
	IF $r == $b
		LET $$scPC += 1
	ELSE
		PRINT '...You look at the Pirate, confused. "What? And I though I was new to this! LOL"'
		LET $$scPl += 1
	END
END
