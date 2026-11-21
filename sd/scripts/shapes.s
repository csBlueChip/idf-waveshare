# shapes.s - drawing library
#   drawPoly n cx cy r colour [fill]   regular polygon:
#     n       sides 3..64
#     cx cy   centre (pixels)
#     r       radius (pixels, > 0)
#     colour  r,g,b | RRGGBB | @h,s,l
#     fill    0 = outline (default), 1 = filled
#   From the UI:     CALL shapes.drawPoly 6 86 160 40 255,255,255
#   From a script:   CALL shapes.drawPoly 6 86 160 40 0,200,255 1
FN drawPoly
  VARS $n $cx $cy $r $c $f $i $a $x1 $y1 $x2 $y2
  IF $arg[0] < 5 OR $arg[0] > 6
    PRINT "usage: drawPoly n cx cy r colour [fill] (n 3..64, r > 0, fill 0|1)"
    RETURN
  END
  LET $n = $arg[1] ; LET $cx = $arg[2] ; LET $cy = $arg[3] ; LET $r = $arg[4]
  LET $c = $arg[5] ; LET $f = 0
  IF $arg[0] == 6
    LET $f = $arg[6]
  END
  IF $n < 3 OR $n > 64 OR $r <= 0 OR ($f != 0 AND $f != 1)
    PRINT "usage: drawPoly n cx cy r colour [fill] (n 3..64, r > 0, fill 0|1)"
    RETURN
  END
  LET $x1 = ROUND($cx + $r) ; LET $y1 = ROUND($cy)
  LOOP $i = 1 TO $n
    LET $a = 360 * $i / $n
    LET $x2 = ROUND($cx + $r * COS($a))
    LET $y2 = ROUND($cy + $r * SIN($a))
    IF $f
      FTRI $cx $cy $x1 $y1 $x2 $y2 $c
    ELSE
      LINE $x1 $y1 $x2 $y2 $c
    END
    LET $x1 = $x2 ; LET $y1 = $y2
  END
END
