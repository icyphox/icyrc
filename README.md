# icyrc
> no bs irc client

![scrot](https://x.icyphox.sh/8K0.png)

Built on top of https://c9x.me/irc/.

## Installing

Requires `ncurses` development files.
Clone this repo and:

```
$ make
# make install
```

Similarly, to uninstall:

```
# make uninstall
```

## Usage

```
usage: irc [-n NICK] [-u USER] [-s SERVER] [-p PORT] [-l LOGFILE ] [-t] [-h]
```

The nick, user and password can be specified using `IRCNICK`,
`USER` and `IRCPASS` environment variables.

### Commands

- `/j #channel` — Join channel
- `/l #channel` — Leave channel
- `/me msg` — ACTION
- `/q user msg` — Send private message
- `/r something` — Send raw command
- `/x` — Quit

### Hotkeys

- <kbd>Ctrl</kbd>+<kbd>n</kbd>/<kbd>p</kbd> to cycle through buffers
- Emacs-like line editing commands: <kbd>Ctrl</kbd>+<kbd>w</kbd>/<kbd>e</kbd>/<kbd>a</kbd> etc.
