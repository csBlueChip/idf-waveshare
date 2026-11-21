# tapzones.s logic with fake events (run by run.sh in a copy of sd/)
LET $$x = 2 ; LET $$y = 4 ; LET $$cw = 80 ; LET $$ch = 86 ; LET $$top = 0 ; LET $$r = 27
LET $$quit = 0 ; LET $$taps = 3 ; LET $$msg = "" ; LET $$guard = 0
EVT IMU ORIENT 1
EVT IMU TAP n:0 zone:0 gx:-40 gy:10 gz:1 a:2 sx:-80 sy:20 sz:2
EVT IMU TAP
EVT IMU TAP n:0 zone:0 gx:-44 gy:12 gz:1 a:2 sx:-88 sy:24 sz:2
EVT IMU TAP n:0 zone:0 gx:-36 gy:8 gz:1 a:2 sx:-72 sy:16 sz:2
CALL tapzones.trainZone 3 3
PRINT "trainZone: $.ret"
EVT BTN 1 SHORT
CALL tapzones.trainZone 4 3
PRINT "trainZone: $.ret"
EVT IMU TAP n:2 zone:4 gx:30 gy:5 gz:0 a:2 sx:60 sy:10 sz:0
EVT IMU 2TAP n:2 zone:4 gx:31 gy:4 gz:0 a:2 sx:62 sy:8 sz:0
EVT IMU TAP n:0 zone:0 gx:1 gy:1 gz:0 a:2 sx:2 sy:2 sz:0
EVT BTN 1 DOUBLE
CALL tapzones.test
PRINT "quit: $$quit"
FCAT tapzones.txt
