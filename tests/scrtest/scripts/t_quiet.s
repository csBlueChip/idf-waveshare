# QUIET levels (default 2): replies / OK / ERR shown per level
ONERROR CONT
PRINT default
REPLY a
FAIL e2
QUIET 0
PRINT quiet 0
REPLY a
FAIL e0
QUIET 1
PRINT quiet 1
REPLY a
FAIL e1
QUIET 3
PRINT quiet 3
REPLY a
FAIL e3
QUIET 4
PRINT bad level: $.err
