# Rocketify

A mod loader for Rocket League sessions that run **without** Easy Anti-Cheat (offline: freeplay, training, workshop maps, local matches).

- `Rocketify.exe` starts the plain `RocketLeague.exe` and loads `rocketify\rocketify.dll` into it.
- `rocketify.dll` loads every DLL in `rocketify\mods\` and adds a native **MODS** entry to the main menu (with the game's own look, sounds and controller support). It opens the game's own dialog.
- **EAC guard:** if any Easy Anti-Cheat module is in the process, Rocketify does nothing; if one appears later, it switches itself off. It never patches or hides from EAC.

## Layout (inside `Rocket League\Binaries\Win64`)

```
Rocketify.exe
rocketify\rocketify.dll
rocketify\rocketify.log        log
rocketify\mods\                mods (*.dll)
```

## Install

### Windows

Click [here](https://github.com/ProPlat28/Rocketify/releases/download/v0.0.1-alpha/Rocketify-Setup.exe) to install on Windows.

### Linux

If you're using Linux, run the following command:

```bash
curl -fsSL https://raw.githubusercontent.com/ProPlat28/Rocketify/refs/heads/main/installer/rocketify-install.sh | bash
```
