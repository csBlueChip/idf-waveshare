# ANSI colour: escapes, INK / PAPER, line ends, bad escapes, files stripped
PRINT "plain"
PRINT "\i3yellow\i8 back \i91bright red \p4on blue\p8 \ip done"
PRINT "a\i2\nb"
INK 6
PRINT "cyan line \i1red\i8 cyan"
PAPER 94
PRINT "on bright blue"
DEBUG "dbg \i2x"
LET $m = "\i5mag"
PRINT "var: $m"
FWRITE col "\i3x\p1y"
FSEEK col START
FREAD $r col EOL
PRINT "file: [$r]"
INK 8
PAPER 8
PRINT "uncoloured"
ONERROR CONT
PRINT "\ix"
PRINT "bad: $.err"
INK 98
PRINT "bad ink: $.err"
PRINT "letters: \ir r\ilg G\pb B\pdx \iwd\ik k\icc\imm\iyy\i9yY\pl0p\ip"
INK lr
PAPER k
PRINT "INK lr PAPER k"
INK d
PAPER d
PRINT "\iR"
PRINT "bad: $.err"
PRINT "\ild"
PRINT "bad: $.err"
INK R
PRINT "bad ink: $.err"
