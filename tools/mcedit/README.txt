MCEDIT SYNTAX COLOURS FOR WAVEDEMO SCRIPTS
==========================================

wavescript.syntax colours script files (*.s) in mcedit (Midnight
Commander's editor): statements and option words yellow, built-in
functions blue, AND / OR and numbers cyan, $x $$x ${x} green, $.x system
variables magenta, strings green (escapes and $vars inside bright),
:labels and .STACK .CONST .FILES red, # comments brown, firmware
commands cyan.

Install (checked with mc 4.8.30; the user folder is
~/.local/share/mc/syntax):

  mkdir -p ~/.local/share/mc/syntax
  cp wavescript.syntax ~/.local/share/mc/syntax/

  # a user Syntax file replaces the system one, so start from a copy;
  # the wavedemo lines must come BEFORE the system's assembler entry
  # (it also claims .s)
  ( printf 'file ..\\*\\\\.s$ Wavedemo\\sscript\ninclude wavescript.syntax\n\n'
    cat /usr/share/mc/syntax/Syntax ) > ~/.local/share/mc/syntax/Syntax

The two lines it adds at the top of ~/.local/share/mc/syntax/Syntax:

  file ..\*\\.s$ Wavedemo\sscript
  include wavescript.syntax

Other file types keep their colours (their syntax files are still
found in the system folder). If an old ~/.local/share/mc/syntax/Syntax
exists, add the two lines at its top instead.

Differences from the SD manager viewer: mcedit cannot tell the first
word of a statement, so option words (START, EOL, NOW ...) and command
names are coloured wherever they appear, and a literal "\#" can still
start the comment colour.

The command list is the firmware's registered commands when this file
was written; the word lists match assets/syntax.cfg and the viewer.
