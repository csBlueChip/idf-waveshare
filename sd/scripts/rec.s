# rec.s - a long command for STOP NOW
PRINT "recording 20 s - try STOP, then STOP NOW"
IMU REC 20000 rectest -f
PRINT "rec ended: ok=$.ok err=$.err"
