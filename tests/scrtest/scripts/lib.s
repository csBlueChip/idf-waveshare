# a library: top-level code never runs when called
PRINT lib top level - must not appear
FN greet $who
  PRINT Hello, $who
  CALL helper
END
FN helper
  PRINT helper in lib
END
FN wrap $t
  RETURN "[" $t "]"
END
