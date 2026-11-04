# dbg.s - for BPT! with no client (copy SD:/scripts/dbg.s over SD:/sys/onBoot.s): USB serial is the console
LET $x = 1
BPT!
LET $x = 2
DEBUG "dbg done x=$x"
