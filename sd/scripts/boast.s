# boast.s - boast and retort duel (duels in SD:/scripts/data/boast.txt)
#   RUN boast a|b   first to 2 points; the game misses 1 in 4 retorts
#   RUN boast c     first to 3; misses 1 in 8; 15 s to retort
#   RUN boast d     Captain boasts; first to 3; never misses; 10 s
#   RUN boast forget  forget everything learned
# The game boasts first, then turns alternate. Retort right: your point;
# wrong or too slow: the game's point. You learn a boast when the game
# says it, and a retort only when the game answers your boast.
# Learned: SD:/scripts/data/boast.var ($$kx Pirate boasts, $$ky Captain
# boasts, $$kr retorts: lists of duel tags; tag 0 "Oh yeah?" is free).
# At a prompt any text is an answer; !!stop stops the game.

SWITCH ~$arg
a|b)	LET $$n = 2 ; LET $$p = 4 ; LET $$s = 0 ; LET $$f = "P"
c)	LET $$n = 3 ; LET $$p = 8 ; LET $$s = 15 ; LET $$f = "P"
d)	LET $$n = 3 ; LET $$p = 0 ; LET $$s = 10 ; LET $$f = "C"
forget)
	GET $r = FDELETE boast.var
	PRINT "all boasts and retorts forgotten"
	EXIT
*)
	PRINT "usage: RUN boast a|b|c|d|forget"
	EXIT
END

# number of duels: I=n on the first line
FSEEK boast START
FREAD $l boast EOL
IF MID($l, 1, 2) != "I="
	PRINT "boast.txt: first line must be I=n"
	EXIT
END
LET $$rows = MID($l, 3, LEN($l))

LET $$kx = ""
LET $$ky = ""
LET $$kr = "0"
ONERROR CONT			# no boast.var on the first run
VARS LOAD boast
ONERROR STOP

LET $$you = 0
LET $$game = 0
LET $turn = 0
PRINT "First to $$n points. En garde!"
LOOP WHILE $$you < $$n AND $$game < $$n
	IF $turn == 0
		CALL gameBoasts
	ELSE
		CALL youBoast
	END
	LET $turn = 1 - $turn
	VARS SAVE boast $$kx $$ky $$kr
	PRINT "  You $$you - $$game Game"
END
IF $$you > $$game
	PRINT "You win!"
ELSE
	PRINT "The game wins."
END
LET $k = $$kr[0] - 1
PRINT "You know $k of $$rows retorts."
EXIT

# text of field P, C or R of duel $t
FN duel $t $f
	FSEEK boast TAG B=$t
	LOOP 6 TIMES
		FREAD $l boast EOL
		IF MID($l, 1, 2) == "$f="
			RETURN MID($l, 3, LEN($l))
		END
	END
	RETURN "?"
END

# the game boasts, you retort (timed in modes c and d)
FN gameBoasts
	LET $b = RND(1, $$rows)
	LET $t = CALL duel $b $$f
	PRINT "Game: $t"
	IF $$f == "P"
		IF $$kx == "" ; LET $$kx = $b ; ELIF !FIND(" $$kx ", " $b ") ; LET $$kx = $$kx $b ; END
	ELSE
		IF $$ky == "" ; LET $$ky = $b ; ELIF !FIND(" $$ky ", " $b ") ; LET $$ky = $$ky $b ; END
	END
	LOOP $i = 1 TO $$kr[0]
		LET $r = $$kr[$i]
		LET $t = CALL duel $r R
		PRINT "  $i) $t"
	END
	LET $end = $.runtime + $$s * 1000
	LOOP FOREVER
		IF $$s
			LET $left = $end - $.runtime
			IF $left <= 0 ; BREAK ; END
			PRINT "retort? ($$s s)"
			INPUT $a $left
		ELSE
			PRINT "retort?"
			INPUT $a
		END
		IF !$.ok
			IF $$s AND $.runtime >= $end ; BREAK ; END
			PRINT "no client - game over"
			EXIT
		END
		LOOP $i = 1 TO $$kr[0]
			IF $a == $i ; BREAK 2 ; END
		END
		PRINT "pick 1..$$kr[0]"
	END
	IF $i > $$kr[0] OR !$.ok
		PRINT "Game: Too slow!"
		LET $$game += 1
		RETURN
	END
	LET $r = $$kr[$i]
	LET $t = CALL duel $r R
	PRINT "You: $t"
	IF $r == $b
		LET $$you += 1
	ELSE
		LET $$game += 1
	END
END

# you boast, the game retorts (wrong 1 in $$p; never if $$p is 0)
FN youBoast
	IF $$f == "P" ; LET $known = $$kx ; ELSE ; LET $known = $$ky ; END
	LOOP $i = 1 TO $known[0]
		LET $b = $known[$i]
		LET $t = CALL duel $b $$f
		PRINT "  $i) $t"
	END
	LOOP FOREVER
		PRINT "boast?"
		INPUT $a
		IF !$.ok
			PRINT "no client - game over"
			EXIT
		END
		LOOP $i = 1 TO $known[0]
			IF $a == $i ; BREAK 2 ; END
		END
		PRINT "pick 1..$known[0]"
	END
	LET $b = $known[$i]
	LET $t = CALL duel $b $$f
	PRINT "You: $t"
	LET $r = $b
	IF $$p AND RND(1, $$p) == 1
		LOOP WHILE $r == $b
			LET $r = RND(0, $$rows)
		END
	END
	LET $t = CALL duel $r R
	PRINT "Game: $t"
	IF $$kr == "" ; LET $$kr = $r ; ELIF !FIND(" $$kr ", " $r ") ; LET $$kr = $$kr $r ; END
	IF $r == $b
		LET $$game += 1
	ELSE
		LET $$you += 1
	END
END
