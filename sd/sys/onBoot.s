# sys/onBoot.s - runs at start-up when NV sys.autorun is 1
LED 0 0 255
WAIT 1000
LED 0 0 0
DEBUG "onBoot ran (uptime $.uptime ms)"
