TESTING SCRIPTS ON A PC
=======================

scrtest is the real script interpreter (src/scr_load.c, scr_eval.c,
scr_exec.c) built for the PC with a fake device (host.c). Use it to run,
step and debug .s scripts without the board. Not part of the firmware.


1. BUILD
--------

Needs gcc (Linux, macOS, or WSL on Windows).

  cd tests/scrtest
  sh run.sh             build /tmp/scrtest and run every test (section 5)

The program is built at $TMPDIR/scrtest (default /tmp/scrtest).


2. RUN A SCRIPT
---------------

  /tmp/scrtest dir name [args] [options]

  dir     folder holding name.s (and any file.s it CALLs or INSERTs)
  name    script name without .s
  args    become $arg, as with RUN name args

  /tmp/scrtest ../../sd/scripts persist
  /tmp/scrtest ../../sd/scripts persist forget

Output: "> text" for PRINT, <<SCRIPT ...>> messages as the client would
see them, "CMD: line" for each device command, and a last line
[result END|STOPPED|ERROR|NOFN].


3. OPTIONS
----------

  Debugger:
    -load             pause before the first line (as LOAD)
    -bpt file:n       breakpoint at line n of file.s (repeat for more)
    -go A,B,...       answers for the pauses, in order: CONT STEP IN OUT
                      STOP (when they run out: CONT)
    -fn name          run only FN name, not the top-level code (as the
                      system scripts' before / instead / after handlers)
  At each pause the call stack and the variables are printed.

  Data and input:
    -file name=path   load a disk file as data file name before the run;
                      repeat for more. name includes the extension:
                        -file boast.txt=../../sd/scripts/data/boast.txt
                        -file boast.var=saved.var   (start from a save)
    -input a,b,c      INPUT answers, in order; when they run out INPUT
                      times out ($.ok = 0). No spaces or commas inside an
                      answer (use XINPUT in the script for those)
    -seed n           start of the random generator (default 1): each n
                      gives a different, repeatable run (RND, $.rnd)
    -dump name        print data file name after the run, between
                      [name] and [end] (repeat for more)

  Example - play boast.s in mode c until the answers run out:
    /tmp/scrtest ../../sd/scripts boast c \
        -file boast.txt=../../sd/scripts/data/boast.txt \
        -input 1,1,2 -seed 5 -dump boast.var


4. THE FAKE DEVICE
------------------

  Commands: device commands are printed as "CMD: line" and reply OK.
    These work on in-memory data files (empty at the start unless
    loaded with -file; gone when the run ends):
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
  Time is virtual: WAIT, WAIT EVENT and INPUT time-outs move it on at
  once; TIMER ticks are made on it. $.runtime / $.uptime follow it.
  Built-ins ($.ax, $.bat ...) have fixed values.

  Not modelled: the screen, IMU, buttons (except by EVT), the console
  and its !!STOP / !!PAUSE handling, NV settings (fixed values), real SD
  files and the system scripts' boot order.


5. THE TEST SUITE
-----------------

  sh run.sh             build, then run scripts/t_*.s
  sh run.sh -update     rewrite every t_*.out from the current output
                        (only after checking the differences!)

  Each t_name.s is run with the words in t_name.opt (if present) as extra
  arguments; paths in .opt files are relative to tests/scrtest. Its
  output is compared with t_name.out: PASS, or FAIL with the output left
  in t_name.got (diff t_name.out t_name.got). The last line is
  "passed n, failed n".

  To add a test: write scripts/t_name.s (and t_name.opt), run it once,
  check the output by hand, save it as t_name.out, and describe the
  files in scripts/DESCRIPT.ION.
