 ,----------.
(  Power On  )=========================================================================================================
 `----------'

When the device is powered on, it will flash the LED for 100ms
	Autorun Enabled  : Default=Green
	Autorun Disabled : Default=Red

NVRAM variables [system]:
	sys.autorun = <0|1>    [0]        # En/Dis-able Autorun
	sys.aron    = <R G B>  [0,255,0]  # 'Enabled'  flash colour ... <0 0 0> -> flash disabled
	sys.aroff   = <R G B>  [255,0,0]  # 'Disabled' flash colour ... <0 0 0> -> flash disabled
	sys.bootcnt = <n>      [0]        # Boot counter

If an Autorun script [discussed later] (onBoot.s or onWait.s) hangs the device (eg. "LOOP FOREVER ; NEXT"),
hold the BOOT button for 3 seconds to kill it and set  sys.autorun=0

============
 <DATETIME>
============

The system often requires a timestamp.
This device does NOT have a Real Time Clock [RTC]
If it has been connected to a Web Browser it will know the real time
	YYYYMMDD_HHMMSS[_n]
Otherwise it uses the boot counter, and the uptime
	bXXXX_HHMMSS[_n]

If required, _n will be appended to resolve naming clashes.

The boot counter is read-only and increments every time the device boots.

NVRAM variables [system]:
	sys.bootcnt = <n>  [0]  # Boot counter


 ,---------------.
(  Boot Sequence  )====================================================================================================
 `---------------'

This is only of technical interest...

This is the boot order from app_main in src/main.c:

 1	sysNvsInit        NVS flash partition (erased only if corrupt)
 2	cmdInit           command table
 3	nvInit            NV settings loaded (per-board defaults made if missing), NV command
 4	serInit           UART0 on uart.tx/uart.rx, USB serial/JTAG, serial task
 5	logInit           log routing to UART / USB
 6	sysInit           sys.bootcnt +1, INFO/file commands
 7	ledInit           RGB LED
 8	sdInit            SD card mounted
 9	resetBootFile     /sys/reset.nvram handled, if present
10	mainAutorunFlash  LED flashes sys.aron/sys.aroff for 100 ms (unless colour BLACK)
11	logLoadCfg        /sys/debug.cfg log volumes
12	lcdInit           LCD (172x320)
13a	gfxInit           drawing
13b	textInit          text boxes
13c	fontInit          fonts
14	i2cInit           I2C bus
15	imuInit           QMI8658 IMU
16	batInit           battery ADC
17	imgInit           image decoder
18	usbDiskInit       USB disk (MSC) support, not started
19	fmInit            SD manager command
20	uiInit            BLE controller and NimBLE stack.
	                  Advertising starts once the stack is ready, then is disabled again.
	                  Boot screen: black, backlight scr.bl, boot square. UI/EVENTS commands
21  btnInit           btn.gpio read, button task, BTN command
22  scriptInit        script commands
23  scriptBoot        /sys/onBoot.s runs to the end, only if sys.autorun is 1.
	                  BOOT held 3 s: sys.autorun 0 and reboot
24  uiBegin           *24
25  appRun            console loop

*24 : uiBegin:
	Reads sys.ui and starts that interface:
		BLE        : advertising on.
		TELNET/WEB : Wi-Fi access point ; DHCP ; Telnet server
		WEB        : Wi-Fi access point ; DHCP ; Web server .. plus DNS if web.captive is 1.
		UART       : switches to uart.baud.
		USB/MSC    : nothing to start.
	If the start fails, it falls back to BLE.

	Wait screen:
		Wi-Fi link state noted.
		onWait.s "before" runs, if sys.autorun is 1.
		If it has an "instead" function, that replaces the built-in Wait screen.
		Otherwise: landscape, details drawn, 200 ms fade in, IMU orientation on.
		BOOT watched for long press and double press.
		Serial UIs only: the [Startup Complete - UART|USB UI active] banner.


 ,--------------.
(  UI Selection  )=====================================================================================================
 `--------------'

Under "normal" boot conditions (ie. no Autorun scripts are overriding/augmenting the boot sequence),
the device will offer a list of UI's (in a loop):

	.-> WEB -> TELNET -> BLE -> USB -> UART -> MSC -> RESET! -.
	`---------------------------------------------------------'

Double-Tap BOOT to select the next UI in the loop.
NB. It can take up to 2 seconds to reconfigure the radio.

NVRAM variables [sys]:
	sys.ui = <Mode>  [BLE]  # Default UI: BLE|TELNET|WEB|UART|USB|MSC


 ,-----------.
(  Emergency  )--------------------------------------------------------------------------------------------------------
 `-----------'

If you have managed to configure the device so nothing works...

If you do not see the UI menu, hold BOOT for 3 seconds to disable Autorun.

You may now use the boot menu to select UI: RESET!


 ,--------------------------.
(  UI : WiFi / Web & Telnet  )-----------------------------------------------------------------------------------------
 `--------------------------'

"WEB" and "TELNET" both use WiFi, which is controlled by the following NVRAM variables:

NVRAM variables [wifi]:
*1	wifi.ssid    = <ssid>       [CS_wave_xx-yy-zz]  # {1..32} chars. Banned: <space>, ':', '|'
*2	wifi.pwd     = <password>   [ChangeMe_nnnn]     # {8..63} chars. Banned: <space>, ':', '|'
	wifi.ip      = <PrivateIP>  [192.168.4.1]       # IP Address of WiFi Access Point
	wifi.cidr    = <CIDR>       [/24]               # /8 .. /30
	wifi.pwr     = <pwr_dBm>    [8.5]               # WiFi TX power 2..20 dBm (0.25 steps)
	wifi.maxconn = <n>          [1]                 # Maximum concurrent WiFi clients {1..4}
	wifi.gw      = <1|0>        [1]                 # 0 = DHCP offers no gateway (engineering)

*1	xx-yy-zz are the last three octets of the device's WiFi MAC
*2	nnnn     is a random number [see UI selection screen]

Android:
	I cannot work out how to get Android to route LAN IPs over WiFi,
	   and everything else over the SIM card
	I have found a workaround for the WEB interface, but not TELNET.
	As such, I recommend BLE.

Windows:
	I recommend WEB - this is the most powerful interface.


 ,-------------.
(  UI : WEB:80  )------------------------------------------------------------------------------------------------------
 `-------------'

,------------------------------.
| UI  : WEB:<web.port>         |
| SSID: <wifi.ssid>            |
| Pwd : (hold BOOT)            |
| IP  : <wifi.ip> <wifi.cidr>  |
| WiFi: Waiting...             |
`------------------------------'

Hold the BOOT button to show the password.

When WiFi connects, you will see  "Waiting..."  ->  "Link Active"
When the Client connects, you will see the Splash Screen.

NVRAM variables [web]:
	web.port     = <n>     [80]  # Web port {1..65535}
	web.captive  = <1|0>   [1]   # 1 = Enable "Captive Portal"
	web.name     = <name>  []    # {0..32} chars, no spaces [qv. uiname.txt]
	web.hist     = <n>     [50]  # Command history size {10..500}

Debug Log Port(s) [dbgf()]:
	UART & USB

Android:
	Never answer the "no internet, stay connected?" question.
		Yes -> Game works ; Internet Stops
		No  -> Game disconnects ; Internet works
		--- -> Game works in the Captive Portal window ; Internet Works
	...I cannot work out how to get Android to route LAN IPs over WiFi,
	   and everything else over the SIM card

	Play with the Captive Portal browser

Windows:
	Connect to WiFi ; Do NOT enable "discovery"

	Play with any browser:  http://<wifi.ip>

 ,----------------.
(  UI : Telnet:23  )---------------------------------------------------------------------------------------------------
 `----------------'

,------------------------------.
| UI  : TELNET:<telnet.port>   |
| SSID: <wifi.ssid>            |
| Pwd : (hold BOOT)            |
| IP  : <wifi.ip> <wifi.cidr>  |
| WiFi: Waiting...             |
`------------------------------'

Hold the BOOT button to show the password.

When WiFi connects, you will see  "Waiting..."  ->  "Link Active"
When the Client connects, you will see the Splash Screen.

NVRAM variables [telnet]:
	telnet.port  = <n>  [23]  # Telnet port {1..65535}

Debug Log Port(s) [dbgf()]:
	UART & USB

Android:
	"No internet, stay connected?"  ->  Yes  ->  Game works ; Internet Stops
	...I cannot work out how to get Android to route LAN IPs over WiFi,
	   and everything else over the SIM card

	This is my favourite Telnet app:
		https://play.google.com/store/apps/details?id=de.kai_morich.serial_wifi_terminal&hl=en
		<burger> -> Settings -> Terminal -> Show Timestamps -> OFF
		<burger> -> Devices -> Add -> Host=<wifi.ip>, Protocol=Telnet, Port=<telnet.port> -> v/ Save
	In future, just press the connect button  "-<| |>-"  (next to the dustbin)

Windows:
	Use PuTTY to connect to <wifi.ip>:<telnet.port>
		https://portableapps.com/apps/internet/putty_portable
	Press <return> a couple of times


 ,----------.
(  UI : BLE  )---------------------------------------------------------------------------------------------------------
 `----------'

,---------------------------.
| UI  : BLE                 |
| Name: <ble.name>          |
`---------------------------'

NVRAM variables [ble]:
*1	ble.name = <name>     [CS_wave_xx-yy-zz]  # {1..29} chars. Banned: <space>, ':', '|'
*2	ble.pwr  = <pwr_DbM>  [9]                 # BLE TX power

*1	xx-yy-zz are the last three octets of the device's BLE MAC
*2	Options are {-24, -21, -18, -15, -12, -9, -6, -3, 0, +3, +6, +9, +12, +15, +18, +20}

Debug Log Port(s) [dbgf()]:
	UART & USB

Android:
	Do *NOT* connect with the Android->Config->Bluetooth (the "normal" way)
	If <ble.name> is listed in your Android Bluetooth menu - you MUST "forget" it!

	This is my favourite BLE app:
		https://play.google.com/store/apps/details?id=de.kai_morich.serial_bluetooth_terminal&hl=en
		<burger> -> Settings -> Terminal -> Show Timestamps -> OFF
		<burger> -> Devices -> Bluetooth LE -> SCAN -> <ble.name>
	In future, just press the connect button  "-<| |>-"  (next to the dustbin)

iPhone:
	This looks like it might be a good choice [untested]
		https://apps.apple.com/us/app/bleserial-nrf/id1632235163


Windows:
	Do *NOT* connect with Windows->Bluetooth (the "normal" way)
	If <ble.name> is listed as a "Paired Device" - you MUST "delete" it!

	This is horrific, but it works, and it is open source!
		https://github.com/healthywalk/BLE-Serial-Terminal/tree/main/BLE_Serial_Terminal/bin/Release


 ,-------------------.
(  UI : USB - Serial  )------------------------------------------------------------------------------------------------
 `-------------------'

,---------------------------.
| UI  : USB - Serial        |
`---------------------------'

NVRAM variables [serial]:
	ser.echo = <1|0>   [1]       # echo input to terminal

Debug Log Port(s) [dbgf()]:
	UART

Android:
	This is my favourite USB Serial app:
		https://play.google.com/store/apps/details?id=de.kai_morich.serial_usb_terminal&hl=en
		<burger> -> Settings -> Terminal -> Show Timestamps -> OFF
		Press the connect button  "-<| |>-"  (next to the dustbin)
		Press Send [=>] a couple of times

iPhone:
	USB Serial is not supported

Windows:
	Run:  cmd.exe
	Run:  mode | findstr /B Status
	Connect the device - and wait a few seconds (for windows UDSB enumeration)
	Run:  mode | findstr /B Status   (again)
	The COM port that has just appeared is your Game module ... eg. COM5
	...yes, Windows UART handling is horrific!

	Now use PuTTY to connect to that COM port
		https://portableapps.com/apps/internet/putty_portable
	...it's actually a "Virtual COM Port", so the baud value is meaningless
	Press <return> a couple of times to activate the interface.


Linux:
	Just as ugly as Windows here

	for d in /sys/bus/usb/devices/*; do \
		grep -qx Espressif "$d/manufacturer" 2>/dev/null && \
		for t in "$d"/*/tty/*; do \
			[ -e "$t" ] && echo "/dev/${t##*/}"; \
		done; \
	done | head -1

	--> eg. /dev/ttyACM0

	picocom /dev/ttyACM0
		Keys:  ^A ^H  :  Help
		       ^A ^X  :  Exit

	Press <return> a couple of times to activate the interface.


 ,-----------.
(  UI : UART  )--------------------------------------------------------------------------------------------------------
 `-----------'

,---------------------------.
| UI  : UART                |
| Baud: <uart.baud> @8N1    |
| Pins: Tx:43 Rx:44         |
`---------------------------'

NVRAM variables [serial]:
	ser.echo  = <1|0>   [1]       # echo input to terminal

*1	uart.baud = <baud>  [115200]  # 9600|19200|38400|57600|115200|230400|460800|921600
*2	uart.tx   = <n>     [43]      # {2..11, 43, 44}
*2	uart.rx   = <n>     [44]      # {2..11, 43, 44}

*1	This is only read when the serial port is initialised

*2	These values cannot be the same
	On the Waveshare, GPIO43 and GPIO44 are labelled "Tx" and "Rx" (respectively)
	Early boot messages will ALWAYS be on GPIO43 ("Tx")
	{2..11} are normally mapped to BTN 1..10, so you will need to
		UN-map the BTN assignments before you can use them for the UART

Pin re-assignment only occurs after a reboot.

Debug Log Port(s) [dbgf()]:
	USB

This is a raw UART port.
It runs at 3v3, and has an internal pull-up on Rx (Tx drives).

This is a low-level interface, for external device support.
If you don't know what you're doing here, just ignore it!

Remember to press <return> a couple of times to activate the interface.


 ,------------------------.
(  UI : MSC - Flash Drive  )-------------------------------------------------------------------------------------------
 `------------------------'

,---------------------------.
| UI  : MSC - Flash Drive   |
| BOOT: hold to start       |
`---------------------------'

NVRAM variables [disk]:
	disk.read  = <r g b>  [0 255 0]  # LED colour during read  operations - {0 0 0} -> disabled
	disk.write = <r g b>  [255 0 0]  # LED colour during write operations - {0 0 0} -> disabled

This will allow you to use the SDCard as a flash drive.

Any OS:
	Your favourite Flash Drive compatible File Manager

When active  SD:/sys/usbdrive.png  is displayed
"Eject" or "umount" to the disk to reboot the device.

Windows takes forever to mount it <sigh>, but once it is going, it runs smoothly.


 ,-------------.
(  UI : RESET!  )------------------------------------------------------------------------------------------------------
 `-------------'

You may reset either:
*1	# NVRAM  : Just the NVRAM variables (to factory default)
	# Script : Just run the reset script (SD:/sys/reset.s)
	# FULL   : Do both

*1	wifi.pwd gets a new random number "ChangeMe_XXXX"

Single-tap BOOT to select: NVRAM, Script, or FULL/Both
Long-hold BOOT to start the reset process
Within 5 seconds, Long-hold BOOT *again* to confirm, or reset will be cancelled.

NVRAM and FULL reboot when done; Script returns to the RESET! screen.

A failed backup still resets.

With no SD card, the only mode is "NVRAM - no backup".

Script and FULL need reset.s, otherwise they give an error and do nothing.

If NVRAM or FULL are selected
*a	1.	If there is an SDCard, NVRAM will be backed up to SD:/sys/<timestamp>.nvram
	2. 	NVRAM will be set to default values
If Script or FULL are selected
	3.	SD:/sys/reset.s will be executed

*a	<timestamp> will be "bXXXX_hhmmss" where
		XXXX    is the boot count in hex
		hhmmss  is the uptime (probably only 1 or 2 seconds [untested])

You can also reset NVRAM variables by creating  SD:/sys/reset.nvram
and specifying the values you want to apply.
NVRAM is cleared to defaults, and then every value in reset.nvram is applied/
After being applied, it will then be renamed to either:  reset.done
                              or, if there were errors:  reset.bad


 ,---------------.
(  SDCard Layout  )====================================================================================================
 `---------------'

/
+--	/images/
|	+--	<images and sprites> .. Screen is 172x320
|
+--	/imu/
|	+--	*.csv        : IMU recordings
|
+--	/scripts/
|	+--	*.s          : scripts
|	|
|	+--	/data/
|		+-- *.txt    : script assets
|		+-- *.var    : script state files
|
+--	/scrshots/
|	+--	*.png        : screenshots .. default: <timestamp>.png
|
+--	/sys/
|	+--	*.nvram      : NVRAM exports .. default: <timestamp>.nvram
|	+--	debug.cfg    : output stream volume levels
|	|
|	+--	onBoot.s     : onBoot script .. after early boot
|	+--	onWait.s     : before/instead/after functions for the Wait screen
|	+--	onConnect.s  : before/instead/after functions for the Splash Screen
|	+--	/data/
|	|	+-- *.txt    : script assets
|	|	+-- *.var    : script state files
|	|
|	+--	splash.png   : splash screen shown when a client connects
|	+--	usbdrive.png : splash screen shown when USB Flash Drive is active
|	|
|	+--	fm.html      : user provided web-based File Manager
|	+--	syntax.cfg   : user provided web syntax higlighter
|
+--	/www/
	+--	favicon.ico  : www favicon
	+--	index.html   : replacement Web UI
	+--	uiname.txt   : Web page Title (overrides web.name)

The built-in web-based file manager supports 4DOS 'DESCRIPT.ION' files.
(one per folder, one file per line)

 ,------------------.
(  CONSOLE COMMANDS  )=================================================================================================
 `------------------'

The boot sequence is complete:
	The device has booted
	UI has been selected
	Connection has been made
	The splash screen is displayed.

At this point, it is expected that onConnect will run your system script.

But before we look at scripts, let's look at the system commands that are available in the UI.


 ,-------------.
(  Basic Rules  )------------------------------------------------------------------------------------------------------
 `-------------'

In the UI you may only ONE command per line

Commands are NOT case-sensitive .. Eg. `REBOOT` == `reboot` == `ReBoOt`

Command limits:  200 characters, 18 arguments

Each command replies either
	OK
	ERR <reason>
	unknown |<input>|

All Input is ASCII .. ^ (circumflex) is used as the degrees symbol (eg. 37.1^C)


 ,------.
(  Help  )-------------------------------------------------------------------------------------------------------------
 `------'

A brief summary of commands and usage can be requested with:
	HELP


 ,-------------------------.
(  Colours - Normal People  )==========================================================================================
 `-------------------------'

Colours may be represented as RGB (decimal or hex/HTML format) or HSL (decimal format only).

Notation: <rgb> means "a colour in any of the allowed formats".  ["<clr>" was less obvious in help text.]

For simplicity, just use HTML colour codes and skip the rest of this section.

	      |  Black   :  White   :  Light Pink  :  Orange
	------|-----------------------------------------------
	 HTML |  000000  :  FFFFFF  :  FF80C0      :  DC6E14

 ,-----------------------.
(  Colours - Power Users  )============================================================================================
 `-----------------------'

For power users...

	Red        {0..255}  {00..FF}
	Green      {0..255}  {00..FF}
	Blue       {0..255}  {00..FF}

	Hue        {0..359} degrees
	Saturation {0..100} percent
	Luminance  {0..100} percent

Pure "Black" & "White" are obvious in RGB/HTML, less obvious in HSL.

	      |  Black       :  White         :  Light Pink   :  Orange
	------|--------------------------------------------------------------
	 RGB  |  0,0,0       :  255,255,255   :  255,128,192  :  220,110,20
	 HTML |  000000      :  FFFFFF        :  FF80C0       :  DC6E14
	 HSL  |  @any,any,0  :  @any,any,100  :  @330,100,75  :  @27,83,47

Bear in mind that due to rounding errors, converting between RGB and HSL is lossy.
Eg.  220,110,20  ->  @27,83,47   ->  219,110,20
                   (27,83.3,47.1)

The HSL -> RGB888 formula can be found in /src/cmd.c:cmdParseHsl()

The firmware never needs RGB??? -> HSL  [qv. Appendix A]

=========================
 LED - Das Blinken Light |---------------------------------------------------------------------------------------------
=========================

The LED is actually three LEDs R, G, and B.
Each takes a "brightness" (PWM factor) of {0..255}
So each colour shown is exactly the colour specified by the code (ie. RGB888).

======================
 LCD - Screen / Glass |------------------------------------------------------------------------------------------------
======================

The LCD actually uses a different (but standard) colour encoding: BGR565 (yes, BGR)
You do not need to worry about this, it's all taken care of 'behind the scenes'.
But you might like to know how it is handled

	RGB888 -> 2^(8+8+8) -> 16,777,216 colours
	RGB565 -> 2^(5+6+5) ->     65,536 colours

So there is obviously colour loss on this (cheap) screen.

We apply rounding to try and minimise the effects of the colour loss:

	r5 = (r * 249 + 1014) >> 11;
	g6 = (g * 253 +  505) >> 10;
	b5 = (b * 249 + 1014) >> 11;
	rgb565 = (r5 << (5+6)) | (g6 << 5) | b5;

The screen driver then converts RGB565 -> BRG565


 ,-------------------------.
(  LED - Das Blinken Light  )==========================================================================================
 `-------------------------'

Change the colour of the LED:

	LED <rgb>

It is worth noting that in certain places BLACK is more than "off", it is "disabled"
Eg. if sys.aroff or sys.aron fires and is set to 000000, the light is NOT flashed, so boot is 100ms faster.


 ,----------------------.
(  LCD - Screen / Glass  )=============================================================================================
 `----------------------'

Although the screen only supports 65536 colours, <rgb> is still specified as {0..255}
	qv. Colours -> Power Users -> LCD
	ie. Don't worry about it, just use 'normal' colours.

===========
 Backlight |-----------------------------------------------------------------------------------------------------------
===========

The backlight level is set as a percentage
	  0 : Off
	100 : Full brightness

	BL <n>  # set backlight brightness {0..100}

==========
 ROTATION |------------------------------------------------------------------------------------------------------------
==========

The screen can be oriented in 4 ways {0..3}

	0 : Rotate Clockwise by (0 *90 =   0) degrees - Portrait/Tall  : USB at the bottom
	1 : Rotate Clockwise by (1 *90 =  90) degrees - Landscape/Wide : USB on the right
	2 : Rotate Clockwise by (2 *90 = 180) degrees - Portrait/Tall  : USB at the top
	3 : Rotate Clockwise by (3 *90 = 270) degrees - Landscape/Wide : USB on the left

Portrait/Tall  is {172 x 320} pixels
Landscape/Wide is {320 x 172} pixels

	ROT <n>  # n = {0..3}

If you specify a value >3, it will be taken modulo 4
	eg. {4->0, 5->1, 158->2}

Rotation ONLY effects the coordinate system, it does NOT cause the current image to rotate.

===========
 AUTO-FLIP |-----------------------------------------------------------------------------------------------------------
===========

The gyroscope can be used to flip the screen when the device is turned.

	SCR FLIP P      # perform gyro flip for [P]ortrait  layouts (ROT = 0 or 2)
	SCR FLIP L      # perform gyro flip for [L]andscape layouts (ROT = 1 or 3)
	SCR FLIP PL     # perform gyro flip for [L]andscape & [P]ortrait layouts
	SCR FLIP NONE   # disable gryo flip

=============
 SCREEN-SHOT |---------------------------------------------------------------------------------------------------------
=============

You can take a screenshot at any point.
Screenshots are in PNG format, and saved to SD:/scrshots/

If you do not specify a name for the screenshot, the system will use <datetime> as the filename.

Do NOT provide the file extension, ".png" will be added for you.

If the file already exists, you will get an error
If you wish to force a file-overwrite, use -f

	SCR SHOT [filname] [-f]

==================
 DOUBLE BUFFERING |----------------------------------------------------------------------------------------------------
==================

The screen is Double Buffered.
Ie. Screen edits are made to a scratch buffer, and, when you are ready, that buffer is enabled and shown.
    At which point the previous buffer becomes the scratch buffer.

If you want the screen to be updated immediately when you trigger a screen-write event, ENable AUTO-update.
If you want to make multiple edits and have them all appear at once, DISable AUTO-Update
If AUTO-update is DISabled, you will need to trigger the update yourself.

	SCR AUTO 1   # ENable  auto-update  [system default]
	SCR AUTO 0   # DISable auto-update
	SCR UPD      # Update now

==============
 TRANSISTIONS |--------------------------------------------------------------------------------------------------------
==============

When an update occurs the scratch buffer is swapped with the active buffer, and the LCD is updated.

This update <mode> can be simple or complex:
	NONE              # synonym for CUT  [system default]
	CUT               # instant display

	PUSH <ms> <side>  # old image slides out as the new one slides in (from the specified side)
	WIPE <ms> <side>  # a hard edge sweeps in (from the specified side) revealing the new image

	FADE     <ms>     # each pixel blends from old to new colour
	DISSOLVE <ms>     # pixels update in a random order

<ms>   is the number of milliseconds (1s = 1000ms) it takes for the transition to complete
<side> is the side the new image comes from [see following table]

	 Arrow | Name | Action
	-----------------------------------------------------
	   v   |  T   | moves down  (enters from the top   )
	   ^   |  B   | moves up    (enters from the bottom)
	   >   |  L   | moves right (enters from the left  )
	   <   |  R   | moves left  (enters from the right )

NB.	CUT, PUSH, WIPE   act on the whole screen (obviously)
	FADE and DISSOLVE act on only pixels which have changed between frames

There are TWO transition variables
	Persistent - the current default transition
	One-Shot   - only the next update transition (then go back to the default)

Set the persistent transition:
	SCR TRP         # identify the active persistent transition
	SCR TRP <mode>  # set the persistent transition

Set the one-shot transition:
	SCR TR          # identify the active one-shot transition
	SCR TR <mode>   # set the one-shot transition

Example useage:
	SCR TRP WIPE  300 v  # 0.3 seconds, move downwards, from the top
	SCR TR  PUSH 1000 L  # 1.0 seconds, move rightwards, from the left
	SCR TRP NONE         # Display immediately

==============
 Clear Screen |--------------------------------------------------------------------------------------------------------
==============

You may clear the entire screen to a single colour:

	CLS <rgb>

====================
 Images and Sprites |--------------------------------------------------------------------------------------------------
====================

An image may be any size (realistically, up to the size of the screen)

RGBA PNG files are supported. So you CAN paint holes in your image.

Eg. You can display an image, and then overlay a sprite

Images are stored in SD:/images/ and must be PNG format.

For test purposed, the first 64 images may be enumerated:
	IMG  # enumerate the first 64 images

...During testing, you may use the number in place of the name.

An image may be displayed anywhere on the screen.
The Top-Left of the screen is {0,0}

If it attempts to print off the edge of the screen, it will be clipped.
If you do not specify {x,y}, then {0,0} is assumed.

Display an image:
	IMG [<name> [<x> <y>]]

Example useage:
	IMG 1          # show the 1st image at {0,0}
	IMG 2 10 20    # show the 2nd image such that it's top-left pixel is plotted
	                              10 pixels in from the left, and 20 pixels down from the top

	IMG field      # show SD;/images/field.png at {0,0}
	IMG dog 30 45  # show SD;/images/dog.png at {x=30, y=45}

==============
 Basic Shapes |--------------------------------------------------------------------------------------------------------
==============

A few basic shapes are included in the core engine.

The top-left     corner is {x,y} = {0,0}
The bottom-right corner is {x,y} = {width,height}
The screen dimentions are:
	ROT 0 or 2 : w=172, h=320
	ROT 1 or 3 : w=320, h=172

Change the colour of a single pixel:
	PLOT  <x> <y>  <rgb>

Draw a line between, and including, (x1,y1 -> x2,y2)
	LINE  <x1> <y1>  <x2> <y2>  <rgb>

Lines are drawn with an end-biased variant of Bressenham's algorithm,
so the line from (x1,y1 -> x2,y2) is always identical to the line from (x2,y2 -> x1,y1)

Boxes (squares and rectangles):
	BOX   <tlx> <tly>  <width> <height>  <rgb>  # BOX outline
	FBOX  <tlx> <tly>  <width> <height>  <rgb>  # Filled BOX

Triangles (graphics engine "polygons"):
	TRI   <x1> <y1>  <x2> <y2>  <x3> <y3>  <rgb>  # BOX outline
	FTRI  <x1> <y1>  <x2> <y2>  <x3> <y3>  <rgb>  # Filled BOX

Circles:
	CIRC  <cx> <cy>  <rad>  <rgb>           #
	FCIRC <cx> <cy>  <rad>  <rgb>           #

{cx,cy} is the coordinate for the CENTRE of the circle
<rad>   is the radius in pixels

============
 Text Boxes |----------------------------------------------------------------------------------------------------------
============

If you want something akin to an ebook screen, you will probably just define a single text-box
that covers the entire screen ...or maybe a gap on the right for the scroll-bar.

If you are designing a UI, it is useful to have many smaller text boxes.

You may define up to 32 boxes. Each holds its own rendering parameters.

List and enumerate all available fonts (maximum 9):
	FONT

Retrieve the parameters of a specific font:
	FONT <n>

	Eg.	n:1 name:DejaVu_Sans_Mono height:24 baseline:19 lead:2 maxw:12 prop:0 first:32 last:126

	n        - font number
	name     - font name
	height   - line height
	baseline - baseline offset (from top)
	lead     - leading height
	maxw     - width of widest character
	prop     - {1:proportion, 0:monospace}
	first    - first ASCII character
	last     - last  ASCII character

	...All measurements in pixels

Create and define a text box:
	ROT    <r>
	TBOX   <n>  <x> <y>  <w> <lines> [<speed> [<font> [<lead>]]]
	TSCRL  <n>  <0|1>
	TPAPER <n>  <NONE|AUTO|rgb>
	TINK   <n>  <NONE|AUTO|rgb>
	TCLS   <n>  [<AUTO|rgb>]

ROT     : set coordinate rotation - TBOX will store this setting for each box

TBOX ...:
<n>     : the box number {1..32}
<x> <y> : the coordinates of the top-left corner of the box (ROTation aware)
<w>     : width  of the box in PIXELS
<lines> : height of the box in LINES  ... the pixel height is calculated from the font
<speed> : 0 -> instant ... >0 pause <speed> ms between characters (simulate typing) [default: 0]
<font>  : font number - use `FONT` for a list of available fonts                    [default: 1]
<lead>  : the space (in PIXELS) between lines ["lead" as in "lead pipes"]           [default: from font]

TSCRL   : scrolling is OFF (0) by default, you may turn it {1:on} and {0:off} at will

TPAPER  : background colour - this only effects future TEXT and TCLS outputs
          ...AUTO will use the current CLS value
		  ...NONE will disable rendering of background (eg. for writing over an image)

TINK    : glyph      colour - this only effects future TEXT outputs
          ...AUTO will Black/White as best contrasts to the current PAPER colour
		  ...NONE will disable rendering of glyph (eg. a gap in the paper)

TCLS    : clear the text box (including outline) to the specified colour
          ...if no colour is given and paper=NONE, use colour=AUTO
		  ...colour=AUTO will use the current CLS value

Modify textbox parameters:
	TBOX <n> SET <x|y|w|l|s|f|d> <v>

<x> <y> : top-left location
<w>     : width  in PIXELS
<l>     : height in LINES
<s>     : speed in ms
<f>     : font number [qv. FONT]
<d>     : leading in PIXELS

<v>     : value

Eg. Move an existing textbox to {10,20}
	TBOX <n> SET x 10
	TBOX <n> SET y 20

Eg. Change the "typing speed":
	TBOX <n> SET s 40

To retrieve the current parameters for a textbox:
	TBOX <n>

	n:1 rot:0 x:6 y:6 w:160 lines:4 speed:0 font:1 lead:2 h:102 cx:10 cy:2 scroll:0 ink:FFFFFF paper:auto

	n       - box number
	rot     - rotation it was defined in
	x y     - top-left corner, in pixels
	w       - width in pixels
	lines   - height in lines
	speed   - typing delay, ms per character
	font    - base font
	lead    - leading in pixels
	h       - height in pixels ((lines * fontheight) + (lines-1 * leading))
	cx      - current cursor position x, in pixels
	cy      - current cursor position line, from 1
	scroll  - auto-scroll {0:off, 1:on}
	ink     - glyph colour      {AUTO, NONE, <rgb>}
	paper   - background colour {AUTO, NONE, <rgb>}


======================
 Show Text in Textbox |------------------------------------------------------------------------------------------------
======================

Text boxes are ASCII, Left-to-Right, only.

The virtual cursor starts on the left of the first line.
It is automatically advanced to the start of the next line when text is displayed.

TEXT does NOT add an implicit '\n' (carriage return) to the end of strings:

	TEXT <n> "hello"
	TEXT <n> "-sailor"

Will render "hello-sailor"
Not "hello
     -sailor"

Line-wrapping and word-breaking are both enforced according to these rules:
	# A word that doesn’t fit on the rest of the line goes to the next line,
	  unless it’s already at the start of a line.
	# A word wider than the box will split between characters.
	# A space at the end of the line that doesn’t fit is dropped. Other spaces are kept.
	# "\n" moves the cursor to the start of the next line
	# Printing beyond the bottom of the box:
	    with scroll on, the box content moves up.
		with scroll off, the rest is dropped until the cursor moves (TYX or TCLS).

You may position the cursor by hand.

	TYX <n>  <l> <x>

<l> : Line number
<x> : pixel coordinate

===================
 What is a String? |---------------------------------------------------------------------------------------------------
===================

For simplicity: Just wrap all your strings up in "quotes", and read the bit about "escapes".

*	Escapes:
		\n      ->  new line.
		\1..\9  ->  select font (for the rest of this TEXT)

		\#      ->  #  .. *ALL* #'s in strings MUST be escaped (or they will start a comment)
		\$      ->  $  .. an unescaped $ will be interpreted as a variable name prefix
		\\      ->  \  .. a way to print the escape character

		\"      ->  "\"" -> "  ..  '\"' -> \"  .. If a string is quoted, that encapsulation quote
		\'      ->  '\'' -> '  ..  "\'" -> \'  ..    needs to be escaped inside the encapsulated string

		...anything not listed above is literal text:  "\#\H" -> |#\H|

In detail, there are a number of rules in play:

*	Unquoted Strings:
		The text begins after the one space that follows n. Any further spaces are kept.

*	Single Quotes:
		Include only and all of the text between the quotes:
			' foo bar ' -> | foo bar |
		To print a single quote, escape it
			'Chief O\'Brien' -> |Chief O'Brien|
		Anything other than blanks (or comments) after the closing quote is an error

*	Double Quotes:
		Include only and all of the text between the quotes:
			" foo bar " -> | foo bar |
		To print a double quote, escape it
			"He's \"the\" man' -> |He's "the" man|
		Anything other than blanks (or comments) after the closing quote is an error

*	Leading \" or \':
		This is not an open-quote, but a literal quote at the start of an unquoted string
			\"hello" -> |"hello"|

*	In scripts [discussed later]:
		$variables are expanded, and then the string is displayed.
		A # or ; that comes from a variable’s value does NOT count as a comment or separator.
			LET $s = "\#;hello" ; TEXT 1 $s
			-> |#;hello|

*	Rendering:
		Starts at the current cursor position, and obeys word-wrap and scroll rules.

Examples:
	TEXT <n> "double quotes"    # Displays |double quotes|
	TEXT <n> 'single quotes'    # Displays |single quotes|
	TEXT <n> "Chief O'Brian"    # Displays |Chief O'Brian|
	TEXT <n> 'He\'s "the" man'  # Displays |He's "the" man|
	TEXT <n> " foo  bar  "      # Displays | foo  bar  |
	TEXT <n>  foo  bar          # Displays | foo  bar|
	TEXT <n> foo  bar           # Displays |foo  bar|
	TEXT <n>   \" foo  bar      # Displays |  " foo  bar|
	TEXT <n>    \' baz "bug's"  # Displays |   ' baz "bug's"|

 ,-------.
(  NVRAM  )============================================================================================================
 `-------'

The system configuration is controlled by NVRAM Variables

NVRAM settings are persistent between boots, and do not require an SDCard.

	NV GET                    | List all current NVRAM settings
	NV GET <var.name>         | Show a specific NVRAM setting
							  |
	NV SET <var.name> <value> | Change a specific NVRAM setting
							  |
*1	NV RESET                  | Reset (almost) ALL NVRAM variables to factory default
	NV RESET <var.name>       | Reset a specific   NVRAM variable  to factory default
							  |
	NV EXPORT <filename>      | Save current settings to   SD/sys/<filename>.nvram
*2	NV IMPORT <filename>      | Load         settings from SD/sys/<filename>.nvram


*1	sys.bootcnt  is NOT reset
	wifi.pwd     gets a new random 4-digit number

*2	sys.bootcnt  is ignored (with a warning)
	all lines are validated before import - any error aborts the import (with an error)


 ,-------------.
(  System Info  )======================================================================================================
 `-------------'

Hardware and firmware details can be retrieved with:
	INFO

	Chip: esp32s3 rev 0.2, 2 cores, 160 MHz
	Flash: 16 MB  PSRAM: 8 MB
	Free: 95 KB internal, 7741 KB PSRAM
	MAC: Wi-Fi 28:84:85:55:74:b9  BT 28:84:85:55:74:ba
	FW: wavedemo 0.5.0 (Nov 15 2026 08:11:12)
	IDF: v6.1
	Partition: ota_0
	App: 1698 KB of 4096 KB (41 %)
	Boot: 182
	Reset: USB
	Timestamp: 20261115-225541
	Clock: 2026-11-15 22:55:41
	Script: none
	Uptime: 0d 00:05:04
	SD: 59.4 GB, 59.4 GB free

Runtime details can be retrieved with:
	MEM

	Internal: free 95 KB, lowest 47 KB, largest block 31 KB
	PSRAM: free 7741 KB, lowest 7652 KB, largest block 7680 KB
	Task             prio  stack free (lowest)
	httpd               5    1576 B
	main                1    4888 B
	IDLE0               0     756 B
	IDLE1               0     752 B
	ser                 5    3152 B
	tcpip              18    1840 B
	dns                 5    1804 B
	imu                 4    4744 B
	ipc0               23     388 B
	btn                 6    3360 B
	btController       23    2780 B
	nimble_host        21    2180 B
	ipc1               24     560 B
	sys_evt            20    1544 B
	esp_timer          22    3044 B
	wifi               23    4100 B
	Tmr Svc             1    1388 B

 ,--------------.
(  UI Selection  )=====================================================================================================
 `--------------'

The UI can be changed, and/or set from the UI:

Check default UI:
	UI                 # active:<current> default:<boot_default>

Change UI:
	UI <mode>          # {Web, Telnet, BLE, USB, UART, MSC}

Change UI & set default:
	UI <mode> DEFAULT  # {Web, Telnet, BLE, USB, UART, MSC}

	If <mode> is NOT the current active mode, switch to it.

Set default UI to current UI:
	UI DEFAULT

Set default UI without switching:
	NV SET sys.ui <mode>

NVRAM variables [sys]:
	sys.ui = <Mode>  [BLE]  # Default UI: BLE|TELNET|WEB|UART|USB|MSC


 ,------.
(  Wait  )=============================================================================================================
 `------'

Although seemingly useless, this aids in debugging groups of commands pasted in to a serial session.

	WAIT <ms>    # Pause for <ms> (1 second = 1000 ms) .. resolution 1 ms


 ,--------.
(  REBOOT  )===========================================================================================================
 `--------'

To reboot the device from the UI:
	REBOOT


 ,---------.
(  Battery  )==========================================================================================================
 `---------'

The battery voltage can be retrieved from the hardware:
	BAT      # Vb:4.19  (two decimal places)


 ,---------------------------------.
(  IMU : Inertial Measurement Unit  )==================================================================================
 `---------------------------------'

The Waveshare is equipped with a QMI8658A "IMU".

==========
 Features |------------------------------------------------------------------------------------------------------------
==========

It offers four features:
	# Accelerometer
	# Gyroscope
	# Temperature Sensor (temp of chip reads a few degrees higher than ambient)
	# Step Counter (pedometer - click) [DISABLED]

It can generate interrupts:
	# Tap                - short sharp movement
	# Motion start       - when motion starts
	# Motion stop        - when motion stops
	# Wake-on-Motion     - stop hibernating
	# Significant motion - Ignored - re-implemented in firmware (for greater control)

========
 Events |--------------------------------------------------------------------------------------------------------------
========

And we can generate inferred events for:
	# Tap Events:
		# Single - <<IMU TAP>>
		# Double - <<IMU 2TAP>>
		# Triple - <<IMU 3TAP>>

	# Motion Events:
		# Motion start       - <<IMU MOTION>>   [once per movement session]
		# Motion stop        - <<IMU STILL>>
		# Significant motion - <<IMU SIGMO>>    [after <ms> of "significant" movement]

	# Wake-on-Motion     - <<IMU WAKE>>
	# Screen orientation - <<IMU ORIENT 0..3|FLAT>>

..."Tap Events" and "Motion Events" are exclusive - one or the other, but not both.

===============
 Configuration |-------------------------------------------------------------------------------------------------------
===============

The Events are set with the `IMU` command:
	IMU TAP    <s d t|NONE>           # sdt=<0|1> .. enable Single/Double/Triple .. "NONE" == "0 0 0"
	IMU ORIENT <L|P|ROT|NONE> [AUTO]  # Landscape/Portrait orientation events [AUTO flip screen]
	IMU STILL  <ms>                   # {0..60000} [0 = off] .. enable Still detection
	IMU MOTION <0|1>                  # enable Motion detection
	IMU SIGMO  <ms> [REPEAT]          # {0, 100..60000} [0 = off] .. enable Significant Motion detection

The IMU settings can be configured with:
	IMU ACC <range> <rate>  # { 2,  4,  8,  16}g                         .. {28, 56, 112, 224, 448, 896}hz
	IMU GYR <range> <rate>  # {16, 32, 64, 128, 256, 512, 1024, 2048}dps .. {28, 56, 112, 224, 448, 896}hz
	IMU WOM <peak>          # {1..255}mg

Running any of those commands without argument will return the current settings:
	IMU TAP     # single:<0|1> double:<0|1> triple:<0|1>
	IMU ORIENT  # mode:<P|L|ROT|NONE> auto:<0|1>
	IMU STILL   # ms:<n>    [0 -> off]
	IMU MOTION  # en:<0|1>
	IMU SIGMO   # ms:<n> repeat:<0|1>

	IMU ACC     # range:<n> rate:<hz>
	IMU GYR     # range:<n> rate:<hz>
	IMU WOM     # mg:<n>

NB.	TAP, ORIENT, STILLM MOTION and SIGMO are all DISABLED at boot.

The IMU core settings can be retrieved:
	IMU SET     # tap.thr:1.3   tap.gap:500  orient.hold:500   orient.flat:30       [a single line
	            # still.thr:64  mot.thr:100  wom:100   step.thr:200   step.min:10    with single spaces]

The IMU core settings can be changed (both Live and NVRAM settings are updated):
	IMU SET <field> <value>  # also edits NVRAM

NVRAM variables [imu]:
	imu.arange      = <g>    [4]    # {2, 4, 8, 16}                  - accelerometer range
	imu.arate       = <hz>   [224]  # {28, 56, 112, 224, 448, 896}Hz - sample rate

	imu.grange      = <dps>  [512]  # {16, 32, 64, 128, 256, 512, 1024, 2048} - gyroscope range
	imu.grate       = <hz>   [224]  # {28, 56, 112, 224, 448, 896}Hz          - sample rate

	imu.tap.thr     = <g>    [1.3]  # {0.1 .. 7.9} - peak reading (one decimal place)
	imu.tap.gap     = <ms>   [500]  # {100..2000}  - maximum gap between taps (for 2tap/3tap)

	imu.orient.hold = <ms>   [500]  # {0..5000}    - delay before registering an orientation change
	imu.orient.flat = <deg>  [30]   # {5..80}      - angle from horizontal which is considered to be "flat"

	imu.still.thr   = <mg>   [64]   # {32..2000}   - largest  change between samples [step size 31.25 mg]
	imu.mot.thr     = <mg>   [100]  # {32..2000}   - smallest change between samples [step size 31.25 mg]
	imu.wom         = <mg>   [100]  # {1..255}     - change that wakes from IMU SLEEP

	imu.step.thr    = <mg>   [200]  # {50..2000}   - [DISABLED] peak-to-peak change
	imu.step.min    = <n>    [10]   # {1..255}     - [DISABLED] steps before counting starts

=============
 Calibration |---------------------------------------------------------------------------------------------------------
=============

The gyroscope may be calibrated.

Place the device flat on a steady surface and run:
	IMU CAL

Any movement during calibration will cause calibration errors.
Too much movement will cause the calibration to fail.

The process will take approximately 1.5 of your Earth seconds.

Calibration results are stored in NVRAM and restored
	# at boot
	# during an IMU RESET.

Every time you run CAL or TEST, the results are written to SD:/imu/imu_cal.log

==============
 Quick Access |--------------------------------------------------------------------------------------------------------
==============

The Accelerometer, Gyroscope, and Temperature have Quick Access commands:
	ACC   # aX:-0.008 aY:-0.916 aZ:0.045  (3 decimal places)  .. range set by IMU ACC
	GYR   # gX:7.529 gY:3.482 gZ:0.444    (3 decimal places)  .. range set by IMU GYR
	TEMP  # ^C:37.1 ^F:98.8 ^K:310.3      (1 decimal place)

===========
 Recording |-----------------------------------------------------------------------------------------------------------
===========

Recording
	IMU REC [<filemame>|*  [-f]]

<filename> can be any {letters, numbers, underscore}[31] ... ".csv" is appended
*          means: use <timestamp>
-f         force overwrite if file already exists

If no filename is specified, the output goes to the active UI.

Sanity limits are imposed (in code).
	BLE/UART       :   3000mS (  3s          ) ... Slow UI - FIFO errors may occur
	Web/Telnet/USB :  60000mS ( 60s ->  1min ) ... Fast UI - data captured reliably
	SDCard/File    : 600000mS (600s -> 10mins) ... Fast    - data captured reliably

The CSV output looks like this:
         t,      ax,      ay,      az,        gx,        gy,        gz
     0.000,  -0.553,  -0.034,   1.215,   193.641,     2.703,   129.219
     4.460,  -0.556,  -0.002,   1.264,   191.875,    12.953,   131.109
     8.921,  -0.582,   0.055,   1.362,   189.062,    23.656,   132.172

========
 Sleeep |--------------------------------------------------------------------------------------------------------------
========

The IMU is able to put the device to sleep (for power saving).
	IMU SLEEP [SCR|RAD|CPU] <mg>  # {1..255}mg change to wake

In this case, the device will be awoken by physical movement [WoM - "Wake on Motion"].
The depth of the sleep can be:
	SCR - Screen, and LED off    [default]
	RAD - SCR + 2.4GHz radio off
	CPU - RAD + CPU Deep Sleep

...Obviously, the deeper the sleep, the longer it takes to wake up, and the more State is lost.

On wake, an Event will be issued:
	<<IMU WAKE>>

==============
 Device Reset |--------------------------------------------------------------------------------------------------------
==============

To reset the device
	IMU RESET

This will:
	# Reset the chip
	# Reload the nvram defaults
	# Apply the most recent Gyro gain calibration values
	# Disable: TAP, ORIENT, STILL, MOTION, and SIGMO

===========
 Self Test |-----------------------------------------------------------------------------------------------------------
===========

The device has a built-in self-test command which can be triggered with:
	IMU TEST     # acc:pass ax:775 ay:665 az:678 gyr:pass gx:1040 gy:1028 gz:1204

The important bits are:
	acc:pass
	gyr:pass

So long as {ax,ay,az} are all >200
       and {gx,gy,gz} are all >300
You can trust the results.

If you see a drastic change in one of these numbers (over time),
the device has probably been dropped and damaged.

Every time you run TEST or CAL, the results are written to SD:/imu/imu_cal.log

============================
 Engineering Debug features |------------------------------------------------------------------------------------------
============================

Advanced debugging output is available
	IMU DEBUG <0|1>

This setting is lost on reboot/
Lines go to the log port(s), not the console client, each starting with milliseconds since boot:
	# The chip’s raw event registers (STATUS1, TAP_STATUS, in hex) every time they change
	# The engine configuration (CTRL8) every time it’s reconfigured
	# A copy of every <<IMU ...>> event.

It is VERY noisy. and mostly useful to locating Tap and Step Counter [DISABLED] quirks.

 ,-------.
(  Files  )============================================================================================================
 `-------'

There are several ways to modify the contents of the SDCard.

The SDCard MUST be formatted to FAT32 - *not* XFAT

MacOS:
	In Disk Utility, select the card, click Erase, and choose MS-DOS (FAT) with Master Boot Record.

Linux:
	lsblk                           <-- find device name
	sudo mkfs.vfat -F 32 /dev/sdX1  <-- use correct device name

Windows does not like to format an SDCard larger than 32GB as FAT32 [oh the irony]. Try:
	Rufus
	guiformat

=============
 Flash Drive |---------------------------------------------------------------------------------------------------------
=============

The device may be mounted as a Flash Drive - to then use as you would do any other flash drive.

	UI Selection : MSC / Flash Drive

	UI Command   : FILES

=======================
 Web UI - File Manager |-----------------------------------------------------------------------------------------------
=======================

The Web UI has a built in File Manager. Start it with:
	FILES WEB

The File Manager code & Syntax Highlighter definitions may be exported:
	FILES FMEXPORT    # -> SD:/sys/fm_builtin.html
	FILES SYNEXPORT   # -> SD:/sys/syntax_builtin.cfg

Features include:
	# DESCRIPT.ION file support
	# Drag'n'drop Uploads
	# Script files can be RUN (with arguments)
	# Script file viewer has syntax highlighting (also .cfg files)
	# NVRAM files have an Import option
	# Images are displayed
	# Text files can be edited ... a simple textarea - with line & column indicators
	
You must press [Exit] to let the interface know you are finished with the File Manager
If you see "ERR SD manager active", you can kill it with:
	FILES WEB STOP|EXIT

==========
 Commands |------------------------------------------------------------------------------------------------------------
==========

The file commands are restricted to specific files:
	*.txt      - user script data files
	*.var      - user script State files
	*.nvram    - nvram backups
	/sys/*.txt - system script data files
	/sys/*.var - system script State files

Script files themselves are handled by:
	LIST

There are a number of commands for file manipulation:
	FDIR    [*|txt|var|nvram]     # [default: *]

List all files (of the specified type)
	FDIR [*]             # list    all    files
	FDIR txt|var|nvram   # list requested filetype

		txt   = script data  \__ system data/States are prefixed /sys/
		var   = script State /
		nvram = nvram backup

Names for FSTAT, FCAT, FAPP and FDELETE, always require the file extension
The handling is different based on the origin of the request

     Name           | Console         | User Script     | System Script
    ====================================================================
          *.txt|var | /scripts/data/* | /scripts/data/* | /sys/data/*
          *.nvram   | /sys/*.nvram    | ERROR           | /sys/*.nvram
                    |                 |                 |
     /sys/*.txt|var | /sys/data       | ERROR           | ERROR
     /sys/*.nvram   | ERROR           | ERROR           | ERROR
                    |                 |                 |

Get the size and r/w status of a file
	FSTAT <filename>.txt|.var|.nvram         # --> <bytes>:<ro|rw>

Show some-or-all of a file to the UI or DEBUG console [USB/UART]:
	FCAT <filename>.txt|.var|.nvram [start [count]] [>DEBUG]

		start - the first line to be shown
		count - the number of lines to be shown

You can append a line of text to any file SD:/scripts/data/<filename>.txt
	FAPP <filename>.txt|.var|.nvram <text>   # append  <text>"\n" to specified file

CARFUL : Erase a file:
WARNING: There is no confirmation, and no trashcan!
	FDELETE <filename>.txt|.var|.nvram       # Permanently delete a file


 ,----------------------.
(  System/Debug Logging  )=============================================================================================
 `----------------------'

The early boot debug info is forced to both UART and USB.

Once the system is stable, debug logging can be redirected.
	LOG  # log:AUTO debug:0
	LOG <UART|USB|BOTH|AUTO>

$ log debug [all]
BLE=4
WIFI=4
WEB=4
SD=4
SCRIPT=4
IMU=4
SYS=4

$ log debug ble
BLE=4

$ log debug ble <0..4> [save]

0=silent
1=errors
2=warnings
3=info
4=debug

LOG DEBUG SAVE






=======================================================================================================================
-----------------------------------------------------------------------------------------------------------------------
=======================================================================================================================
-----------------------------------------------------------------------------------------------------------------------

























PRINT text  reply text {\n \\ \# \$}
DEBUG text  text to the log port(s)





EVENTS [BTN|IMU|UI|SCRIPT|ALL 0|1]  events sent to the client

BTN [n s l d [TACT|TOUCH]] | BTN [n] SET deb|long|dbl|rpt ms | BTN CAL

LIST [name [from [to]]]  list a script

RUN [name [args]]  list / start a script
LOAD name [args]  load, pause at the first line
STOP [NOW]  stop the script
PAUSE  pause the script
CONT  resume a paused script
STEP [IN|OUT]  run one line (paused)
BPT [[file:]n | DEL ALL|#n|[file:]n]  breakpoints
VARS  variables (paused)
STACK  call stack (paused)

































 ,------------------------.
(  Appendix A : rgb2hsl()  )===========================================================================================
 `------------------------'

// RGB888 -> HSL : h {0..359}, s and l {0..100} (nearest whole numbers)
void  rgb2hsl (uint8_t r8,  uint8_t g8,  uint8_t b8,  int *h,  int *s,  int *l)
{
	int    max = (r8 > g8) ? ((r8 > b8) ? r8 : b8) : ((g8 > b8) ? g8 : b8) ;
	int    min = (r8 < g8) ? ((r8 < b8) ? r8 : b8) : ((g8 < b8) ? g8 : b8) ;

	int    d   = max - min;                // chroma, 0..255

	float  L   = (max + min) / 510.0f;     // 0..1
	float  H   = 0;
	float  S   = 0;

	if (d) {
		float  l2 = 2.0f * L;
		S = d / (255.0f * (1.0f - ((l2 - 1.0f < 0) ? (1.0f - l2) : (l2 - 1.0f)) ) );
		if      (max == r8) H = 60.0f * ((float)(g8 - b8) / d) ;
		else if (max == g8) H = 60.0f * ((float)(b8 - r8) / d + 2.0f) ;
		else                H = 60.0f * ((float)(r8 - g8) / d + 4.0f) ;
		if (H < 0)  H += 360.0f ;
	}

	*h = (int)(H + 0.5f) % 360;
	*s = (int)(S * 100.0f + 0.5f);
	*l = (int)(L * 100.0f + 0.5f);
}

 ,-------------------------.
(  Appendix B : drawPoly()  )==========================================================================================
 `-------------------------'

FN drawPoly $n $cx $cy $r          # n sides, centre cx cy, radius r
  VARS $i $a $x1 $y1 $x2 $y2
  LET $x1 = ROUND($cx + $r) ; LET $y1 = ROUND($cy)
  LOOP $i = 1 TO $n
    LET $a = 360 * $i / $n
    LET $x2 = ROUND($cx + $r * COS($a))
    LET $y2 = ROUND($cy + $r * SIN($a))
    LINE $x1 $y1 $x2 $y2 255 255 255
    LET $x1 = $x2 ; LET $y1 = $y2
  END
END

-----------------------------------------------------------------------------------------------------------------------
=======================================================================================================================
