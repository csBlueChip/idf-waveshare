# .FILES n raises the limit for this run (run.files is 4 in the PC host)
.FILES 6
ONERROR CONT
FTELL $p a ; FTELL $p b ; FTELL $p c ; FTELL $p d ; FTELL $p e ; FTELL $p f
PRINT six open: $.ok
FTELL $p g
PRINT seventh: $.err
