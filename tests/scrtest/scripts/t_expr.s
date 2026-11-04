# expressions and values
LET $a = 3.14159
PRINT a=$a
LET $id = "007"
PRINT id=$id
IF $id == 7 ; PRINT "007 == 7" ; END
LET $r = 2 + 3 * 4 ; PRINT 14: $r
LET $r = (2 + 3) * 4 ; PRINT 20: $r
LET $r = 2 - 3 - 4 ; PRINT -5: $r
LET $r = 7 % 3 ; PRINT 1: $r
LET $r = -7.5 % 2 ; PRINT -1.5: $r
LET $r = 7.5 % -2 ; PRINT 1.5: $r
LET $r = (1 << 4) (0xFF & 0x0F) (5 ^ 3) (4 | 1) ~0 (256 >> 2) 0b101
PRINT bits 16 15 6 5 -1 64 5: $r
LET $r = (1 < 2) (2 <= 2) (3 > 4) (3 >= 3) ("abc" == "abc") ("abc" != "ABC") ("abc" ~= "ABC")
PRINT cmp 1 1 0 1 1 1 1: $r
LET $r = ("abc" < "abd") ("10" < "9") ("x10" < "x9")
PRINT textcmp 1 0 1: $r
LET $r = !0 !1 !"" !"a" (!$a == 0)
PRINT not 1 0 1 0 1: $r
LET $n = 0
IF $n != 0 AND 10 / $n > 2 ; PRINT bad ; ELSE ; PRINT lazy AND ok ; END
IF $n == 0 OR 10 / $n > 2 ; PRINT lazy OR ok ; END
LET $r = INT(-2.7) ROUND(2.5) ABS(-3) MIN(4, 2) MAX(4, 2) SQRT(16) ROUND(SIN(30) * 1000) ROUND(COS(60) * 1000) ATAN2(1, 1)
PRINT funcs -2 3 3 2 4 4 500 500 45: $r
LET $r = LEN("hello") UPPER("abc") LOWER("ABC") MID("abcdef", 2, 3) "[" MID("abc", 5, 2) "]" FIND("abcabc", "ca") FIND("abc", "x")
PRINT textf 5 ABC abc bcd [  ] 3 0: $r
LET $r = $a $id
PRINT join: $r
LET $j = $id -1
PRINT operator wins 6: $j
LET $k = $id "-1"
PRINT join wins 007 -1: $k
LET $byte = 45
LET $$g = 45
LET $pi = 3.14159
LET $neg = -2.7
PRINT fmt 00101101 2D 3.14 0045 3 -10: ${byte/2,8} $${g/16,2} ${pi/10,0,2} ${byte/10,4} ${pi/16} ${neg/2}
LET $big = 1e3 ; LET $sm = 2.5e-2
PRINT num 1000 0.025 0.333: $big $sm 
LET $t = 1 / 3 ; PRINT third $t
