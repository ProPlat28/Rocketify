# Rocketify

A mod loader for Rocket League sessions that run **without** Easy Anti-Cheat (offline: freeplay, training, workshop maps, local matches).

- `Rocketify.exe` starts the plain `RocketLeague.exe` and loads `rocketify\rocketify.dll` into it.
- `rocketify.dll` loads every mod (`.rlmod`, or `.dll`) in `rocketify\mods\` and adds a native **MODS** entry to the main menu (with the game's own look, sounds and controller support). It opens the game's own dialog.
- **EAC guard:** if any Easy Anti-Cheat module is in the process, Rocketify does nothing; if one appears later, it switches itself off. It never patches or hides from EAC.

## Layout (inside `Rocket League\Binaries\Win64`)

```
Rocketify.exe
rocketify\rocketify.dll
rocketify\rocketify.log        log
rocketify\mods\                mods (*.rlmod)
```

## Install

### Windows

Click [here](https://github.com/ProPlat28/Rocketify/releases/download/v0.0.2-alpha/Rocketify-Setup.exe) to install on Windows.

### Linux

If you're using Linux, run the following command:

```bash
curl -fsSL https://raw.githubusercontent.com/ProPlat28/Rocketify/refs/heads/main/installer/rocketify-install.sh | bash
```

## Writing mods

Include `<rocketify.hpp>` (or `<rocketify>`, they are the same), say which place you modify, then write what happens there:

```cpp
#include <rocketify.hpp>

rocketify(MainMenu) {
    void onSettings() {
        create::popup(
            "Test",       // title
            "Hi! Test!",  // message
            "YES", "NO",  // buttons
            "classic"     // popup type
        );
    }
};
```

- **MainMenu:** `onPlay`, `onItemShop`, `onRocketPass`, `onGarage`, `onClub`, `onProfile`, `onSettings`, `onQuit`, `onMods`, `onNews`, `onChallenges`, `onFriendsLists`. Your function replaces what the button normally does.
- **Popup types:** `"classic"` and `"warning"`.
- **Buttons:** one to four, separated by commas. Clicking any of them closes the popup.

### 1. Install the `rocketify` command

Windows (PowerShell):

```powershell
irm https://raw.githubusercontent.com/ProPlat28/Rocketify/refs/heads/main/cli/install.ps1 | iex
```

Linux:

```bash
curl -fsSL https://raw.githubusercontent.com/ProPlat28/Rocketify/refs/heads/main/cli/install.sh | bash
```

Then install everything needed to build mods (CMake, Ninja, a compiler):

```bash
rocketify install dependencies
```

### 2. Make a mod

```bash
rocketify new "My Mod"
```

Run `rocketify build` in the mod's folder: it builds the mod and puts it into the game (`--no-install` only builds it). If the game is not found, set its folder once with `rocketify config mods-dir PATH`. `rocketify list` and `rocketify remove NAME` manage the mods in the game, `rocketify help` lists everything.

Plain C mods can use `rocketify.h` directly.
