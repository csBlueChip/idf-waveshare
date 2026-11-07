# -file, -input, -seed and -dump (see t_opts.opt)
FSEEK quiz TAG B=2
FREAD $q quiz EOL
PRINT "tag 2: $q"
INPUT $a
PRINT "input 1: $a ($.ok)"
INPUT $b
PRINT "input 2: $b ($.ok)"
INPUT $c 500
PRINT "input 3: '$c' ($.ok)"
LET $r = RND(1, 1000)
PRINT "rnd: $r"
LET $$ans = "$a $b"
VARS SAVE t_opts $$ans
