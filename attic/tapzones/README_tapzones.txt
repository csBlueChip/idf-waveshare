Tap zones - documentation removed at 0.7.58 (from 0.7.57). See RESTORE.txt.

== README.txt section 2, IMU commands
  IMU TAP n|* s d t             tap events of zone n (* = all zones):
                                single, double, triple 0|1
  IMU TAP NONE                  all zones off
  IMU TAP                       x:2 y:4 mode:run cal:ok, then one line
                                per zone (as IMU TAP n)
  IMU TAP n                     single:1 double:0 triple:0 zone:2
                                cal:-0.412:0.128:0.051
  IMU TAP XY [x y]              tap zones: x columns by y rows as at ROT 0
                                (x * y 1..16; default 1 1 = the whole
                                board, as before). A new XY turns the
                                zones off and loads the latest
                                SD:/sys/imutap_XxY_*.cal
  IMU TAP CAL START|STOP        calibration mode: every tap is <<IMU TAP
                                n:0 zone:0 gx:.. gy:.. gz:.. a:..>>
  IMU TAP n CAL x:y:z           store zone n's centre (NV imu.tap.zoneN:
                                gx/a : gy/a : gz/a of its taps; x:y = z 0;
                                0:0:0 = none). The
                                first one for a new XY clears the others
  IMU TAP SAVE [file]           calibration -> SD:/sys/file.cal, or
                                imutap_XxY_NNNN_<timestamp>.cal (NNNN =
                                highest so far + 1)
  IMU TAP LOAD [file]           SD:/sys/file.cal, or the latest
                                imutap_XxY_*.cal for the current XY; its
                                first line imu.tap.xy=x:y sets XY (an
                                imutap_XxY_ name must agree with it)
  Tap zones: while a zone is on and imu.arate = imu.grate, the chip FIFO
  is copied to a 0.5 s ring every 20 ms; each tap carries the largest
  gyro samples around its acceleration spike (gx gy gz, dps) and the
  spike (a, g); sx sy sz = the first swing: the mean gyro over
  imu.tap.win ms from the start of the spike, minus the 20 ms before it
  (EXPERIMENT, kept for comparison):
    <<IMU TAP n:2 zone:4 gx:-88.0 gy:30.0 gz:6.0 a:2.40 sx:-41.2 sy:12.8 sz:3.1>>
  (2TAP / 3TAP too). Taps weaker than imu.tap.min send nothing; a chip
  "double" seen in one poll is two taps only with two spikes >= 80 ms
  apart. A spike is used once: a chip tap with no new spike (>= 80 ms
  after the last one used) is its rebound and sends nothing.
  zone: the physical zone (ROT 0 numbering), the nearest calibrated
  centre to (gx/a, gy/a, gz/a); n: its number in reading order as the screen
  is shown now (ROT, plus the ORIENT AUTO flip). n:0 zone:0 = between
  two zones (nearest not nearer than imu.tap.dead x the second) or no
  data. 2TAP / 3TAP only count taps in the same zone. XY > 1 needs a
  complete calibration (else ERR no calibration data) and equal rates.
  Without capture (rates differ, IMU REC running) events are plain
  <<IMU TAP>>. See sd/scripts/tapzones.s (held landscape: ROT 1, IMU
  ORIENT L AUTO) and tests/test_tapzones.txt.
    IMU TAP 1   single:1 double:0 triple:0 zone:1 cal:0:0:0

== README.txt NV keys
  imu.tap.min 1.8            tap zones: weaker taps (g) send nothing (1.0..16.0)
  imu.tap.win 4              tap zones: first-swing window, ms (1..30)
  imu.tap.dead 0.7           tap zones: a tap whose nearest centre is not
                             nearer than 0.7 x the second is zone 0
                             (0.1..1.0; 1.0 = always the nearest)
  imu.tap.xy  1:1            the grid of the stored tap calibration (ROT 0)
  imu.tap.zone1..16  0:0:0   tap zone centres gx/a:gy/a:gz/a (IMU TAP n
                             CAL / LOAD; 0:0:0 = not calibrated)

== src/imu.h
 *   IMU TAP n|* s d t | NONE  <<IMU TAP>> <<IMU 2TAP>> <<IMU 3TAP>> per tap zone;
 *                             IMU TAP n shows single double triple zone cal
 *   IMU TAP XY [x y] | CAL START|STOP | n CAL x:y | SAVE [f] | LOAD [f]
 *                             tap zones (ROT 0 grid, NV imu.tap.zoneN, .cal files in SD:/sys);
 *                             events then carry n: zone: gx: gy: gz: a:

== TODO.txt
 36. Board test - tap zones: RUN tapzones (2x4): trains (yellow circle,
     5 taps each; events n:0 zone:0 gx gy gz a), saves
     SD:/sys/imutap_2x4_0001_<stamp>.cal; test: each tap lights its
     circle, n: / zone: right at ROT 0..3 and with the ORIENT AUTO flip;
     taps between circles -> zone:0; 2TAP only within one zone. Run again:
     no training (file loaded). IMU TAP XY 3 3 -> no file -> IMU TAP * 1
     0 0 = ERR no calibration data. IMU TAP SAVE fred / LOAD fred;
     renamed imutap_3x3_... holding 2:4 -> ERR. IMU REC during taps: plain
     events. imu.arate != grate: plain events (1x1) / ERR (XY > 1). IMU
     task idle (no polling) with nothing on: check with MEM / a scope on
     I2C. How well do zones separate (hand-held / on a table)?
     EXPERIMENT (0.7.55, remove if it leads nowhere): first-swing gx gy gz
     (imu.tap.win, baseline), px py pz old peaks in the event, imu.tap.min,
     chip double split by spikes, tapzones.s 0.5 s guard + px py pz in
     tapzones.txt. Compare angle tables (nail / pad) old vs new.
     0.7.56: classifier in 3D (gz/a too: zone CAL x:y:z); SCRIPT_EV_TEXT
     64 -> 128 (script.c, outside IMU TAP: event text was cut for
     scripts) - keep even if the experiment goes.
     0.7.57: classify on the old peaks again (gx gy gz; first swing sent
     as sx sy sz); a spike is used once (s_spikeLast, s_ringTotal): a
     chip tap with no new spike = rebound, no event. Test: RUN tapzones
     2 2 12.

== DESCRIPT.ION lines
tapzones.s tap zones: X x Y circles, train (IMU TAP CAL), then each tap lights its circle: RUN tapzones [X Y [taps]]
test_tapzones.txt IMU tap zones (IMU TAP XY / CAL / SAVE / LOAD, RUN tapzones)
tz_check.s calls tapzones.s functions with fake <<IMU TAP ...>> events (training average, test display, tapzones.txt log)
tz_check.out expected output of tz_check.s

== tests/scrtest/run.sh (tz_check block)
# tapzones.s functions with fake IMU TAP events (card layout, sd/scripts/tapzones.s)
tmp=$(mktemp -d)
cp -R "$here/../../sd/." "$tmp/"
cp "$here/card/tz_check.s" "$tmp/scripts/"
"$out" "$tmp" tz_check > "$here/card/tz_check.got" 2>&1 || true
rm -rf "$tmp"
if [ "$1" = "-update" ]; then
	cp "$here/card/tz_check.got" "$here/card/tz_check.out"
fi
