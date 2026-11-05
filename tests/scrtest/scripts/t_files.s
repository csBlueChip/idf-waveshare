# data files: FWRITE FREAD FTELL FSEEK FCLOSE, FLOG FSTAT FDELETE (positions from 1)
ONERROR CONT
FTELL $p log
PRINT new file position $p
FWRITE log "line one\nline two\n"
FTELL $p log
FSTAT log
PRINT after write: pos=$p stat=$.ret
FSEEK log START
FREAD $a log EOL
FREAD $b log EOL
FREAD $c log EOL
PRINT eol: [$a] [$b] [$c] ok=$.ok err=$.err
FSEEK log SET 6
FREAD $w log 3
PRINT set 6 read 3: [$w]
FSEEK log OFFS -4
FTELL $p log
PRINT offs -4: $p
FSEEK log END
FTELL $p log
PRINT end: $p
FSEEK log END 4
FREAD $t log 10
PRINT end 4 read 10: [$t] ok=$.ok err=$.err
FSEEK log SET -5
FTELL $p log
PRINT clamp low: $p
FSEEK log SET 999
FTELL $p log
PRINT clamp high: $p
FSEEK log SET 6
FWRITE log "ONE"
FSEEK log START
FREAD $a log EOL
PRINT overwrite: [$a]
FLOG log appended by FLOG
FSEEK log START
FREAD $x log 200
PRINT whole: [$x] err=$.err
FREAD $y nofile 1
PRINT missing: $.err
FWRITE bad-name x
PRINT bad name: $.err
FTELL $q f2 ; FTELL $q f3 ; FTELL $q f4
FTELL $q f5
PRINT five: $.err
FCLOSE f2
FTELL $q f5
PRINT after fclose: pos $q
XRO log
FWRITE log "x"
PRINT ro: $.err
FSTAT log
PRINT stat: $.ret
FDELETE log
PRINT delete ro: $.err
LET $$score = 12.5
LET $$name = 'say "hi"\nnext'
LET $local = 7
VARS SAVE game
VARS SAVE game2 $$score $local
LET $$score = 0 ; LET $$name = "" ; LET $local = 0
VARS LOAD game $$score
PRINT load list: score=$$score name=[$$name]
VARS LOAD game
PRINT load all: score=$$score name=[$$name]
VARS LOAD game2 ALL
PRINT game2: local=$local
VARS LOAD game2 $$nothere
PRINT missing var: $.err
VARS LOAD nosuch
PRINT missing file: $.err
