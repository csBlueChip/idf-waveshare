 ,----------.
(  Power On  )
 `----------'

When the device is powered on, it will flash the LED for 100ms
	Autorun Enabled  : Default=Green
	Autorun Disabled : Default=Red

NVRAM variables [system]:
	sys.autorun = <0|1>    [0]        # En/Dis-able Autorun
	sys.aron    = <R G B>  [0 255 0]  # 'Enabled'  flash colour ... <0 0 0> -> flash disabled
	sys.aroff   = <R G B>  [255 0 0]  # 'Disabled' flash colour ... <0 0 0> -> flash disabled
	sys.bootcnt = <n>      [0]        # Boot counter

If an Autorun script [discussed later] (onBoot.s or onWait.s) hangs the device (eg. "LOOP FOREVER ; NEXT"),
hold the BOOT button for 3 seconds to kill it and set  sys.autorun=0

The boot counter increments each time the device boots. It is primarily so we can generate
non-conflicting timestamps, when the device cannot know the real time.
Eg. `bootcnt_uptime` rather than `date_time` - this field is read-only


 ,--------------.
(  UI Selection  )
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
(  Emergency  )
 `-----------'

If you have managed to configure the device so nothing works...

If you do not see the UI menu, hold BOOT for 3 seconds to disable Autorun.

You may now select UI: RESET!


 ,--------------------------.
(  UI : WiFi / Web & Telnet  )
 `--------------------------'

"WEB" and "TELNET" both use WiFi, which is controlled by the following variables:

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

 ,-------------.
(  UI : WEB:80  )
 `-------------'

,------------------------------.
| UI  : WEB:<web.port>         |
| SSID: <wifi.ssid>            |
| Pwd : (hold BOOT)            |
| IP  : <wifi.ip> <wifi.cidr>  |
| WiFi: Waiting...             |
`------------------------------'

Hold the BOOT button to show the password

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
(  UI : Telnet:23  )
 `----------------'

,------------------------------.
| UI  : TELNET:<telnet.port>   |
| SSID: <wifi.ssid>            |
| Pwd : (hold BOOT)            |
| IP  : <wifi.ip> <wifi.cidr>  |
| WiFi: Waiting...             |
`------------------------------'

Hold the BOOT button to show the password

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
(  UI : BLE  )
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
(  UI : USB - Serial  )
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
(  UI : UART  )
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
(  UI : MSC - Flash Drive  )
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
(  UI : RESET!  )
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
(  SDCard Layout  )
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

