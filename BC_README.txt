 ,---------------.
(  SDCard Layout  )====================================================================================================
 `---------------'

/
+--	/images/          : <images and sprites> .. Screen is 172x320
|	+-- *.png         : RGB or RGBA
|
+--	/imu/             : Inertial Measurement Unit
|	+--	imu_cal.log   : IMU calibration & test data
|	+--	*.csv         : IMU recordings
|
+--	/scripts/         : Libraries and User scripts.
|	+--	*.s           : scripts
|	|
|	+--	/data/
|		+-- *.txt     : script assets
|		+-- *.var     : script state files
|
+--	/scrshots/
|	+--	*.png         : screenshots .. default: <timestamp>.png
|
+--	/sys/
|	+--	default.nvram : override firmware defaults
|	+--	reset.nvram   : if exists, loaded at boot time
|	+--	reset.bad     : ...import failed
|	+--	reset.done    : ...import succeeded
|	|
|	+--	*.nvram       : NVRAM exports .. default: <timestamp>.nvram
|	+--	debug.cfg     : console log volume, per device
|	|
|	+--	onBoot.s      : onBoot script .. after early boot
|	+--	onWait.s      : before/instead/after functions for the Wait screen
|	+--	onConnect.s   : before/instead/after functions for the Splash Screen
|	+--	onReset.s     : run during a system reset [qv. UI: RESET!]
|	+--	/data/
|	|	+-- *.txt     : script assets
|	|	+-- *.var     : script state files
|	|
|	+--	splash.png    : splash screen shown when a client connects
|	+--	usbdrive.png  : splash screen shown when USB Flash Drive is active
	+--	uiname.txt    : Web page Title (overrides web.name)
|	|
|	+--	fm.html       : user provided web-based File Manager
|	+--	syntax.cfg    : user provided web syntax higlighter
|	|
|	+--	alias.cfg     : UI aliases
|
+--	/www/
	+--	favicon.ico   : www favicon
	+--	index.html    : replacement Web UI
	+--	cfg           : reserved
	+--	ws            : reserved
	+--	*.*           : any file in this folder will be served

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

Help for additional parameters, add a "?" where you are stuck:
	BTN 1     # n:1 gpio:0 type:tact short:1 long:1 double:1 t_deb:30 t_long:600 t_dbl:300 t_rpt:100
	BTN 1 ?   # usage: BTN n short long double [TACT|TOUCH]


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

The backlight level is calculated using a perceived brightness curve.
The true range is the width of the PWM DAC (eg. 10bit {0..1023})
The value in NVRAM `scr.bl.steps` defined the number of steps you desire
Whatever you choose 0 is off and the last step will be full-brightness
With finer (perceived) control at lower levels.

NVRAM variables [screen]:
	scr.bl.steps = <n>   # integer value for full brightness [default: 20]
	scr.bl       = <n>   # default level at boot {0..scr.bl.steps}

Set the level with:	
	BL <n>               # set backlight brightness {0..scr.bl.steps}

==========
 ROTATION |------------------------------------------------------------------------------------------------------------
==========

The screen can be rotated in 4 ways {0..3}

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

=============
 SCREEN-FLIP |---------------------------------------------------------------------------------------------------------
=============

The screen coordinates can be flipped:
	SCR FLIP P      # perform coordinate flip for [P]ortrait
	SCR FLIP L      # perform coordinate flip for [L]andscape
	SCR FLIP PL     # perform coordinate flip for [L]andscape & [P]ortrait
	SCR FLIP NONE   # disable coordinate flip [default]

If, eg., you install the device "upside down", and you want the screen to be flipped by default,
add `SCR FLIP <mode>` to SD:/sys/onBoot.s

For engineering/debug reasons the code draws a random coloured square, randomly on the edge of the screen.
...If this annoys you, add `CLS <colour>` to SD:/sys/onBoot.s


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

RGBA PNG files are supported. So you CAN paint holes and translucency in your image.

Eg. You can display an image, and then overlay a sprite (and use Alpha for anti-aliasing)

Images are stored in SD:/images/ and must be PNG format.

For test purposes, the first 64 images may be enumerated:
	IMG  # enumerate the first 20 images

...During testing, you may use the number in place of the name.

An image may be displayed anywhere on the screen.
The Top-Left of the screen is {0,0} ...relative to the current ROT(ation)

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

A Script library called shapes.s is provided.
It contains:
	drawPoly()

Eg.
	CALL shapes.drawPoly <sides> <cy> <cy> <radius> <rgb> [<fill>]    # fill:<0|1>, default 0

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

*	Escapes PRINT/DEBUG/TEXT:
		\n        ->  new line.

		\#        ->  #  .. *ALL* #'s in strings MUST be escaped (or they will start a comment)
		\$        ->  $  .. an unescaped $ will be interpreted as a variable name prefix
		\\        ->  \  .. a way to print the escape character

		\"        ->  "\"" -> "  ..  '\"' -> \"  .. If a string is quoted, that encapsulation quote
		\'        ->  '\'' -> '  ..  "\'" -> \'  ..    needs to be escaped inside the encapsulated string

	[FONTS] - TEXT only:
		\1 .. \9  ->  select font (for the rest of this TEXT)

	[COLOURS] - PRINT/DEBUG only .. may be disabled with the ANSI command.
		\i<c>    ->  ink   colour <c>
		\p<c>    ->  paper colour <c>
		\i8 \p8  ->  use "default" colour for ink/paper
		\ip      ->  use "default" colour for ink and paper

		<c> Colours (ANSI standard):
			Web colours are set in SD:/sys/syntax.cfg
			Terminal colours are set by your terminal software/"emulator"
			These are the (generally expected/accepted) colours

			0  / k   -> blK
			1  / r   -> Red           2 / g  ->  Grn         4 / b  ->  Blu
			6  / c   -> Cyn(b+g)      5 / m  ->  Mag(b+r)    3 / y  ->  Yel(r+g)
			7  / w   -> Wht(r+g+b)

			9x / lx  ->  Light(/Bright/Bold)  <x>
			90 / lk  ->  Light(/Bright/Bold)  blK  (aka. Dark Gry)
			93 / ly  ->  Light(/Bright/Bold)  Yel  (making Yel -> Brn)
			97 / lw  ->  Light(/Bright/Bold)  Wht  (making Wht -> Light Gry)

			8  / d   ->  <active default>
				Either: The active INK and PAPER colours (if set in the running script)
					or: ansi.id/ansi.pd                  (in the Web UI)
					or: ANSI SGR 0/39/49

			synonyms:  K  R G B  C M Y  W  [L]ight [D]efault  ... always lower-case

			FYI:
				Lum Order:  0:blk, 4:blu, 1:red, 5:mag, 2:grn, 6:cyn, 3:yel, 7:wht
				Shades   :  0:blk, 90:DGry, 7:LGry, 97:Wht

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
	NV GET <startsWith>       | Show all NVRAM vars that start with the specified string
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

A List of all NV RAM Variables (probably out of date by now - sorry):
	ble.name=*                       # <name> - Bluetooth advertising name [* -> use firmware default]
	ble.pwr=9                        # <dBm> radio power
	ble.ansi.ui=0                    # enable ANSI colour codes for the UI

	wifi.pwr=8.5                     #
	wifi.ssid=*                      #
	!wifi.pwd=[redacted]             #
	wifi.ip=192.168.4.1              #
	wifi.cidr=/24                    #
	wifi.gw=1                        #
	wifi.maxconn=1                   #

	web.port=80                      #
	web.name=                        #
	web.captive=1                    #
	web.hist=50                      #
	web.ansi.ui=1                    # enable ANSI colour codes for the UI

	ser.echo=1                       #

	telnet.port=23                   # <1..65535> port number
	telnet.ansi.ui=1                 # <1|0> enable ANSI colour codes for the UI

	usb.ansi.ui=1                    # <1|0> enable ANSI colour codes for the UI
	usb.ansi.con=1                   # <1|0> enable ANSI colour codes for the Console

	uart.baud=115200                 # Baud rate @8n1
	uart.rx=44                       # <gp>  GPIO pin for UART Rx
	uart.tx=43                       # <gp>  GPIO pin for UART Tx
	uart.ansi.ui=1                   # <1|0> enable ANSI colour codes for the UI
	uart.ansi.con=1                  # <1|0> enable ANSI colour codes for the Console

	disk.read=0,255,0                #
	disk.write=255,0,0               #

	btn.gpio=0 2 4 5 6 7 8 9 10 11   # GPIO pins assignments: first pin is button #1, etc.
	btn.deb=30                       # <ms> debounce time
	btn.dbl=300                      # <ms> double-tap slowest speed
	btn.long=600                     # <ms> long-press time
	btn.rpt=0                        # <ms> repeat/typeomatic timer
	btn.touch=0.015                  # <> touch sensor sensitivity

	imu.arange=4                     # accelerometer range
	imu.arate=224                    # accelerometer sample rate
	imu.grange=512                   # gyroscope range
	imu.grate=224                    # gyroscope sample rate
	imu.gcal=                        # gyroscope calibration
	imu.mot.thr=100                  #
	imu.orient.flat=30               #
	imu.orient.hold=500              #
	imu.step.min=10                  # step counter - DISABLED
	imu.step.thr=200                 # step counter - DISABLED
	imu.still.thr=64                 #
	imu.tap.gap=500                  #
	imu.tap.thr=1.3                  #
	imu.wom=100                      #

	run.files=4                      # script: default max open files

	scr.bl.steps=20                  # maximum screen backlight level (perception curve)
	scr.bl=20                        # default screen backlight level

	sys.autorun=0                    # <1|0> autorun (startup scripts) is enabled
	sys.aron=0,255,0                 # <rgb> boot flash colour for : auto-run:on
	sys.aroff=255,0,0                # <rgb> boot flash colour for : auto-run:off
	sys.ui=BLE                       # default UI: {BLE, WEB, TELNET, USB, UART}

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

The IMU can work out which way up a handheld device is being held and flip the coordinate system
	IMU ORIENT                     # mode:P auto:1                             [default:disabled]
	IMU ORIENT NONE|P|L|ROT        # send Event when the device rotates (update on next redraw)
	IMU ORIENT NONE|P|L|ROT AUTO   # send Event when the device rotates (update now)
		NONE : disable
		P    : Portrait
		L    : Landscape
		ROT  : ROTation rules

Detect lack of motion:
	IMU STILL <ms>                 # send an event when motion stops for <ms>  [default:disabled]
	IMU MOTION <1|0>               # send an event when motion starts for <ms> [default:disabled]

//!! TAP & SIGMO need to be explained here!

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

Erase a file:   ***WARNING***: There is no confirmation, and no trashcan!
	FDELETE <filename>.txt|.var|.nvram       # Permanently delete a file <sic> [qv. photorec]


 ,-----------------.
(  Console Logging  )==================================================================================================
 `-----------------'

The early boot debug info is forced to both UART and USB.

Once the system is stable, debug logging can be redirected.
	LOG                        # log:AUTO debug:0
	LOG <UART|USB|BOTH|AUTO>   # Set logging device

All logging options are valid in all UIs.
By default, logging goes to both, unless one is in use as the UI:
	UI = Web    : Debug -> UART + USB
	     Telnet : Debug -> UART + USB
	     BLE    : Debug -> UART + USB
	     USB    : Debug -> UART
	     UART   : Debug -> USB

Inline Logging can only be enabled when the UI is either UART or USB.
...It simply doesn't make sense in any other UI.
To enable/disable Inline Logging (view logging messages in the UI):
	ENable  : Type "+++" (sans quotes) ; do NOT press <enter> ; wait 1 second  ... (LOG -> log:... debug:1)
	DISable : Type "---" (sans quotes) ; do NOT press <enter> ; wait 1 second  ... (LOG -> log:... debug:0)

Logging noise is on a scale of {0..4}:
	0 : silent
	1 : errors              ESP_LOGE (text starts "E (")
	2 : 1 + warnings        ESP_LOGW (text starts "W (")
	3 : 2 + information     ESP_LOGI (text starts "I (")
	4 : 3 + debug messages  ESP_LOGI (text starts "DBG") and ESP_LOGD & ESP_LOGV

Each devices/code section has its own noise level:
	LOG DEBUG [ALL]   # ble:4 wifi:4 web:4 sd:4 script:4 imu:3 sys:4
	LOG DEBUG BLE     # ble:4
	LOG DEBUG imu     # imu:3

You can set any device to any volume.
	LOG DEBUG <ALL|BLE|WIFI|WEB|SD|SCRIPT|IMU|SYS> <volume>

To make the changes survive a reboot, append SAVE
	LOG DEBUG <ALL|BLE|WIFI|WEB|SD|SCRIPT|IMU|SYS> <volume> SAVE
...Hand edits to your debug.cfg file will be KKEPT, only the specific flag byte will be edited.

You can create a new template debug.cfg
	LOG DEBUG SAVE
...Hand edits to your debug.cfg file will be LOST, The old file will be dumped to the UI & the Console (as a backup).

To send a message to the Debug Console, WITH a terminating <cr>
	DEBUG <text>    # normal text rules apply

Test DEBUG command:
	PRINT <text>    # will produce the same result to the UI

To refresh your memory on text escaping:
	\n      ->  new line.

	\#      ->  #  .. *ALL* #'s in strings MUST be escaped (or they will start a comment)
	\$      ->  $  .. only required in scripts (to prevent variable expansion)
	\\      ->  \  .. a way to print the escape character

	\"      ->  "\"" -> "  ..  '\"' -> \"  .. If a string is quoted, that encapsulation quote
	\'      ->  '\'' -> '  ..  "\'" -> \'  ..    needs to be escaped inside the encapsulated string

	\i[9]c  ->  ink   colour   9 -> light/bright/bold
	\p[9]c  ->  paper colour   c = {0:blk, 4:blu, 1:red, 5:mag, 2:grn, 6:cyn, 3:yel, 7:wht}
	\i8 \p8 ->  ink/paper     -> system default
	\ip     ->  ink AND paper -> system default

	"double" or 'single' quotes are optional
	\" -> produce a " inside a "double-quoted" string
	\' -> produce a ' inside a 'single-quoted' string
	\" or \' as the first character -> used Unquoted-Text rules


 ,--------.
(  Events  )===========================================================================================================
 `--------'

Events act as interrupts.
The core can queue 16 EVENTS, after which, the oldest is lost.

In the UI:   All (active) events are seen as they happen, asynchronously.

In a Script: The queue is cleared when a script starts
             Events arrive between script lines
			 ...You can therefore build "atomic" code with (eg.) `TYX 1 3 10 ; TEXT 1 "hello"`

Each Event type can be enabled/disabled individually:
	EVENTS                              - BTN:1 IMU:1 UI:1 SYS:1 SCRIPT:1
	EVENTS BTN|IMU|UI|SYS|SCRIPT <1|0>  - Enable/Disable Event class

...The UI and the running script each have their own set of flags

Userland Events (may be hidden):
	BTN    <n> SHORT                   - short press (and typematic repeat)
	       <n> DOUBLE                  - double press

	       <n> DOWN                    - long-press
	       <n> REPEAT                  - typomatic inside a Long-Press
	       <n> UP                      - long-release

	IMU    TAP                         - taps (IMU TAP 1 d t)
	       2TAP                        - taps (IMU TAP s 1 t)
	       3TAP                        - taps (IMU TAP s d 1)
	       MOTION                      - movement starts
	       STILL                       - quiet after motion
	       SIGMO                       - significant motion
	       STEP n                      - never sent (STEP not implemented)
	       ORIENT {0..3}|FLAT          - orientation change
	       WAKE                        - woke from IMU SLEEP (or SLEEP failed)

	UI     CONNECT                     - client connects
	       LINK UP|DOWN                - Wi-Fi link established/lost

	SYS    TIME stamp adj:+n|-n|0|new  - clock set from the browser

UI-only Events:
	SCRIPT <name> END                  - script finished normally
	       <name> STOPPED              - script was stopped
	       <name> ERR [file:line] msg  - script error
	       <name> PAUSED file:line     - paused (breakpoint, PAUSE)

Script-only Events (always hidden from UI):
	UI     DISCONNECT                  - client disconnected (never seen by the client)
	TIMER  <n> [cnt]                   - script timers [scripts only]

Timer ticks are merged in to a single Event.
Ie. If you set a 2ms Timer, and read the event 10ms later, you will see a single event of 5 ticks, not 5 2ms events.

=========
 Scripts |-------------------------------------------------------------------------------------------------------------
=========

Scripts start with all Events off

Events may be explicitly enabled/disabled with:
	EVENTS BTN|IMU|UI|SYS|TIMER <1|0>   - Enable/Disable Event class

Or implicitly enabled/disabled with:
	ON BTN|IMU|UI|SYS|TIMER <function>  - enable event  for handler-attach
	ON BTN|IMU|UI|SYS|TIMER OFF         - disable event for handler-detach

	TIMER <n> <ms>                      - enable Timer Events

Scripts can block or release Events to the UI:
	UI EVENTS <n>
		       Errors | Events | Replies + OKs
		      ---------------------------------
		 0  ->    ~   |   ~    |    ~    [silent]
		 1  ->    1   |   ~    |    ~    [*default]
		 2  ->    1   |   1    |    ~    [debug]
		 3  ->    1   |   1    |    1    [verbose]

 ,---------.
(  Buttons  )==========================================================================================================
 `---------'

Each available GPIO pin can be assigned to a Button.

A Button is either:
	# Tact [PtM] -> Gnd  (Internal pull-up)
	# Touch      -> Capacitive sensor

A Tact (Push-to-Make) switch must connect the GPIO pin to Gnd when pressed.

A Touch sensory is a capacitive sensor - not all controllers offer this on all pins (incl. "all" and "none").
The Waveshare happens to offer this on every exposed GPIO pin!

Touch sensors do not require a physical switch, but do require occasional (re-)calibration.

NVRAM variables [buttons]:
	btn.gpio = <list>  [0 2 4 5 6 7 8 9 10 11]   # button GPIO pins {1..N}
	btn.deb  = <ms>    [30]                      # debounce
	btn.long = <ms>    [600]                     # long press
	btn.dbl  = <ms>    [300]                     # double click timeout
	btn.rpt  = <ms>    [0]                       # typomatic control
	
Show Button assignments and default timings:
	BTN              # deb:<ms> long:<ms> dbl:<ms> rpt:<ms> b1:<1|0> ... b10:<1|0>
		deb :<ms>    #   debounce   - wait <ms> for reading to stabalise              [default: 30      ]
		long:<ms>    #   long-press - hold for at least <ms>                          [default: 600     ]
		dbl :<ms>    #   double-tap - two presses within <ms>                         [default: 300     ]
		rpt :<ms>    #   repeat     - repeat key event every <ms> (after first event) [default: 0       ]
		bN  :<1|0>   #   button #N enabled/disabled                                   [default: disabled]

	BTN <n>          # disabled: n:1 gpio:0 type:off
	                 # enabled : n:1 gpio:0 type:tact short:1 long:0 double:1 t_deb:30 t_long:600 t_dbl:300 t_rpt:0
	
	BTN <n> <s l d> [TACT|TOUCH]
		n : {1..N}   # Button number
		s : <1|0>    # enable/disable single-tap Events
		l : <1|0>    # enable/disable long-press Events
		d : <1|0>    # enable/disable double-tap Events
		TACT         # GPIO goes via an SPST PtM to Gnd
		TOUCH        # GPIO is a capacitive touch sensor
	...calling this form, sets all timings to the system default, every time it is called

As mentioned previously, TOUCH sensors need calibration.
This is more an "environment" thing (warm/cold, dry/wet, etc.), than a "per user" thing.
Calibration takes about 1500ms (1.5s) - DO NOT touch it during calibration!
	BTN CAL                               # calibrate a touch sensor

You have a LOT of control over the button timing!
	BTN <n|*> RESET                       # disable all controls, and clear the timings  [* -> all buttons]
	BTN <n|*> <s l d> [TACT|TOUCH]        # Keep current timing, or initilaise if clear  [* -> all buttons]
	BTN <n|*> <0 0 0> [TACT|TOUCH]        # notably does NOT reset the timings!          [* -> all buttons]
	BTN <n|*> SET DEB|LONG|DBL|RPT <ms>   # sets a specific timing value                 [* -> all buttons]
	
	BTN SET DEB|LONG|DBL|RPT <ms>         # sets the global timings, used to initialise buttons [default: NVRAM]
	BTN RESET                             # as BTN * RESET, plus the global timings are reloaded from NVRAM

With some lateral thinking, every timing option is available:
	You want DOWN [REPEAT] UP events, but don't want to wait a "long" time for it to start?
		...BTN n 0 1 0 ; BTN n SET LONG 50 ; BTN n SET RPT 120
	You want a held button to auto-repeat {TAP TAP TAP ...}?
		...BTN n 1 0 0 ; BTN n SET SHORT 50 ; BTN n SET RPT 120

It is reasonable to have different user profiles saved as SD:/sys/user1_btn_prefs.nvram
Then when 'user1' takes control:
	NV LOAD user1_btn_prefs
	BTN RESET
	CALL buttonSetup()

Scripts may find it useful to get a list of active buttons:
	BTN LIST                              # returns "1 3 4 5 7" to say "buttons 1,3,4,5,7 are active"


=======================================================================================================================
-----------------------------------------------------------------------------------------------------------------------
=======================================================================================================================
-----------------------------------------------------------------------------------------------------------------------

=======================================================================================================================

=======================================================================================================================
=======================================================================================================================

LIST [name [from [to]]]  list a script

LOAD name [args]  load, pause at the first line
RUN [name [args]]  list / start a script

PAUSE  pause the script
CONT  resume a paused script
STOP [NOW]  stop the script

STEP [IN|OUT]  run one line (paused)
BPT [[file:]n | DEL ALL|#n|[file:]n]  breakpoints

VARS  variables (paused)
STACK  call stack (paused)

=======================================================================================================================
































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

 ,---------------------------------.
(  Appendix C : Smooth light curve  )==================================================================================
 `---------------------------------'

It has no formal algorithm name. 
It’s usually called CIE L* lightness correction or a perceptual brightness (gamma) correction table.

In LED and maker circles it’s often mislabelled a “CIE 1931 correction curve”, 
but the L* formula actually comes from CIE 1976.

int  list[STEPS] = {0};

int*  getSteps (int max,  int steps)
{
	list[0] = 0;
	for (int i = 1;  i <= steps;  i++) {
		list[i] = getStep(max, steps, i);
	}
	return list;
}

int  getStep (int max,  int steps,  int i)
{
    double L = (100.0 * i) / steps;
    double Y;
    if (L > 8.0) {
        double t = (L + 16.0) / 116.0;
        Y = t * t * t;
    } else {
        Y = L / 903.3;
    }
    return (int)(Y * max + 0.5);
}

-----------------------------------------------------------------------------------------------------------------------
=======================================================================================================================
