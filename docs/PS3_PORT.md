# PS3 port

A build for PS3 consoles running custom firmware (CFW/HEN), made with the
open-source PSL1GHT SDK. It uses the same game code as the Vita build. Only
the platform layer is different (`platform/ps3`):

- **Graphics:** citro3d calls go straight to the RSX. Combiner programs are
  generated and assembled at run time with PSL1GHT's cgcomp assembler.
- **Sound:** libaudio.
- **Controls:** the DualShock 3 pad.
- **Files:** read from the game's folder on the hard disk.

**Status:** compiles and links. Not yet tested on a console or in RPCS3.

## Build

Requirements:

- [ps3toolchain](https://github.com/ps3dev/ps3toolchain) with PSL1GHT
  installed.
- The `PS3DEV` and `PSL1GHT` environment variables set, with
  `$PS3DEV/bin` and `$PS3DEV/ppu/bin` on your `PATH`.

```sh
python tools/bootstrap.py            # fetches citro3d, Kinoko, PSL1GHT (cgcomp) ...
cmake -G Ninja -B build/ps3 -DMKW_PLATFORM=ps3
cmake --build build/ps3              # -> build/ps3/mkw.pkg (and mkw.self)
```

The game files are the same as the Vita's. Build them with
`python tools/vita/build.py --assets-only <disc>`.

## Install

1. Install `build/ps3/mkw.pkg` with Package Manager (title ID `MKWP00001`).
2. Copy the **contents** of `build/vita/data/mkw/` to
   `/dev_hdd0/game/MKWP00001/USRDIR/mkw/` (FTP or USB).
3. Launch **Mario Kart Wii** from the XMB.

The log goes to `/dev_hdd0/game/MKWP00001/USRDIR/mkw/ps3.log`.

## Settings

`mkw/quality.txt`, line `resolution = N`: the internal resolution as a
multiple of 400x240. N ranges from 1 to 4 and defaults to 2; 3 is about 720p.

Splitscreen (see below) adds two lines:

| Setting | Default | |
|---|---|---|
| `splitDivisor` | `2` | The internal resolution is divided by this while 2-4 players race (`1` to `4`) |
| `splitRacers` | `8` | Racers in a splitscreen Grand Prix: the players plus CPUs (`4` to `12`) |

## Multiplayer (splitscreen Grand Prix)

Connect one DualShock 3 per player, then choose **Multiplayer** on the main
menu and pick **2P**, **3P** or **4P**. Player *n* is the *n*-th connected
controller.

1. Player 1 chooses the engine class (50cc, 100cc, 150cc or Mirror).
2. Every player picks a character, vehicle and drift type at the same time,
   each on their own grid with their own controller. Circle backs out of a
   choice; player 1's Circle on the character grid returns to the classes.
3. Once everyone is ready, player 1 picks the cup.

The race uses the Wii's layouts: two players get the top and bottom halves,
three or four players get quadrants. With three players the fourth quadrant
shows the course map with every racer. Each view has its own camera and HUD;
the result boards appear once every player has crossed the line. Any
player's START pauses. Course bloom is off in splitscreen.

## Controls

| PS3 | Game |
|---|---|
| Left stick / D-pad | Steer, menus |
| Cross | Accelerate / confirm (A) |
| Circle | Back (B) |
| Square / Triangle | Y / X |
| L1 or L2 | L (item) |
| R1 or R2 | R (drift) |
| Start / Select | Start / Select |
