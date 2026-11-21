# tapzones.s - tap the screen: which circle was it? (IMU tap zones)
#   RUN tapzones [X Y [taps]]   X columns x Y rows of zones as at ROT 0 (the
#                               IMU TAP XY grid; X*Y <= 16; default 2 4 =
#                               4 across x 2 down when held landscape),
#                               taps per circle when training (5)
# Held landscape: everything is drawn at ROT 1, IMU ORIENT L AUTO turns the
# picture when the board is turned round.
# Uses the latest SD:/sys/imutap_XxY_*.cal if there is one, else trains:
# tap each yellow circle; the centres (gx/a : gy/a : gz/a) are stored (IMU TAP
# n CAL, IMU TAP SAVE).
# Then test: every tap lights its circle and shows n: (as seen now) and zone:.
# BOOT short = train again, BOOT double = end. IMU settings are put back.
# Every tap with data is logged to <data>/tapzones.txt (FCAT tapzones.txt):
#   \# <time> X Y taps     at the start of each run
#   phase,target,zone,n,gx,gy,gz,a,sx,sy,sz
#   e.g. train,3,0,0,-88.0,30.0,6.0,2.40,-41.2,12.8,3.1   (sx sy sz: first swing)
# phase train (target = the yellow circle) or test (target 0: free taps);
# zone / n as reported (0 while training). Plot gx/a against gy/a per target.

VARS $$x $$y $$taps $$quit $$cw $$ch $$top $$r $$msg $$guard $ar $at $gr $gt $s

LET $$x = 2 ; LET $$y = 4 ; LET $$taps = 5 ; LET $$quit = 0 ; LET $$msg = "" ; LET $$guard = 500
IF $arg[0] >= 2
	LET $$x = INT($arg[1]) ; LET $$y = INT($arg[2])
END
IF $arg[0] >= 3
	LET $$taps = INT($arg[3])
END
IF $$x < 1 OR $$y < 1 OR $$x * $$y > 16 OR $$taps < 1
	PRINT "usage: RUN tapzones [X Y [taps]] (X * Y 1..16)"
	EXIT
END

# keep the IMU settings, then 8 g / 2048 dps at 896 Hz (capture needs equal rates)
GET $s = IMU ACC
LET $ar = $s[2] ; LET $at = $s[4]
GET $s = IMU GYR
LET $gr = $s[2] ; LET $gt = $s[4]
IMU ACC 8 896
IMU GYR 2048 896
BTN 1 1 0 1

CALL setup
FAPP tapzones.txt \# $.time $$x $$y $$taps
FAPP tapzones.txt phase,target,zone,n,gx,gy,gz,a,sx,sy,sz
IMU TAP XY $$x $$y			# loads the latest calibration for X x Y, if any
GET $s = IMU TAP
IF $s[0] < 8 OR $s[8] != "ok"
	CALL train
END
LOOP WHILE !$$quit
	CALL test
	IF !$$quit
		CALL train
	END
END

IMU TAP NONE
IMU TAP XY 1 1
IMU ORIENT NONE
IMU ACC $ar $at
IMU GYR $gr $gt
BTN 1 0 0 0
TCLS 1
CLS 0,0,0
PRINT "tapzones: end"
EXIT

# ---------- screen (ROT 1, 320 x 172) ----------
# Zones are physical (ROT 0: X columns, Y rows). At ROT 1 the ROT-0 rows run
# left to right and the ROT-0 columns top to bottom, right column first; each
# circle is at the centre of its zone. The text line sits on a boundary
# between rows: (X >> 1) / X down for an odd X > 1, else half way down.
FN setup
	VARS $$x $$y $$cw $$ch $$top $$r $ty
	ROT 1
	CLS 0,0,0
	LET $$top = 0 ; LET $$ch = 172 / $$x ; LET $$cw = 320 / $$y
	IF $$x % 2 == 1 AND $$x != 1
		LET $ty = ROUND(INT($$x / 2) / $$x * 172) - 13
	ELSE
		LET $ty = 86 - 13
	END
	TBOX 1 0 $ty 320 1 0 1
	LET $$r = MAX(8, INT(MIN($$cw, $$ch) / 2) - 13)
	IMU ORIENT L AUTO
	CALL drawAll 50,50,50
END

# filled circle for physical zone z
FN circle $z $col
	VARS $c $rw $cx $cy $$x $$cw $$ch $$top $$r
	LET $c = ($z - 1) % $$x ; LET $rw = INT(($z - 1) / $$x)
	LET $cx = ROUND($$cw * ($rw + 0.5))
	LET $cy = ROUND($$top + $$ch * ($$x - 1 - $c + 0.5))
	FCIRC $cx $cy $$r $col
END

FN drawAll $col
	VARS $z $$x $$y
	LOOP $z = 1 TO $$x * $$y
		CALL circle $z $col
	END
	CALL show
END

# the text line, always drawn after the circles
FN show
	VARS $$msg
	TCLS 1
	TEXT 1 "$$msg"
END

# ---------- training: each circle in turn ----------
FN train
	VARS $z $res $$x $$y $$taps $$quit $$msg
	IMU TAP NONE
	IMU TAP CAL START
	IMU TAP * 1 0 0
	CALL drawAll 50,50,50
	LET $z = 1
	LOOP WHILE $z <= $$x * $$y
		CALL circle $z 255,200,0
		CALL trainZone $z $$taps
		LET $res = $.ret
		IF $$quit
			BREAK
		END
		IF $res == "retrain"
			CALL drawAll 50,50,50
			LET $z = 1
		ELSE
			CALL circle $z 0,90,0
			CALL show
			LET $z += 1
		END
	END
	IMU TAP CAL STOP
	IMU TAP NONE
	IF !$$quit
		IMU TAP SAVE
		LET $$msg = "trained - now tap any circle"
		CALL show
	END
END

# collect taps for zone z: average gx/a, gy/a, gz/a -> IMU TAP z CAL x:y:z
# (taps in the first $$guard ms (0.5 s) after the circle moves are ignored:
# still aimed at the last one)
FN trainZone $z $n
	VARS $k $e $sx $sy $sz $a $cx $cy $cz $t0 $$quit $$msg $$guard
	LET $k = 0 ; LET $sx = 0 ; LET $sy = 0 ; LET $sz = 0 ; LET $t0 = $.uptime
	LET $$msg = "zone $z: tap the yellow circle ${n}x"
	CALL show
	LOOP WHILE $k < $n
		WAIT EVENT
		LET $e = $.event
		IF $e[1] == "BTN" AND $e[0] >= 3
			IF $e[3] == "DOUBLE"
				LET $$quit = 1
				RETURN "quit"
			ELIF $e[3] == "SHORT"
				RETURN "retrain"
			END
		ELIF $e[1] == "IMU" AND $e[2] == "TAP" AND $.uptime - $t0 >= $$guard
			IF $e[0] < 20
				LET $$msg = "no tap data: rates must match"
				CALL show
			ELIF $e[14] > 0
				LET $a = $e[14]
				FAPP tapzones.txt train,$z,$e[6],$e[4],$e[8],$e[10],$e[12],$a,$e[16],$e[18],$e[20]
				LET $sx += $e[8] / $a ; LET $sy += $e[10] / $a ; LET $sz += $e[12] / $a
				LET $k += 1
				LET $$msg = "zone $z: $k of $n"
				CALL show
			END
		END
	END
	LET $cx = $sx / $n ; LET $cy = $sy / $n ; LET $cz = $sz / $n
	IMU TAP $z CAL $cx:$cy:$cz
	RETURN "ok"
END

# ---------- test: light the circle of every tap ----------
FN test
	VARS $e $z $n $$quit $$msg
	IMU TAP * 1 1 1
	IF !$.ok
		LET $$msg = "tap: $.err"
		CALL show
		LET $$quit = 1
		RETURN
	END
	CALL drawAll 50,50,50
	LOOP FOREVER
		WAIT EVENT
		LET $e = $.event
		IF $e[1] == "BTN" AND $e[0] >= 3
			IF $e[3] == "DOUBLE"
				LET $$quit = 1
				IMU TAP NONE
				RETURN
			ELIF $e[3] == "SHORT"
				IMU TAP NONE
				RETURN
			END
		ELIF $e[1] == "IMU" AND $e[2] != "ORIENT" AND $e[0] >= 6
			LET $n = $e[4] ; LET $z = $e[6]
			IF $e[0] >= 20
				FAPP tapzones.txt test,0,$z,$n,$e[8],$e[10],$e[12],$e[14],$e[16],$e[18],$e[20]
			END
			IF $z == 0
				LET $$msg = "$e[2]: between zones"
				CALL drawAll 50,50,50
			ELSE
				LET $$msg = "$e[2] n:$n zone:$z"
				CALL drawAll 50,50,50
				CALL circle $z 0,200,255
				CALL show
			END
		END
	END
END
