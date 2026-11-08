TESTING SCRIPTS ON A PC
=======================

scrtest is the real script interpreter (src/scr_load.c, scr_eval.c,
scr_exec.c) built for the PC with a fake device (host.c). Use it to run,
play, step and debug .s scripts without the board. Not part of the
firmware.


1. BUILD
--------

Needs gcc (Linux, macOS, WSL or MSYS2 on Windows).

  cd tests/scrtest
  make                  build ./scrtest here
  sh run.sh             build /tmp/scrtest and run every test (section 5)


2. RUN A SCRIPT
---------------

  ./scrtest SD name [args] [options]

  SD      the card's root folder - normally the project's sd/ (../../sd)
  name    script name without .s: scripts/name.s, or sys/name for
          SD/sys/name.s (as RUN on the device); CALL file.fn and INSERT
          files come from the same places
  args    become $arg, as with RUN name args

  Data files are the real files in SD/scripts/data: FREAD / FSEEK read
  them, FWRITE / FLOG / VARS SAVE write them, FDELETE deletes them -
  exactly where the device would.

  ./scrtest ../../sd boast a -i         play boast.s (mode a) at the keyboard;
                                        boast.var is saved in sd/scripts/data
  ./scrtest ../../sd boast forget       forget what was learned
  ./scrtest ../../sd persist -i

Output: PRINT text ("> text" without -i), <<SCRIPT ...>> messages as
the client would see them, "CMD: line" for each device command, and a
last line [result END|STOPPED|ERROR|NOFN].


3. OPTIONS
----------

  Play:
    -i                interactive: INPUT reads a line from the keyboard,
                      with a real time-out (INPUT $v 15000 waits 15 s);
                      PRINT is plain text; WAIT, WAIT EVENT and TIMERs
                      take real time. Ctrl+D (end of input) = no client;
                      Ctrl+C stops.

  Debugger:
    -load             pause before the first line (as LOAD)
    -bpt file:n       breakpoint at line n of file.s (repeat for more)
    -go A,B,...       answers for the pauses, in order: CONT STEP IN OUT
                      STOP (when they run out: CONT)
    -fn name          run only FN name, not the top-level code (as the
                      system scripts' before / instead / after handlers)
  At each pause the call stack and the variables are printed.

  Input and checks:
    -input a,b,c      INPUT answers, in order; when they run out INPUT
                      times out ($.ok = 0). No spaces or commas inside an
                      answer (use XINPUT in the script for those)
    -seed n           start of the random generator (default 1): each n
                      gives a different, repeatable run (RND, $.rnd)
    -dump name        print data file name after the run, between
                      [name] and [end] (repeat for more)

  Test-suite layout:
    -test             SD is the folder holding the scripts themselves;
                      data files live in memory (empty at the start,
                      nothing written to disk). Used by run.sh.
    -file name=path   (-test only) load a disk file as data file name:
                        -file boast.txt=../../sd/scripts/data/boast.txt

  Example - a repeatable, non-interactive run:
    ./scrtest ../../sd boast c -input 1,1,2 -seed 5 -dump boast.var


4. THE FAKE DEVICE
------------------

  Commands: device commands are printed as "CMD: line" and reply OK.
    These work on the data files (real files, or in memory with -test):
      FLOG f text, FSTAT f, FDELETE f, FCAT f (f, f.txt or f.var)
      FWRITE FREAD FTELL FSEEK FCLOSE, VARS SAVE / LOAD
  Extra lines a test script can use (they are commands, so they work
  as script lines):
      EVT text          queue an event, e.g. EVT BTN 1 SHORT
      XINPUT text       queue an INPUT answer (spaces allowed)
      XCLIENT 0|1       client disconnected / connected (no client:
                        INPUT returns at once, $.ok = 0)
      XSTOP / XPAUSE    STOP / PAUSE request, seen before the next line
      XGO mode          queue a debugger answer (as -go)
      FAIL reason       reply ERR reason
      REPLY a b c       reply one line per word, then OK
      XRO f             make data file f.txt read-only
  Time is virtual without -i: WAIT, WAIT EVENT and INPUT time-outs move
  it on at once; TIMER ticks are made on it. $.runtime / $.uptime follow
  it. With -i it is real time.
  Built-ins ($.ax, $.bat ...) have fixed values.

  Not modelled: the screen, IMU, buttons (except by EVT), the console
  and its !!STOP / !!PAUSE handling, NV settings (fixed values) and the
  system scripts' boot order.


5. THE TEST SUITE
-----------------

  sh run.sh             build, then run scripts/t_*.s and card/card_check.s
  sh run.sh -update     rewrite every .out from the current output
                        (only after checking the differences!)

  Each scripts/t_name.s is run with -test and the words in t_name.opt
  (if present) as extra arguments; paths in .opt files are relative to
  tests/scrtest. Its output is compared with t_name.out: PASS, or FAIL
  with the output left in t_name.got (diff t_name.out t_name.got).

  card/card_check.s checks the card layout: run.sh copies the project's
  sd/ to a temporary folder, runs the script there (real data files),
  lists what is left in scripts/data and compares with
  card/card_check.out. The project's sd/ is never written.

  The last line is "passed n, failed n".

  To add a test: write scripts/t_name.s (and t_name.opt), run it once,
  check the output by hand, save it as t_name.out, and describe the
  files in scripts/DESCRIPT.ION.
