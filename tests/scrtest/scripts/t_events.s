# events, handlers, WAIT EVENT, TIMER, INPUT
QUIET 1
EVT BTN 1 SHORT
EVT IMU TAP
WAIT EVENT
PRINT 1st: $.event ret=[$.ret]
ON IMU onImu
EVT IMU 2TAP
EVT BTN 2 DOUBLE
PRINT line after events
WAIT EVENT 100
PRINT waited: $.event
EVT IMU TAP
EVT IMU MOTION
WAIT EVENT 50
PRINT after handlers: $.event ret=[$.ret]
TIMER 1 100
WAIT 350
WAIT EVENT
PRINT merged: $.event
WAIT EVENT
PRINT next tick: $.event uptime ok
TIMER 1 0
WAIT EVENT 500
PRINT timeout: [$.event]
ON TIMER onTimer
TIMER 2 40
LET $$ticks = 0
LOOP WHILE $$ticks < 3
  WAIT EVENT
  PRINT got $.event ret=$.ret
END
TIMER 2 0
ON TIMER OFF
ON IMU OFF
XINPUT hello there
INPUT $in 1000
PRINT input: [$in] ok=$.ok
INPUT $in 1000
PRINT input timeout: [$in] ok=$.ok
EVT UI LINK UP
ON UI onUi
WAIT 1
PRINT dropped=$.dropped
UI WEB
PRINT never: UI switch ends the script
FN onImu
  PRINT handler IMU [$arg] 1=$arg[1]
  RETURN "imu:" $arg[1]
END
FN onTimer
  $$ticks++
  RETURN $arg[1]
END
FN onUi
  PRINT ui handler: $arg
END
