# amux reattach demo
#
#   vhs demo/reattach.tape        (run from the repository root)
#
# Renders demo/reattach.gif: a session carrying a day's worth of agent
# scrollback, replayed in full (the thing you don't want) and then the way
# amux does it by default.
#
# Needs vhs: https://github.com/charmbracelet/vhs
#
# NOTE: `Ctrl+\` below is the default detach key. If your version of vhs
# cannot parse that token, run the demo session with an easier key instead:
# add `-e ^o` to each amux command and replace `Ctrl+\` with `Ctrl+O`.

Output demo/reattach.gif

Set Shell bash
Set FontSize 15
Set Width 1200
Set Height 640
Set Padding 20
Set TypingSpeed 55ms

# ---------------------------------------------------------------- setup ----
# Hidden: build a session holding ~17 MB of history, the way a long day with
# a coding agent leaves one. Not recorded, so it costs the GIF nothing.
Hide
Type `export PATH="$PWD:$PATH" AMUX_SOCKET_DIR="$(mktemp -d)" PS1='$ '`
Enter
Type "clear"
Enter
Type `amux -n agent sh -c 'seq 1 2000000; while :; do printf "agent> "; read q || exit 0; echo "  ...answering: $q"; done'`
Enter
Sleep 15s
Type "clear"
Enter
Show

# ------------------------------------------------- replaying everything ----
Sleep 1s
Type "# a long-running agent session: ~17 MB of scrollback"
Enter
Sleep 1.5s
Type "# replaying all of it is the thing you don't want:"
Enter
Sleep 1s
Type "amux -s full -a agent"
Enter
Sleep 6s
Ctrl+\
Sleep 2s

# ------------------------------------------------------ the amux default ----
Type "clear"
Enter
Sleep 500ms
Type "# amux replays only what your terminal can actually keep:"
Enter
Sleep 1s
Type "amux -a agent"
Enter
Sleep 3s

# land straight on the live prompt, and prove it is live
Type "still there?"
Enter
Sleep 3s
Ctrl+\
Sleep 2s

# ---------------------------------------------- nothing has been thrown away ----
Type "# ...and the full history is still recorded:"
Enter
Sleep 1s
Type "amux -H agent | wc -l"
Enter
Sleep 4s
