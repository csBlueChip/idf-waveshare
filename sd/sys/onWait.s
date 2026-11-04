# sys/onWait.s - system script around each Wait screen (NV sys.autorun 1)
# $arg = the details, e.g. UI:WEB:80|SSID:x|Pwd:y|IP:192.168.4.1|Mask:/24|Link:Waiting
#
# All three functions are optional; they are here as a template.
# An EMPTY function is ignored. For 'instead' that means the built-in Wait
# screen is used. A single RETURN (or EXIT) is enough to make 'instead' a
# real handler that replaces the built-in screen (and its BOOT gestures).

FN before
  DEBUG "onWait before: $arg[1] $arg[2]"
END

FN instead
END

FN after
  DEBUG "onWait after"
END
