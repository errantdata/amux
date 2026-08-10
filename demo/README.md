# Demo assets

`reattach.tape` is a [VHS](https://github.com/charmbracelet/vhs) script. VHS
drives a real terminal from a text file and renders a GIF, so the demo is
reproducible and re-renderable when the interface changes — not a one-off
screen capture nobody can regenerate.

## Rendering

```sh
# macOS / Linuxbrew
brew install vhs

# or grab a release binary: https://github.com/charmbracelet/vhs/releases

vhs demo/reattach.tape        # from the repository root
```

That writes `demo/reattach.gif`, which `README.md` embeds. **Render and commit
the GIF before pushing publicly**, or the README shows a broken image.

`make demo` does the same thing and tells you what to install if vhs is
missing.

## What the tape shows

A session carrying ~17 MB of scrollback, the way a long day with a coding
agent leaves one:

1. `amux -s full -a agent` — replaying the whole thing, which crawls; the
   demo gives up and detaches after a few seconds
2. `amux -a agent` — the default, landing on the live `agent>` prompt at
   once, and answering a question to show the session is live
3. `amux -H agent | wc -l` — the full history is still recorded

Setup (generating the 2,000,000 lines) happens between `Hide` and `Show`, so it
executes without being recorded and costs the GIF nothing.

## What this cannot show

Native scrolling. VHS renders a fixed-height terminal with no scrollback to
interact with, so there is no way to demonstrate a wheel scroll into real
terminal scrollback. That claim needs an actual screen recording of a human
scrolling in a real terminal — worth doing separately if you want it in the
README.

## Adjusting

- `Ctrl+\` is the default detach key. If your vhs cannot parse that token, add
  `-e ^o` to the amux commands in the tape and use `Ctrl+O` instead.
- The `Sleep 15s` in the hidden setup is how long `seq 1 2000000` needs to
  fill the ring. On a slower machine, raise it — if it is too short the first
  replay chases a still-writing session and the timings look wrong.
- `Sleep 6s` after `amux -s full` is how long the crawl is shown. Longer is
  more damning but makes the GIF heavier.
