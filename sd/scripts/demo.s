# demo.s - spec example: BOOT short = hexagon, double = end; tap prints
ON BTN onBtn
ON IMU onImu
BTN 1 1 0 1
IMU TAP 1 1 0
CLS 0 0 0
PRINT "demo: BOOT short = hexagon, BOOT double = end, tap the board"
LET $$run = 1
LOOP WHILE $$run
  WAIT EVENT
END
IMU TAP NONE
BTN 1 0 0 0
EXIT

FN onBtn
  SWITCH $arg[2]
  SHORT)  CALL shapes.drawPoly 6 86 160 40
  DOUBLE) LET $$run = 0
  END
END

FN onImu
  SWITCH $arg[1]
  TAP)   PRINT "tap - ax=$.ax"
  2TAP)  CLS 0 0 0
  *)     DEBUG ignored $arg
  END
END
