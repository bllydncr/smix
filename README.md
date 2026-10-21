# smix

A minimal PipeWire mixer for the terminal, written in plain C in the
[suckless](https://suckless.org) style. No ncurses, no libpipewire: it talks to
WirePlumber through `wpctl` and draws with ANSI escape sequences. Everything you
would want to change (keys, actions, scripts) lives in `config.h`, which you
edit and recompile.

Developed for Void Linux (musl), but it should build anywhere with a C99
compiler and POSIX libc.

```
 Sinks  Sources  Streams
* Built-in Audio Analog Stereo   ########----------------------  40%
  HDMI Output                    ########################------ 120% M
```

## Features

- Three sections: **Sinks**, **Sources**, **Streams** (per-application volume)
- Volume bar with overdrive support (default full bar = 150%)
- Mute indicator, default device marked with `*`
- Per-channel volume (FL, FR, ...) for nodes that expose channels, opened with `c`
- Auto-refresh, so changes made by other programs show up on their own
- Every key is bound to a shell command in `config.h`
- Commands can use the selected node's ID (`%d`) and get it as an environment
  variable, so your own scripts plug in without touching the C code
- Optional hook that runs after every action (status bar refresh, OSD, ...)
- Roughly 400 lines of code, zero library dependencies

## Requirements

- `wpctl` (from `wireplumber`) and a running PipeWire session
- `pactl` (`pulseaudio-utils`) and a running `pipewire-pulse`, only for the
  per-channel view (the same layer pulsemixer uses)
- A C99 compiler and `make`

On Void Linux:

```sh
sudo xbps-install -S pipewire wireplumber pulseaudio-utils base-devel
```

## Build and install

```sh
make
sudo make install        # installs to /usr/local/bin (PREFIX=... to change)
```

The first `make` copies `config.def.h` to `config.h`. After editing `config.h`
just run `make` again. To start over from the defaults, delete `config.h`.

`config.h` is never overwritten. After updating smix, run
`diff config.def.h config.h` and add any new key bindings (for example `c` for
channels) to your `config.h`. `make` warns when `config.def.h` is newer.

## Usage

```sh
smix
```

| Key              | Action                          |
| ---------------- | ------------------------------- |
| `j` / `k`, arrows| select next / previous node     |
| `l` / `h`        | volume +5% / -5%                |
| `L` / `H`        | volume +1% / -1%                |
| `0`              | set volume to 100%              |
| `c`, `Enter`     | show / hide channels of the node|
| `Space`          | lock / unlock channels together |
| `m`              | toggle mute                     |
| `d`              | set as default (sinks, sources) |
| `Tab`, `1` `2` `3` | switch section                |
| `r`              | refresh now                     |
| `p`              | launch `pavucontrol`            |
| `q`, `Esc`       | quit                            |

On a node row `h`/`l` change the whole node. After pressing `c`, the node's
channels appear underneath it. Select one with `j`/`k` and `h`/`l` then change
only that channel, which is how you adjust balance. Channels start locked
(`L` in the last column): `h`/`l` then move all channels together and keep
the balance. `Space` unlocks them, set `CHLOCK` to 0 in `config.h` to start
unlocked. A node with fewer than two
channels, or without channel info, simply stays closed.

These are only the defaults. Change them in `config.h`.

## Configuration

`config.h` has a few settings and one key table:

```c
static const int barwidth  = 30;    /* bar width in columns            */
static const int maxvol    = 150;   /* volume (%) that fills the bar   */
static const int refreshms = 1000;  /* poll wpctl every N ms           */
static const char *postcmd = "";    /* run after every `run` action    */
```

### Key bindings

Each entry is `{ key, function, argument }`:

```c
{ 'l',     run,      {.s = "wpctl set-volume -l 1.5 %d 5%+"} },
{ 'm',     run,      {.s = "wpctl set-mute %d toggle"} },
{ 'p',     spawn,    {.s = "pavucontrol"} },
{ 'j',     movesel,  {.i = +1} },
{ '2',     settab,   {.i = 1} },
{ 'q',     quit,     {0} },
```

Available functions:

| Function    | Argument    | Effect                                              |
| ----------- | ----------- | --------------------------------------------------- |
| `run`       | `.s` command| run via `sh -c`, wait for it, then refresh; then run `postcmd` |
| `spawn`     | `.s` command| run detached, no waiting (GUI programs, long jobs)  |
| `movesel`   | `.i` offset | move the selection (also steps through opened channels) |
| `volume`    | `.i` percent| change volume; on a channel row only that channel   |
| `togglech`  | none        | show / hide channels of the selected node           |
| `togglelock`| none        | lock channels together / adjust one channel alone   |
| `switchtab` | `.i` offset | next / previous section                             |
| `settab`    | `.i` index  | jump to a section (0 sinks, 1 sources, 2 streams)   |
| `refresh_k` | none        | refresh now                                         |
| `quit`      | none        | exit                                                |

Special key names: `K_UP`, `K_DOWN`, `K_LEFT`, `K_RIGHT`, `K_ESC`, `'\t'`
(Tab), `'\n'` (Enter).

### Commands and scripts

In any command string:

- `%d` is replaced with the ID of the selected node. If nothing is selected, a
  command containing `%d` is skipped.
- The environment contains `SMIX_ID`, `SMIX_NAME` and `SMIX_TAB`
  (`Sinks`, `Sources` or `Streams`).
- stdin, stdout and stderr are redirected to `/dev/null`, so commands never
  scribble over the TUI.

Examples:

```c
/* send a notification about the selected node */
{ 'x', run, {.s = "notify-send \"$SMIX_NAME\" \"id $SMIX_ID\""} },

/* your own script, gets the node id as $1 */
{ 'n', run, {.s = "~/.local/bin/mylauncher.sh %d"} },

/* set the selected sink to exactly 30% */
{ '3', run, {.s = "wpctl set-volume %d 0.30"} },
```

Refresh a status bar after every change:

```c
static const char *postcmd = "pkill -RTMIN+10 dwmblocks";
```

## How it works

- `wpctl status` is parsed once per refresh to list the nodes under the
  `Sinks`, `Sources` and `Streams` headings of the `Audio` section. The
  selection is tracked by node ID, so it survives list changes.
- Channel volumes are read with `pactl list <kind>` and written with
  `pactl set-<kind>-volume <index> <p1>% <p2>% ...`. This goes through
  pipewire-pulse, which also drives hardware (ALSA route) volumes. Pulse
  indexes do not reliably equal wpctl ids, so each pactl entry is matched to
  the node (via `wpctl inspect`) by `object.id`, then `object.serial`, then
  `node.name`, then pulse index, and finally by the device description.
  If nothing matches, the status line shows the node id and serial.
- Holding a key does not queue one command per repeat: all pending keys are
  read at once and identical `volume`/`movesel` presses are merged into a
  single bigger step. The screen updates immediately and the full refresh
  (`wpctl status`, `pactl list`) runs once the keys stop for 150 ms.
- Stream entries in `wpctl status` carry no volume, so smix asks
  `wpctl get-volume <id>` for each stream. With many streams, raise
  `refreshms`.
- Because the parser reads human-oriented output, a drastic change of the
  `wpctl status` layout in a future WirePlumber release could break it. The
  parser is one function (`refresh()` in `smix.c`) and easy to adapt.

## Limitations

- Fixed 3 sections, no card profile switching.
- Long node names are cut to fit the terminal width.
- Needs WirePlumber. Plain PipeWire without a session manager has no `wpctl`.
