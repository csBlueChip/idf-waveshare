# .FILES 2 lowers it
.FILES 2
ONERROR CONT
FTELL $p a ; FTELL $p b
FTELL $p c
PRINT third: $.err
