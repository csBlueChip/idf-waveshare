# control flow
LET $x = 5
IF $x < 3
  PRINT bad1
ELIF $x < 6
  PRINT elif ok
ELIF $x < 9
  PRINT bad2
ELSE
  PRINT bad3
END
IF 0 ; PRINT bad ; ELSE ; PRINT else ok ; END
LET $s = "x"
LOOP $i = 1 TO 5
  LET $s = $s $i
END
PRINT to: $s after $i
LET $s = ""
LOOP $i = 10 TO 1 STEP -3
  LET $s += "n$i"
END
PRINT down: $s after $i
LOOP $i = 5 TO 1
  PRINT never
END
PRINT empty loop i=$i
LET $n = 0
LOOP WHILE $n < 3
  $n++
END
PRINT while n=$n
LET $c = 0
LOOP 3.9 TIMES ; $c++ ; END
PRINT times c=$c
LOOP 0 TIMES ; PRINT never ; END
LET $c = 0
LOOP FOREVER
  $c++
  IF $c >= 4 ; BREAK ; END
END
PRINT forever c=$c
LET $out = ""
LOOP $i = 1 TO 3
  LOOP $j = 1 TO 3
    IF $j == 2 ; NEXT ; END
    IF $i == 2 ; NEXT $i ; END
    IF $i == 3 AND $j == 3 ; BREAK 2 ; END
    LET $out += "$i:$j"
  END
END
PRINT nested: $out
SWITCH $x
1..4)  PRINT bad
5|7)   PRINT switch five
       PRINT second line
*)     PRINT bad default
END
SWITCH ~"TaP" ; tap) PRINT ci ok ; *) PRINT bad ; END
SWITCH "zzz" ; a) PRINT bad ; *) PRINT default ok ; END
SWITCH 3 ; 1..2|9) PRINT bad ; 3..5) PRINT range ok ; END
SWITCH "q" ; a) PRINT bad ; END
PRINT no match ok
LET $k = 0
:again
$k++
IF $k < 3 ; GOTO again ; END
PRINT goto k=$k
LOOP $i = 1 TO 10
  IF $i == 2 ; GOTO out ; END
END
:out
PRINT left loop i=$i
EXIT
PRINT never
