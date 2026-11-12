NOTEPAD++ SYNTAX COLOURS FOR WAVEDEMO SCRIPTS
=============================================

Two Notepad++ User Defined Languages for script files (*.s), with the
same rules and different colours:

  wavescript_dark.xml     "Wavedemo script (dark)"  - for dark themes
  wavescript_light.xml    "Wavedemo script (light)" - for the default
                          white theme

Import ONE of them (both claim the .s extension):

  Language > User Defined Language > Define your language... > Import...
  then restart Notepad++.

  Or copy the file to %AppData%\Notepad++\userDefineLangs\ and restart.

If .s files still open with the built-in Assembly colours, pick the
language by hand: Language > Wavedemo script (dark / light).

Colours: statements (bold) and option words (START, EOL, NOW ...)
blue, built-in functions yellow, AND / OR blue bold, firmware commands
cyan, $x $$x ${x} orange, $.x system variables pink, strings "..."
'...' green, numbers purple, :labels and .STACK .CONST .FILES
orange-red, # comments grey. IF / LOOP / SWITCH / FN ... END fold.

Differences from the SD manager viewer: option words and command names
are coloured wherever they appear (Notepad++ cannot tell the first word
of a statement), and a literal "\#" still starts the comment colour.

The command list is the firmware's registered commands when these files
were written; the word lists match assets/syntax.cfg, the viewer and
tools/mcedit. Not tested in Notepad++ itself (only checked as XML).
