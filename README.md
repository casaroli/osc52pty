# osc52pty

Copy to your Mac's clipboard from a remote machine, in terminals that don't
support OSC 52 (like Terminal.app).

OSC 52 is the escape sequence programs use to set the clipboard through the
terminal: `ESC ] 52 ; c ; <base64> BEL`. tmux, Neovim and others send it,
and it travels fine over ssh — but Terminal.app ignores it.

`osc52pty` runs a command (say, `ssh`) in a pseudo-terminal, watches its
output, cuts out OSC 52 sequences and pipes their decoded text to `pbcopy`.
Everything else passes through untouched.

A shell/C rewrite of [roy2220/osc52pty](https://github.com/roy2220/osc52pty).
macOS only.

## Two versions

Both behave the same and take the same arguments. Install one of them; both
are called `osc52pty`.

| | zsh script (`osc52pty`) | C program (`c/`) |
|---|---|---|
| Needs | nothing, macOS ships zsh | a C compiler to build |
| Throughput of output | ~10–20 MB/s | ~100–125 MB/s, same as bare `script(1)` |
| Startup | ~10 ms | ~4 ms |
| Processes | 3 (wraps `script(1)`) | 1 |
| Typed ahead before the command starts | may be dropped | kept |

The zsh version is plenty for an interactive ssh session; the C version is
there if you `cat` big files or want it lean.

## Install

### zsh script

```sh
install -m 755 osc52pty /usr/local/bin/osc52pty
```

or anywhere else on your `PATH`, e.g. `~/.local/bin`.

### C program

Needs the Xcode Command Line Tools (`xcode-select --install`) for `cc` and
`make`.

```sh
cd c
make
make install              # to /usr/local/bin
make install PREFIX=~/.local   # or elsewhere
```

`make uninstall` (same `PREFIX`) removes it.

## Usage

```sh
osc52pty ssh myhost
osc52pty ssh -p 2222 user@host
```

With no command it runs your `$SHELL`. Its exit status is the command's.

To use it for every ssh, add an alias to `~/.zshrc`:

```sh
alias ssh='osc52pty ssh'
```

### On the remote side

Anything that emits OSC 52 works. To check, run this on the remote machine
and then paste locally:

```sh
printf '\033]52;c;%s\a' "$(printf 'THE TEXT' | base64)"
```

A small helper to put in the remote `~/.bashrc` / `~/.zshrc`:

```sh
# usage: some-command | osc52copy
osc52copy() { printf '\033]52;c;%s\a' "$(base64 | tr -d '\n')"; }
```

**tmux** forwards OSC 52 to the outer terminal when `set-clipboard` is on.
In the remote `~/.tmux.conf`:

```
set -g set-clipboard on
```

tmux only does this for terminals it believes support it; `xterm*` TERM
values (Terminal.app's default is `xterm-256color`) are covered by default
since tmux 3.2. For anything else add
`set -as terminal-features ',YOUR-TERM:clipboard'`.

**Neovim** has a built-in OSC 52 provider, see `:help clipboard-osc52`.

### Limits

- Copy only. Paste requests (`ESC ] 52 ; c ; ? BEL`) are swallowed and not
  answered, so configure editors to paste with your terminal's normal paste.
- The selection letter (`c`, `p`, `s`, ...) is ignored; everything goes to the
  general pasteboard.
- An unterminated sequence is held back until it ends, up to 16 MB, then
  given up on and printed as is.

### Settings

- `OSC52PTY_COPY` — command that receives the decoded text on stdin, instead
  of `pbcopy`. Run by `/bin/sh -c` in the C version; split into words by
  the zsh version.
- `OSC52PTY=1` is set inside, so scripts can tell they run under it.

## Tests

```sh
zsh test.zsh       # zsh version
cd c && make test  # C version
```

Both feed the parser exact chunk sequences — sequences split at every
possible byte, both terminators (`BEL` and `ESC \`), near misses like
`ESC ] 5 x`, NUL bytes, UTF-8 split across reads, megabytes of random data —
and check what reaches the terminal and what reaches the clipboard. The C
tests also replay every case one byte at a time.

## Layout

```
osc52pty      zsh version
test.zsh      its tests
c/osc52pty.c  C version
c/test.c      its tests
c/Makefile
```

## License

MIT, see [LICENSE](LICENSE).
