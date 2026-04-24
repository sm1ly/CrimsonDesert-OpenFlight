# CDFlight (Open Source Flight Mod)
An open-source flight modification for Crimson Desert (tested on v1.04.02).

Unlike closed-source alternatives, this mod is fully open, intercepting physics velocity updates directly at the SSE instruction level (`addps xmm0, [...]`) via a trampoline hook.

## Features
- **True Open Source**: No obfuscation, no hidden code.
- **Configurable**: Change keys and flight speeds via `CDFlight.ini`.
- **Native Injection**: Operates directly on `xmm0` vector registers for smooth movement.

## Installation
1. Install an ASI Loader (like Ultimate ASI Loader or DMM).
2. Place `CDFlight.asi` and `CDFlight.ini` into your `bin64` folder.
3. Edit `CDFlight.ini` to your liking.

## Default Controls
- **Num 9**: Ascend
- **Num 8**: Descend

## Compilation
Open `CDFlight.sln` in Visual Studio 2022 and build for `x64 Release`, or run `msbuild CDFlight.sln /p:Configuration=Release /p:Platform=x64`.

## Technical Details
Hooks into the game's physics step (`movups [r13], xmm0`) at `CrimsonDesert.exe+38524BC`. When a key is pressed, it adds a Z/Y vertical velocity delta before the vector is written back to the actor's physics context.
