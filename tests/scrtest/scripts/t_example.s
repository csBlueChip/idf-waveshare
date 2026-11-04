# demo.s (spec example; events faked with EVT)
QUIET 1
ON BTN onBtn
ON IMU onImu
BTN 1 1 1 0
IMU TAP 1 1 0
LET $$run = 1
EVT BTN 1 SHORT
EVT IMU TAP
EVT IMU STILL
EVT BTN 1 DOUBLE
LOOP WHILE $$run
  WAIT EVENT
END
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
