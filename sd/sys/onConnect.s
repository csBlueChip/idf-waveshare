# sys/onConnect.s - system script around the connect splash (NV sys.autorun 1)
# $arg = UI:name, e.g. UI:WEB
#
# All three functions are optional; they are here as a template.
# An EMPTY function is ignored. For 'instead' that means the built-in splash
# is used. A single RETURN (or EXIT) is enough to make 'instead' a real
# handler that replaces the built-in splash.

FN before
END

FN instead
END

FN after
  PRINT "welcome ($arg)"
END
