<div align="center">

# GODFATHER REBORN

**The Godfather: The Don's Edition (PS3, 2007), running natively on Windows.**

![Platform](https://img.shields.io/badge/platform-Windows%2010%20%7C%2011-1f1f1f?style=for-the-badge&logo=windows)
![Status](https://img.shields.io/badge/status-playable-8b0000?style=for-the-badge)
![Co-op](https://img.shields.io/badge/co--op-yes-8b0000?style=for-the-badge)
![License](https://img.shields.io/badge/license-Apache%202.0-1f1f1f?style=for-the-badge)

</div>

---

An unofficial native Windows port of **The Godfather: The Don's Edition** (PS3,
European release `BLES00043`). The game's Cell PowerPC and SPU code is
statically recompiled to x86-64 with
[ps3recomp](https://github.com/sp00nznet/ps3recomp), so it runs as a normal
Windows program rather than under an emulator, with a Direct3D 12 renderer.

**This repository contains no game code or data.** You build the game yourself,
on your own PC, from your own disc.

> Godfather Reborn is a fan project. It is not affiliated with, endorsed by or
> sponsored by Electronic Arts or Paramount.

## Features

**On top of the original game**

| | |
|---|---|
| **Co-op** | Play the city together: host from the pause menu, a friend joins by address. Partners, NPCs, traffic, combat, grabs and mission cutscenes are synced |
| **Inventory & stores** | An inventory in the game's own look, and stores inside the city's businesses (tobacconist, bar, doctor, restaurant, ...) |
| **Tailor** | Change clothes at Gabriel's Tailoring and see them live on your Don |
| **Street dialogue** | Walk up to anyone and talk: say hello, ask for the word on the street or lean on them. They answer with the game's own voice lines and subtitles |
| **Ragdolls** | Physics-driven bodies (Jolt Physics) with impact sounds |
| **Living animation** | Foot placement on stairs and kerbs, sitting on furniture, smoking a cigarette |

**PC port**

| | |
|---|---|
| **Frame rate** | Unlocked well past the original 30 FPS, with a frame limiter |
| **Graphics** | Resolution scale, sharper shadows, texture filtering and upscaling, ambient occlusion, bloom, colour grading, sharpening and wet-street reflections when it rains |
| **Keyboard & mouse** | Full keyboard and mouse controls, mouse look on foot, PC button prompts, and keyboard versions of the SIXAXIS motion moves |
| **Controllers** | Xbox (XInput) controllers, with the motion moves on buttons |
| **Comfort** | Real cutscene skip, quit to desktop, 12 language options |

Every mod above can be switched off in the launcher's **Mods** section.

## Easy install

Download **The Godfather Setup.exe** from the
[latest release](https://github.com/whompay/GodfatherReborn/releases/latest),
run it, choose your game (a disc dump folder with `PS3_GAME` in it, or an
`.iso`) and press **Install**.

Setup installs what it needs if it is missing (Python and the Visual Studio
2022 Build Tools with clang, through winget), regenerates the game's code from
your disc, builds it and adds a shortcut. You need about 10 GB of free disk
space and 16 GB of RAM. The first build takes a while, depending on your CPU.

If your `PS3_GAME/USRDIR` only has `EBOOT.BIN` (no decrypted `EBOOT.elf`),
Setup uses [RPCS3](https://rpcs3.net) to decrypt it, so have RPCS3 installed.

To update, press **Update now** in Setup or use the **Update The Godfather**
shortcut. Only what changed is downloaded and rebuilt; your saves and settings
are kept.

## Requirements

- Your own copy of **The Godfather: The Don's Edition** (PS3, Europe,
  `BLES00043`), dumped from your console. Setup refuses other versions.
- Optional, for French, Italian, German or Spanish voices and text: also your
  dump of that country's disc (`BLES00044` FRA, `BLES00045` ITA, `BLES00046` GER,
  `BLES00047` SPA). Only its language files are used, on top of `BLES00043`
  (`godfather-installer --lang-game <dump>`); pick the language in the launcher.
- Windows 10 or 11 (64-bit) and a GPU with Direct3D 12 support.
- About 10 GB of free disk space and 16 GB of RAM.

## Playing

Start **The Godfather** from the shortcut. The launcher window opens first:
pick your display, graphics, controls, language and mods, then press **Play**.

| Key | Effect |
|---|---|
| E | Talk to the person in front of you |
| I | Inventory |
| B | Store (inside a business) |
| U | Sit down / stand up |
| J | Smoke a cigarette |
| G | Next GPS target (in a car; D-pad right on a controller) |
| F10 | Trainer |
| F11 | Fullscreen / window |

### Co-op

Pause the game and open the **Co-op** tab. One player hosts, the other enters
the host's address and joins. Co-op uses UDP port `27960`: play on the same
network, over a VPN, or forward that port on the host's router. Both players
need the same version of the game.

### Settings

The launcher's display, graphics and control options are also in the pause
menu (**Co-op > PC Settings**). Everything is saved to
`godfather-recomp/settings.ini`.

## How it works

ps3recomp translates every PowerPC function in the game's executable (about
40,000) and its SPU programs to C++, and reimplements the PS3's system
libraries, the RSX GPU (on Direct3D 12), audio and input. This repository adds
the Godfather-specific parts:

| Path | Contents |
|---|---|
| `godfather-recomp/gf_*.c` | The port and the mods: co-op (`gf_coop*`), stores (`gf_shop`), dialogue, ragdolls, camera, prompts, ... |
| `godfather-recomp/tools` | Build steps that read assets from your disc (collision, dialogue lines, icons, ...) |
| `ps3recomp` | The recompiler and runtime, with this port's changes |
| `installer` | Setup, the updater and the launcher |
| `installer/patches` | Hand fixes to the recompiled code, applied after it is regenerated from your disc |
| `installer/godfather.toml` | What Setup does, step by step |

## Contributing

Bug reports and fixes are welcome. See [CONTRIBUTING.md](CONTRIBUTING.md).

## Legal

This project distributes only original source code, configuration files and
patches. It does not include, and must not be used to distribute, any part of
The Godfather: no disc images, game files, decrypted executables, recompiled
code or built game executables. Dump your own disc. Requests for or links to
game files will be removed.

The project's own code is released under the [Apache License 2.0](LICENSE). If
you share, re-upload or build on it, you must keep the [NOTICE](NOTICE) file
and credit it as **Godfather Reborn by Whompay**. ps3recomp and the other
libraries are covered by their own licenses; see
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). "The Godfather" is a
trademark of its owner and is used here only to name the game this project
works with.

## Credits

- **Godfather Reborn by Whompay**: the project itself.
- [ps3recomp](https://github.com/sp00nznet/ps3recomp) by Ned Heller / sp00nznet
  and contributors.
- [RPCS3](https://rpcs3.net) and its contributors, for the PS3 knowledge the
  port leans on and the decryption step.
- [Jolt Physics](https://github.com/jrouwe/JoltPhysics) by Jorrit Rouwe.
- EA Redwood Shores, for the game.
- [ainzonix-png](https://github.com/ainzonix-png), for the trainer's rain toggle.
