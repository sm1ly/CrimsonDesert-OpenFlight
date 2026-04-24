# CDFlight (Open Source Flight Mod)
An open-source, robust flight modification for Crimson Desert. 
Built and tested for **v1.04.02**.

Unlike closed-source alternatives, this mod is fully open, lightweight, and intercepts physics velocity updates securely at the SSE instruction level via a trampoline hook.

## Features
- **Safe Delta Manipulation**: Intercepts the pure positional delta (`movaps xmm0, xmm6`) instead of absolute world coordinates. This completely eliminates collision deaths and physics glitches.
- **Delayed AOB Scanner**: Hook installs safely after the game unpacks in memory, ensuring maximum compatibility with ASI loaders.
- **Branchless Math**: Operates directly on SSE registers (`mulps`, `addps`) without stack/flag clobbering or branching logic, ensuring buttery smooth frame rates.
- **Simultaneous Flight**: X/Z vectors are multiplied based on camera trajectory, while Y vectors rely on fixed additive deltas. This allows you to fly forward and upward at the same time seamlessly.
- **True Open Source**: Clean C++ code, no obfuscation, no bloat.

## Controls
- **Left Shift**: Forward Boost (multiplies current movement based on camera angle).
- **Num 9**: Ascend (applies upward thrust).
- **Num 8**: Descend (applies downward thrust).
- *Note: You can press Shift + Num 9 simultaneously to fly forward and upward like a jet!*

## Installation
1. Install a mod loader like [Definitive Mod Manager (DMM)](https://www.nexusmods.com/crimsondesert/mods/14) or Ultimate ASI Loader.
2. Place `CDFlight.asi` and `CDFlight.ini` into your `Crimson Desert/bin64` folder.
3. Edit `CDFlight.ini` to adjust speeds and keybinds to your liking.

## Compilation
To compile from source, open `CDFlight.sln` in Visual Studio 2022 and build for `x64 Release`, or run the provided `cdflight_build.bat` / MSBuild script.

## Technical Details (For Modders)
The mod hooks the physics update loop at the SSE instructions right before the absolute coordinate addition:
```assembly
movaps xmm0, xmm6     ; xmm6 contains pure positional delta per tick
subss xmm9, xmm8
addps xmm0, [r13]     ; [r13] is previous absolute position
movups [r13], xmm0    ; written back to actor's physics context
```
By multiplying `xmm0` early on the X and Z axes, we preserve camera-based trajectory. By adding a constant to `xmm0` on the Y axis, we simulate jet thrust upward regardless of the base delta.

