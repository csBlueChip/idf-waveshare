WAVEDEMO - Waveshare ESP32-S3-LCD-1.47B resource demo
=====================================================

Firmware 0.5.0, ESP-IDF v6.1. Command console over BLE, telnet or web.


1. CONSOLE
----------

Interfaces (one active at a time, boot choice in NV ui.boot):

  BLE      Nordic UART Service, device name from NV rad.ble
  TELNET   Wi-Fi AP, telnet port 23
  WEB      Wi-Fi AP, captive-portal web console at http://<rad.ip>/
  UART     UART0 on the header (TX GPIO43, RX GPIO44), 8N1, NV ui.baud
  USB      USB-C port (the chip's USB-Serial/JTAG)

At start-up the LED flashes for 100 ms: sys.aron colour if sys.autorun is 1,
else sys.aroff. The interface starts at the end of start-up, after the
onBoot script (sys.autorun 1); until then only the Boot screen.

Rules:

  - One command per line, words separated by spaces.
  - Command names are case-insensitive.
  - Success replies OK; failure replies ERR <reason>.
  - Unknown commands reply: unknown |<line>|
  - '#' starts a comment; "\#" is a literal '#'.
  - Blank and comment-only lines are ignored silently.
  - Text arguments (ECHO, PRINT, DEBUG, FLOG; FWRITE in scripts):
      text starts after one separating space; further spaces are kept
      "text" or 'text'   quoted (leading spaces before the quote dropped);
                         \" inside "..." / \' inside '...' = the quote;
                         anything but blanks after the closing quote = ERR
      \"text / \'text    not quoted, starts with a quote
      escapes            \n new line, \\ \# \$ literal; \1..\9 font
                         (ECHO only, dropped elsewhere)
    '#' starts a comment even inside quotes: write \#.
  - Max line 200 chars, max 18 arguments.
  - All I/O is ASCII ('^' is used for degrees).
  - The screen updates after every command (see SCR AUTO).


2. COMMANDS
-----------

General

  HELP                          list commands
  INFO                          chip, memory, MACs, firmware, partition,
                                reset reason, timestamp, uptime, SD card size
  WAIT ms                       pause 0..60000 ms, 1 ms resolution
                                (blocks the client's other commands)
  REBOOT                        reply OK, then restart
  MEM                           RAM: internal / PSRAM free, lowest, largest
                                block; each task's lowest free stack
  PRINT text                    reply text (text rules: section 1)
  DEBUG text                    text to the log port(s), as dbgf()
Data files (text files SD:/scripts/data/file.txt; file = letters, digits,
'_', max 31, given without .txt)

  FLOG file text                add text plus a new line at the end of the
                                file (folders and file created)
  FCAT file [start,count] [>DEBUG]
                                show the file, or count lines from line
                                start (1-based); to the log port(s) with
                                >DEBUG; CR dropped, non-ASCII shown as '?',
                                lines over 200 chars split
  FDIR [txt|var|nvram|*]        list the files: name, size, ro|rw (default
                                txt; * lists all three, with extensions)
  FSTAT file                    size:ro or size:rw (ro = read-only, set on
                                a PC); in a script the reply goes to $.ret
  FDELETE file                  delete the file (not a read-only one)
  FCAT / FSTAT / FDELETE: file = name (name.txt), name.var
  (SD:/scripts/data, VARS SAVE files) or name.nvram (SD:/sys). In a .nvram
  file FCAT shows a line starting with '!' as !key=[redacted].
  FLOG and the script commands FWRITE / FREAD / FTELL / FSEEK / FCLOSE
  (section 9) use .txt files only.
  No SD card, a missing file or a read-only file is an ERR.

  LOG                           show where logs go (section 7)
  LOG UART|USB|BOTH|AUTO        set it (this boot only; default AUTO)
  EVENTS                        show which events the client receives:
                                BTN 1 IMU 1 UI 1 SCRIPT 1
  EVENTS src 0|1                src = BTN IMU UI SCRIPT ALL; 0 = the client
                                gets no <<src ...>> events (this boot only;
                                scripts will always get every event)

Interface

  UI                            show active interface
  UI BLE                        switch to BLE
  UI TELNET                     reply SSID/PWD/IP/PORT, switch to telnet
  UI WEB                        reply SSID/PWD/URL, switch to web console
  UI UART                       reply BAUD/PINS, switch to the UART console
  UI USB                        switch to the USB serial console
  UI NEXT                       switch to the next interface: WEB ->
                                TELNET -> BLE -> USB -> UART -> WEB
                                (as BOOT double press; ui.boot unchanged)
    Events (to the client):
    <<UI CONNECT>>              first message of a new session
    <<UI DISCONNECT>>           session ended (no client left to see it;
                                for scripts)
    <<UI LINK UP>>              a Wi-Fi device got an IP (every join)
    <<UI LINK DOWN>>            the last Wi-Fi device left (the session is
                                dropped: for scripts)
    A client only sees LINK UP when another device joins, i.e. with
    rad.maxcli > 1 (default 1).

LED

  LED r g b                     RGB LED colour, 0..255 each

Screen

  CLS r g b                     clear to colour, reply W/H
  ROT n                         drawing rotation n mod 4 (0/2 portrait,
                                1/3 landscape); screen unchanged; replies W/H
  BL pct                        backlight 0..100
  SCR AUTO 0|1                  update screen after every command (default 1)
  SCR UPD                       update screen now
  SCR FLIP                      show flip setting: NONE, P, L or PL
  SCR FLIP P|L|PL|NONE          device upside down: P swaps ROT 0/2,
                                L swaps ROT 1/3; stored in NV, applied now
  SCR SHOT                      save screen as /scrshots/<timestamp>.png
  SCR SHOT name                 save screen as /scrshots/name.png
  SCR SHOT [name] -f            same, overwrite if it exists
  SCR TR                        show transitions: "x > y" (one-shot >
                                persistent), "x" if y is CUT, "> y" if no x
  SCR TR name args              one-shot transition for the next update
  SCR TR CUT                    next update instant
  SCR TR NONE                   cancel one-shot (persistent is used)
  SCR TRP                       show persistent transition
  SCR TRP name args             persistent transition for every update
  SCR TRP CUT                   persistent off (NONE = CUT)
  SCR TR NONE NONE              cancel one-shot and set persistent CUT
                                (same as SCR TRP NONE NONE)

  Transitions (ms = 10..10000, T|B|L|R = side the new image enters from,
  relative to ROT):

    PUSH ms T|B|L|R             old slides out, new slides in (full screen)
    FADE ms                     colour blend (changed area only)
    WIPE ms T|B|L|R             hard edge sweeps in (full screen)
    DISSOLVE ms                 pixels flip in random order (changed area only)

  Updates caused by text typing never use a transition.

Graphics (current rotation, clipped to screen, colours 0..255)

  PLOT x y r g b                pixel
  LINE x1 y1 x2 y2 r g b        line
  BOX x y w h r g b             rectangle outline
  FBOX x y w h r g b            filled rectangle
  TRI x1 y1 x2 y2 x3 y3 r g b   triangle outline
  FTRI x1 y1 x2 y2 x3 y3 r g b  filled triangle
  CIRC cx cy rad r g b          circle outline
  FCIRC cx cy rad r g b         filled circle

Text boxes (n = 1..9; a box keeps the rotation it was created in)

  TBOX n x y w lines [speed] [lead] [font]
                                define box: pixels, height in lines;
                                speed = typing ms/char (0 = off)
  TBOX n SET x|y|w|h|s|l|f v    change one property (h in lines, s speed,
                                l lead, f font)
  TSCRL n 0|1                   auto-scroll at the bottom (default 0)
  TYX n y x                     move cursor (pixels from box top-left)
  ECHO n text                   print at cursor (text rules: section 1;
                                \1..\9 = font, this ECHO only)
  TINK n r g b                  text colour
  TCLS n r g b                  set background, clear box, cursor 0,0
  FONT                          list fonts: n name height fw|prop

Images (PNG in /images on the SD card)

  IMG                           list PNGs as "n name"
  IMG name|n                    draw image at 0,0
  IMG name|n x y                draw image at x,y (-1000..1000)

Sensors

  ACC                           accelerometer aX aY aZ (g)
  GYR                           gyroscope gX gY gZ (dps)
  TEMP                          IMU temperature (^C)
  BAT                           battery voltage "Vb: x.yy V"

  Values are replied only (on-screen display is commented out in the code).

IMU (QMI8658A; see section 8)

  IMU                           status: ranges, rates, detectors
  IMU ACC [range rate]          accel range 2|4|8|16 g, rate 28..896 Hz
  IMU GYR [range rate]          gyro range 16..2048 dps, rate 28..896 Hz
                                (stored in NV, applied now)
  IMU RESET                     chip reset, NV settings, all detectors off
  IMU TAP [s d t | NONE]        tap events: single, double, triple 0|1
  IMU MOTION [0|1]              event when movement starts
  IMU STILL [ms]                event after ms of quiet following a motion
                                (0 = off, max 60000)
  IMU SIGMO [ms [REPEAT]]       significant motion: still moving ms after
                                the movement began (0 = off, 100..60000);
                                REPEAT: again every ms while it keeps moving
  IMU STEP ...                  ERR not implemented (the chip's pedometer
                                never counts; see section 8)
  TAP cannot run with MOTION, STILL or SIGMO: switching one on while the
  other is on replies ERR naming what is on (e.g. "TAP cannot run with
  MOTION, SIGMO (turn them off first)", "MOTION cannot run with TAP (IMU
  TAP NONE first)"). MOTION, STILL and SIGMO work together.
  IMU ORIENT [NONE|P|L|ROT] [AUTO]
                                orientation events (P portrait only, L
                                landscape only, ROT all); AUTO also turns
                                the picture 180 deg when ROT is upside down
  IMU SET [key value]           list / store + apply the imu.* tuning keys
                                (wom tap.thr tap.gap mot.thr still.thr
                                step.thr step.min orient.hold orient.flat;
                                step.* unused while STEP is not implemented)
  IMU WOM [thr]                 wake-on-motion threshold 1..255 mg (NV)
  IMU SLEEP [SCR|RAD|CPU] [thr] sleep until motion or BOOT (default SCR;
                                thr overrides imu.wom for this sleep)
  IMU CAL                       gyro calibration (keep still ~1.5 s)
  IMU TEST                      accel + gyro self-test, PASS/FAIL
  IMU DEBUG [0|1]               to the log port(s), each line starting with
                                ms since boot: the chip's raw event status
                                (STATUS1, TAP_STATUS, hex) on every change,
                                CTRL8 on every reconfiguration and a copy of
                                every <<IMU ...>> event; this boot only
                                (default 0)
  IMU REC ms                    record to the console (max 3000 ms on BLE
                                and UART, 60000 ms on telnet/web/USB)
  IMU REC ms name|* [-f]        record to /imu/name.csv (* = timestamp),
                                max 600000 ms; -f overwrites an existing file

  Detectors (TAP MOTION STILL SIGMO ORIENT) are off at every boot and
  are not stored. Events arrive unprompted:

    <<IMU TAP>> <<IMU 2TAP>> <<IMU 3TAP>> <<IMU MOTION>> <<IMU STILL>>
    <<IMU SIGMO>> <<IMU ORIENT 0..3>> <<IMU ORIENT FLAT>>
    <<IMU WAKE>>

Buttons (n = 1..10 on GPIO 0 2 4 5 6 7 8 9 10 11; 1 = BOOT, tact only)

  BTN                           list buttons and global timings
  BTN n s l d                   enable tact button n: short/long/double 0|1
  BTN n s l d TACT|TOUCH        same, choose switch or touch pad
  BTN n 0 0 0                   disable button n
  BTN SET deb|long|dbl|rpt ms   global timing (for buttons enabled later)
  BTN n SET deb|long|dbl|rpt ms timing for button n only
  BTN CAL                       recalibrate touch pads (hands off)

  Timings: deb debounce, long long-press, dbl double-click window,
  rpt repeat (boot values: NV btn.deb/long/dbl/rpt).
  Events arrive unprompted:

    <<BTN n SHORT>>  <<BTN n DOUBLE>>
    <<BTN n DOWN>> <<BTN n UP>> a long press: DOWN once held for long, UP on
                                release (rpt = 0)
    <<BTN n REPEAT>>            long on, rpt > 0: at long, then every rpt ms
    typematic SHORT             short only, rpt > 0: at press, at long,
                                then every rpt ms

Settings (stored in flash, used at the next boot)

  NV GET                        list all settings
  NV GET key                    show one setting
  NV SET key value              check and store
  NV SET key r g b              same, for colour keys (disk.*, sys.aron,
                                sys.aroff)
  NV RESET                      all settings to defaults (except sys.bootcnt)
  NV RESET key                  one setting to default
  NV EXPORT name                write /sys/name.nvram (rad.pwd is written
                                as !rad.pwd=...: FCAT shows [redacted])
  NV IMPORT name                load /sys/name.nvram (all lines checked,
                                nothing stored if any is bad; sys.bootcnt
                                is skipped: "warning: sys.bootcnt ignored";
                                a leading '!' on a line is ignored)
  RAD GET                       list radio settings
  RAD GET key ...               show radio settings
  RAD SET key value ...         store radio settings (all or none)

  RAD keys: ssid pwd ble ip netmask pwr nogw maxcli (short names for rad.*).
  In values "\#" stands for '#'.

USB disk

  FILES                         SD card becomes a USB drive (backlight
                                30 %, LED flickers disk.read/disk.write
                                colour on access); reboots on eject,
                                unplug or no PC within 30 s. Shows
                                /sys/usbdrive.png if present (PNG, max
                                172x320 and 512 KB), else the built-in
                                image

Scripts (SD:/scripts/name.s; language: SCRIPTING.txt, section 9 here)

  RUN                           list the scripts
  RUN name [args]               start name.s (OK at once, output follows)
  LOAD name [args]              load and pause before the first line
  STOP [NOW]                    stop the script; NOW also cuts a long
                                command short (IMU REC, screen transition)
  PAUSE / CONT                  pause after the current line / resume
  STEP [IN|OUT]                 (paused) next line / into a call / out
  BPT [[file:]n | DEL ALL|#n|[file:]n]
                                breakpoints (kept until reboot)
  VARS / STACK                  (paused) variables / call stack
  LIST [name [from [to]]]       list a script with line numbers
                                ('>' paused line, '*' breakpoint)
  While a script runs only STOP, STOP NOW and PAUSE work, also as
  !!STOP, !!STOP NOW, !!PAUSE (other lines reply "ERR script running").
  While INPUT waits only the !! forms are commands: any other line,
  "stop" included, is the input text.


3. NV VARIABLES
---------------

  key        default         range / meaning
  ---------  --------------  ------------------------------------------
  rad.ssid   wavedemo        Wi-Fi SSID, 1..32 chars, no spaces, ':', '|'
  rad.pwd    waveP@55word    Wi-Fi password, 8..63 chars, no spaces, ':', '|'
  rad.ble    ESP32-S3-Demo   BLE device name, 1..29 chars, no spaces, ':', '|'
  rad.ip     192.168.4.1     AP address, must be a host in rad.mask
  rad.mask   /24             AP netmask /8../30
  rad.pwr    8.5             Wi-Fi TX power 2..20 dBm (0.25 steps)
  rad.nogw   0               1 = DHCP offers no gateway (engineering)
  rad.maxcli 1               Wi-Fi clients at a time 1..4
  ui.boot    BLE             interface at boot: BLE|TELNET|WEB|UART|USB
  ui.baud    115200          UART UI speed: 9600|19200|38400|57600|115200|
                             230400|460800|921600 (read when UART UI starts)
  ui.echo    1               1 = serial UIs echo what you type
  scr.bl     100             backlight at boot, 0..100 %
  scr.pflip  0               1 = portrait upside down (ROT 0 <-> 2)
  scr.lflip  0               1 = landscape upside down (ROT 1 <-> 3)
  btn.deb    30              global debounce 0..200 ms
  btn.long   600             global long-press 50..5000 ms
  btn.dbl    300             global double-click window 50..5000 ms
  btn.rpt    0               global repeat 0..5000 ms
  btn.touch  0.015           touch threshold ratio 0.001..0.5
  web.hist   50              web console history entries 10..500
  disk.read  0 255 0         LED colour for USB disk reads (r g b)
  disk.write 255 0 0         LED colour for USB disk writes (r g b)
  imu.arange 4               accel range 2|4|8|16 g
  imu.arate  224             accel rate 28|56|112|224|448|896 Hz
  imu.grange 512             gyro range 16|32|64|128|256|512|1024|2048 dps
  imu.grate  224             gyro rate 28|56|112|224|448|896 Hz
  imu.wom    100             wake-on-motion threshold 1..255 mg
  imu.tap.thr 1.3            tap peak 0.1..7.9 g (one decimal)
  imu.tap.gap 500            max gap between taps of a burst 100..2000 ms
  imu.mot.thr 100            motion threshold 32..2000 mg (31.25 mg steps)
  imu.still.thr 64           quiet threshold 32..2000 mg (31.25 mg steps)
  imu.step.thr 200           step peak-to-peak 50..2000 mg
  imu.step.min 10            steps before counting starts 1..255
                             (step.*: unused, IMU STEP not implemented)
  imu.orient.hold 500        orientation must hold 0..5000 ms
  imu.orient.flat 30         FLAT within 5..80 degrees of horizontal
  run.files  4               data files with a position per script 1..16
                             (when the script has no .FILES n)
  sys.autorun 0              1 = run the system scripts (onBoot ...);
                             not used yet (scripting to come)
  sys.aron   0 255 0         LED flash (100 ms) at start-up when autorun 1
  sys.aroff  255 0 0         same when autorun 0 (0 0 0 = no flash)
  sys.bootcnt 0              boot counter (read-only: no NV SET / RESET key;
                             exported; NV IMPORT ignores it with a warning;
                             kept by NV RESET)

  Fixed (not in NV): Wi-Fi channel 6, telnet port 23, web port 80;
  one telnet client / one web page at a time.


4. SD CARD
----------

FAT32, 4-bit SDMMC. Also reachable from a PC with FILES.
The project's sd/ folder is a ready card layout (folders, example scripts,
system scripts, favicon): copy its contents to the card. Every folder has a
DESCRIPT.ION file describing its files (the firmware ignores them).
The card may be pulled and inserted while running (no card-detect pin:
it is checked at each use). Commands then reply "no SD card" (or "write
failed" if it was pulled during a write); a re-inserted card is mounted
at its next use.

  /scripts/             scripts name.s (RUN)
  /images/              PNG images for IMG (name.png: no spaces,
                        not all digits, unique ignoring case; max 64)
  /scrshots/            screenshots from SCR SHOT
                        (default name = timestamp, e.g. b002A_001523.png)
  /imu/                 recordings from IMU REC ms name|* (.csv)
  /sys/                 settings files from NV EXPORT / for NV IMPORT,
                        (name.nvram: key=value lines, '#' comments,
                        !key=value = secret: FCAT shows [redacted]),
                        splash.png (shown when a client connects) and
                        usbdrive.png (FILES screen; built-in if missing),
                        default.nvram (all NV defaults: NV IMPORT default),
                        system scripts onBoot.s onWait.s onConnect.s
                        (NV sys.autorun 1; by hand RUN sys/onBoot)
  /www/                 web files served in WEB mode (http://<rad.ip>/path
                        -> /www/path; / -> /www/index.html if present;
                        no caching, no folder listings)
                        Reserved: /www/cfg and /www/ws are never served
                        (the built-in /cfg and /ws always win).

  Note: .nvram files hold the Wi-Fi password in plain text (FCAT
  redacts it; the file itself is not encrypted).

Created automatically when first needed: /scrshots/, /sys/, /imu/,
/scripts/data/ (FLOG, FWRITE, VARS SAVE; FDIR / FCAT show them).

Timestamp (automatic file names, INFO): bNNNN_hhmmss = boot count in hex
(NV sys.bootcnt, +1 every boot) and time since boot, e.g. b002A_001523 =
boot 42, 0 h 15 min 23 s. A name already taken gets -1, -2 ... added.
Create /images/ and /www/ yourself (missing = none).


5. CONNECTION SCREENS
---------------------

  Boot        SCR AUTO 1, black, backlight scr.bl, boot square (a
              random-colour square on the bottom edge of the glass), Wait.
  Wait        ROT 0/2 -> 1/3, SCR AUTO 0, details fade in (0.5 s),
              IMU ORIENT L AUTO (details turn 180 deg with the device).
  Connect     IMU ORIENT NONE (view flip off), SCR AUTO 0, ROT 0, fade
              to black (1 s), /sys/splash.png
              fades in (1 s; "sys/splash.png" printed if missing),
              SCR AUTO 1.
  Disconnect  SCR AUTO 1, fade to black (1 s), Wait.
  (also on UI switch)

  UART / USB have no connect signal: the first line received (even an
  empty one) is the connect; the session lasts until UI switch or REBOOT.

  Fades are internal: SCR TR / TRP settings are not changed.

  Details (font 1, orange on black):

    WEB / TELNET              BLE
    UI  : WEB:80              UI  : BLE
    SSID: <rad.ssid>          Name: <rad.ble>
    Pwd : (hold BOOT)
    IP  : <rad.ip> <rad.mask>
    WiFi: Waiting... | Link Active

    UART                      USB
    UI  : UART                UI  : USB
    Baud: <ui.baud>
    Pins: TX43 RX44

  - "Link Active" (white): a device has joined the Wi-Fi and has an IP;
    back to "Waiting..." when the last one leaves.

  - BOOT (GP0) on any Wait screen:
      long press (btn.long)  show the Wi-Fi password until released
      double press           next interface: WEB -> TELNET -> BLE -> USB
                             -> UART -> WEB
                             (second press within btn.dbl of the first
                             release; this session only, ui.boot unchanged)
  - Lines too long for the screen end in '~'.


6. WEB CONSOLE
--------------

  - Join the AP; phones open the portal by themselves (needs rad.nogw 0),
    or browse to http://<rad.ip>/.
  - Replies scroll as a page (works in Android's sign-in window); the
    input line and Send stay at the bottom. The page is always 2 px
    taller than the window, so it (and the "^" list) can scroll even
    when empty. While the "^" list is open the page is kept at least 1 px
    off the top, so Android doesn't take a downward swipe as pull-to-refresh.
  - Sent commands are echoed as "$ cmd".
  - Enter sends; Up/Down recall history; "^" (left of the input) lists
    history - tap a command to put it in the input.
  - History is kept in the browser's local storage (max web.hist
    entries); it is never sent to the board.
  - F5 clears the reply box.
  - Page source: assets/index.html (embedded in the firmware).
  - Files in /www on the SD card are served first; /www/index.html
    replaces the built-in page. /cfg and /ws are always built in
    (settings and the command WebSocket) - a custom page needs /ws.
  - Anything not found redirects to the console page.
  - Reconnect: the page checks /cfg first (2 s limit) and only then opens
    the WebSocket (3 s limit), retrying every 2 s; leaving or reloading
    the page closes it. One page at a time: a newer page takes over.
  - When the last Wi-Fi device leaves, the board closes all its web
    connections and shows the Wait screen.
  - Heartbeat: after 3 s with nothing received the page sends <<PING>>;
    the board answers <<PONG>> at once (even while a command or script
    is busy; neither is shown, scripts never see them). No answer within
    3 s: "[disconnected - no reply]" and the page reconnects.
  - REBOOT closes the page first (WebSocket code 4001): "[disconnected -
    device rebooting]", then it reconnects. (FILES stops the web console
    when it starts: plain "[disconnected]".)
    A custom page should do the same with <<PING>> and code 4001.


7. SERIAL CONSOLE (UART / USB) AND LOGS
---------------------------------------

  Terminal: 8N1 at ui.baud (UART) or any speed (USB). Replies end in CR LF.
  Input lines end on CR, LF or CR LF. Typed characters are echoed when
  ui.echo is 1 (turn the terminal's local echo off, or ui.echo 0);
  Backspace / DEL erases. Control and non-ASCII characters are ignored.

  [Startup Complete - UART UI active]   (or USB) is the last line at boot
                         when a serial UI is active; press Enter three
                         times on an empty line to see it again.

  +++  then 1 s with no input (no Enter)   [debug enabled, --- to stop]
                         logs also come out on the UI port
  ---  then 1 s with no input              [debug disabled]
  With Enter instead, +++ / --- are ordinary (unknown) commands. Debug
  also ends on a UI switch or REBOOT.

  Log routing (ESP_LOGx and dbgf()), LOG AUTO (the default):
    UI = UART          logs on USB
    UI = USB           logs on UART
    BLE/TELNET/WEB     logs on both
    debug on           logs on both, including the UI port
  LOG UART / USB / BOTH fixes the port(s) instead, even the UI port (debug
  on still adds the other); LOG AUTO goes back. This boot only.
  When UART is not the UI it runs at 115200 for the logs.

  Not routed (always both ports, from the build-time console): ROM and
  bootloader messages, start-up messages before the log hook, crash
  reports, and any plain printf inside ESP-IDF.

  FILES takes over the USB port: a USB console session ends there
  (followed by the usual reboot).

  Code: dbgf("fmt", ...) - printf-style log line, '\n' added, max 200
  chars, any task (not interrupts). See src/log.h.

8. IMU
------

  Chip: QMI8658A on I2C 0x6B; INT1 = GPIO13 (wake line), INT2 = GPIO12.
  Events are polled every 20 ms.

  TAP s d t     The chip reports each tap; firmware counts taps closer than
                imu.tap.gap into a burst. The n-th tap of a burst reports
                its order if enabled; after the highest enabled order the
                count restarts. Two quick taps: 1 1 0 -> TAP, 2TAP;
                1 0 0 -> TAP, TAP; 0 1 0 -> 2TAP. Taps less than 100 ms
                apart are lost. Works best at imu.arate 224 or more.
                Alone only: the chip's tap engine reports nothing while
                MOTION / STILL / SIGMO run (checked with IMU DEBUG).
  Engines       Turning a detector on or off reconfigures the chip with
                the sensors off (~100 ms); switching engines while the
                sensors run can leave the tap engine stuck. Changing only
                which tap orders report does not touch the chip.
  MOTION        One event when movement starts. Re-armed after quiet
                (STILL ms, or 1 s if STILL is off).
  STILL ms      Only after a MOTION (event or not): quiet for ms.
  SIGMO ms      Timed here: a motion session starts with the first movement
                after quiet (as MOTION) and ends after quiet (STILL ms, or
                1 s if STILL is off). SIGMO when movement is still seen ms
                after the start; once per session, or with REPEAT every ms
                (timed from the last SIGMO) until the session ends. (The chip's own engine
                times from its first movement and quiet does not restart
                it: "move, rest, move" fired at once. Kept, switched off:
                IMU_SIGMO_CHIP 0 in imu.c.)
  STEP          Not implemented: the chip's pedometer never counted on the
                board (no step flag at 56 or 224 Hz, any imu.step.min). The
                code is kept, switched off (IMU_STEP_ENABLE 0 in imu.c).
  ORIENT        From gravity; reported after imu.orient.hold ms. The
                number is the ROT that shows upright (SCR FLIP included).
                AUTO (view flip): when the side up is the current ROT
                turned 180 deg, the panel shows the whole picture turned
                180 deg (re-sent, instant); back to normal when ROT is
                up again. ROT, drawing, text boxes and SCR SHOT are not
                changed. A 90 deg turn (ROT mode) only sends the event.
                ORIENT without AUTO, NONE or IMU RESET end the view flip.
                The Wait screen turns on ORIENT L AUTO by itself; a client
                connecting turns ORIENT off (each client starts clean).
  SLEEP         The IMU goes to wake-on-motion (accel only, 21 Hz).
                SCR: backlight and LED off; commands still work (IMU
                     commands reply ERR IMU asleep; LED r g b is stored
                     and shown at wake).
                RAD: as SCR, and the radio off (client dropped; it reconnects
                     after the wake, so <<IMU WAKE>> is not seen).
                CPU: deep sleep; wake = reboot (BOOT or motion).
                Wake: motion above thr, or BOOT pressed.
  CAL           Gyro X/Y gain calibration; lost at power-off/reset.
  TEST          Self-test: accel > 200 mg, gyro > 300 dps on every axis.
  REC           Accel + gyro through the chip FIFO at the rate (needs
                imu.arate = imu.grate). Lines: "t ax ay az gx gy gz"
                (t ms from start, g, dps), header line first. Blocks other
                commands while recording. "ERR FIFO overflow" if the
                client or card was too slow (samples lost).


9. SCRIPTS
----------

  Full language: SCRIPTING.txt (quick reference: SCRIPTING_REF.txt).
  Examples: the project's sd/ folder (sd/scripts/, sd/sys/).

  - PRINT goes to the client; DEBUG to the log port(s).
    <<SCRIPT name END|STOPPED|ERR ...|PAUSED ...>> tell the client what
    happened (EVENTS SCRIPT 0 hides them).
  - QUIET (in the script, default 2) sets what else the client sees:
      0 everything, 1 no command replies / OKs, 2 also no <<BTN|IMU|UI>>
      events, 3 also no command ERRs. The script always gets every event.
  - System scripts, NV sys.autorun 1 (start-up LED flash: sys.aron green,
    sys.aroff red):
    In SD:/sys/ (by hand: RUN sys/onBoot, LIST sys/onBoot, BPT sys/onBoot:4):
      onBoot.s       at the end of start-up, before the interface starts
      onWait.s       FN before / instead / after around each Wait screen
      onConnect.s    FN before / instead / after around the connect splash
    While onBoot or onWait runs, holding BOOT for 3 s sets
    sys.autorun 0 and reboots (a broken system script can't lock you out).
    onWait before/after and all of onConnect run while the console
    waits: the new client's lines are taken after onConnect ends.
  - Data files in scripts (SD:/scripts/data/file.txt):
      FWRITE file text        write the text (no new line added) at the
                              file's position, overwriting / growing it
      FREAD $v file n|EOL     read n bytes (1..200) or to the end of the
                              line (CR/LF dropped); past the end: $.ok 0,
                              ERR "over-read n" (n = bytes missing); EOL at
                              the end of the file: ERR "EOF"
      FTELL $v file           the position
      FSEEK file START | END [n] | SET n | OFFS n
      FCLOSE file             forget the position
    POSITIONS COUNT FROM 1: byte #1 is the first byte; an empty file has
    size 0 and the first byte written is #1; the end of the file is
    size + 1 (FSEEK END = END 0). END n = size + 1 - n; results are kept
    within 1 .. size + 1. A file starts at 1 when first used; up to
    run.files (NV, default 4) or .FILES n (top of the script, 1..16)
    files at once; all are closed when the script ends.
  - VARS SAVE name [$a $$b ...] writes SD:/scripts/data/name.var (a new
    file each time). WITHOUT A LIST, SAVE STORES THE GLOBALS ONLY ($$);
    locals are saved only when listed.
    VARS LOAD name [$a $$b ... | ALL] sets the listed variables (all when
    there is no list); a listed name not in the file is an error.
  - BPT! in a script with no client connected: the USB serial port is the
    console while it is paused (VARS, STEP, CONT ... typed there).
  - STOP NOW cuts IMU REC and screen transitions short; an image decode
    still finishes.
  - PC tests of the interpreter: tests/scrtest (sh run.sh).

