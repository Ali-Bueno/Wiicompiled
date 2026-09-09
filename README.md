# Mario Kart Wii Accessibility

An accessibility mod for Mario Kart Wii on PC.

It's a fork of [WiiCompiled](https://github.com/patchzyy/Wiicompiled), a native PC port of the
game made by static recompilation, with menu narration and a set of audio driving aids compiled
in. The game itself is untouched: same physics, same items, same CPU, same everything. The mod
only adds sound and speech on top; it never changes how the game plays.

Speech goes through [PRISM](https://github.com/ethindp/prism), so NVDA, JAWS and SAPI all work
without any extra setup. With none of them running the game still runs, it just stays quiet.

> **You need a controller.** The game does not work with the keyboard. Any pad that Windows sees
> as a gamepad is fine (Xbox, PlayStation, generic). Keyboard keys are only used for a couple of
> mod shortcuts, not for driving.

> No Nintendo code or data is in this repository or in the installer. You need your own dump of
> the European (PAL) disc, game id `RMCP01`. The game is compiled on your machine from that disc,
> which is also why installing takes a while.

## What you get

**Menus.** Every screen reads itself: the screen's title and text when you arrive, then whatever
the cursor is on, then its description. Moving the cursor reads the new item. Dialogs read too.
On the character and vehicle screens the weight class and the seven stat bars (speed, weight,
acceleration, handling, drift, off-road, mini-turbo) are spoken, because in the original they are
just pictures.

**Driving.** This is the part that took the longest to get right, and it works differently from
what you might expect.

- The steering guide lives in your own engine sound. The engine leans to the side you need to get
  away from, following the racing line a bit ahead of the kart, so you steer away from the sound
  and back to centre. A left-hand bend puts the engine on your right. Centred engine means you're
  on the line. There is no extra tone for this. A bend is heard as a steady lean, not as something
  to chase.
- Corners are called out ahead of time: "left", "hard right", "hairpin left long", and two corners
  in a row come as "right then left". Before each corner three beeps count down into it, rising in
  pitch, then a beep at the entry, one at the apex and a higher one at the exit.
- Near the edge of the road you hear beeps that get quicker the closer you are, on the side of the
  danger. Off the road they turn into a held tone until you're back on. "Off road", "on road" and
  "wrong way" are spoken as well, and those stay on even if you turn the edge tones off.
- While your hands are empty a short blip points at the nearest item box you can still take. It
  drops in pitch once the box is behind you. It goes quiet as soon as you're holding something.
- When the item roulette stops, the item is named: "red shell", "triple mushrooms", "bullet
  bill" and so on. Using it or losing it isn't announced, the game's own sounds cover that.
- Laps and positions are spoken when they change ("lap 2 of 3", "4th"), and "finished 2nd" at the
  end.

**Volume.** Your own kart can be turned up past the original (up to 200 percent) so the steering
lean stays easy to hear, the rival karts can be turned down so the pack stops covering your
engine, and the item roulette has its own volume. Music and master volume are there too.

**Languages.** The mod speaks English, German, French, Spanish and Italian, following the language
you pick for the game. The phrases are plain text files you can edit (see below).

**Retro Rewind.** The installer can also build a second executable with Retro Rewind's custom
tracks and its online play (Retro-WFC). The accessibility code is compiled into both.

## Installing

1. Download the installer. This link always gives you the latest version:
   https://github.com/Ali-Bueno/Wiicompiled/releases/latest/download/MKWiiAccessibilityInstaller.exe
   It's about 300 MB because it carries the whole compiler toolchain. Older versions are on the
   [releases page](https://github.com/Ali-Bueno/Wiicompiled/releases).
2. Have your disc image ready. Accepted formats: `iso`, `gcm`, `gcz`, `ciso`, `wbfs`, `wia`, `rvz`.
   It has to be the PAL release.
3. Run the installer. It asks for four things: where to install (the default `C:\MKWiiAccess` is
   a good choice, the build does not like long or non-ASCII paths), the disc image, the language
   for the game and the speech, and whether you want Retro Rewind. The installer is a plain
   Windows program, nothing fancy, so it's accessible on its own.
4. Press Install and go do something else. Twenty minutes on a fast machine, well over an hour
   on a slow one. It needs about 21 GB free while it works (27 with Retro Rewind); most of that is
   released when it's done.

When it finishes you get shortcuts on the Desktop and in the Start menu called
"Mario Kart Wii Accessibility" and, if you asked for it, "Mario Kart Wii Accessibility (Retro
Rewind)". Everything the install writes stays inside the install folder, so deleting that folder
removes the whole thing apart from the shortcuts.

If you said no to Retro Rewind and change your mind, run the installer again, point it at the
same folder and press "Add Retro Rewind".

If something fails, the installer says so and the full log is in `installer-log.txt` in the
install folder. Fixing the cause and pressing Install again continues from where it stopped.

## The settings menu

From any game menu (not during a race), click both sticks at once (L3+R3) or press F8 on the
keyboard. The menu is spoken. Up and down move between rows, left and right change a value in
steps of 5, A activates, B closes. Every row reads its name, its value and a short explanation of
what it does.

Rows, in order:

1. master volume
2. music volume
3. my kart volume (0 to 200)
4. rival karts volume
5. item roulette volume
6. invert steering pan
7. edge cues (on/off)
8. hear the edge tone
9. hear a curve beep
10. hear the item box

The last three just play the sound so you know what to listen for.

About "invert steering pan": by default the engine sounds on the side you must steer *away*
from. Some people would rather drive toward the sound, like following a guide. Turning this on
flips it, and that's all it does.

## Changing things by hand

Everything the menu changes is also in `UserData\Config.toml` inside the install folder. The
game re-reads the `[accessibility]` section every couple of seconds while it runs and says
"settings reloaded" when it picks up a change, so you can edit it with the game open.

```toml
[system]
# 1 English, 2 German, 3 French, 4 Spanish, 5 Italian. Changes the game's text and the mod's speech.
language = 4

[accessibility]
invert_steering_pan = false
edge_cues = true
# "cpu" follows the line the CPU karts drive; "item" follows the item route instead.
line_source = "cpu"
kart_volume = 180
rival_kart_volume = 20
item_roulette_volume = 100
```

The language is the one thing the settings menu doesn't offer. The installer sets it, and after
that it's this file.

Careful with the syntax: a broken `Config.toml` makes the game fall back to defaults for the
whole file, including the path to the disc data, and it won't start. The installer keeps a
`Config.toml.bak` next to it for that reason.

**Phrases.** Next to the game executable there's an `accessibility_lang` folder with one file per
language (`en.ini`, `es.ini`, ...). They are `key = text` lines in UTF-8. Change any phrase you
like; a missing key or file falls back to the built-in English.

**Sounds.** The `accessibility_sounds` folder holds the item box blip as `item_box.wav`. If the
file is missing the mod uses a synthesised tone instead, so you can swap in your own.

## What the mod does not do

It doesn't steer, brake or slow you down, and it doesn't touch item odds, CPU difficulty or
timers. Everything it adds is sound and speech. Driving well is still on you.

Split-screen isn't supported by the mod: the item announcement and the driving aids follow
player one only.

## Building from source

You don't need this to play. The installer already compiles the game.

The fork lives on the `accessibility` branch. Almost all of the mod is in
`runtime/src/accessibility/`; the hooks into the rest of the runtime are a handful of one-line
calls. The build follows upstream's: the translator turns the disc's code into C++, and
LLVM-MinGW Clang compiles that together with the runtime. Upstream's own README, which this file
replaces, is at [patchzyy/Wiicompiled](https://github.com/patchzyy/Wiicompiled).

## Credits and license

WiiCompiled is patchzyy's work; without it none of this exists. PRISM is by Ethin Probst.
Retro Rewind and Retro-WFC belong to their own teams.

Like WiiCompiled, this project is licensed under the GPL v3.
