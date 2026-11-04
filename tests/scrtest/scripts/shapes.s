# shapes.s (spec example)
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
