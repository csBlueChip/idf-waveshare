# ask.s - INPUT and a timer
PRINT "name? (10 s)"
INPUT $name 10000
IF $.ok ; PRINT "hello $name" ; ELSE ; PRINT "no answer" ; END
TIMER 1 200
WAIT 1000
WAIT EVENT
PRINT "timer event: $.event"
TIMER 1 0
