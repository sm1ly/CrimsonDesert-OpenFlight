# CDFlight (Open Source Flight Mod)
An open-source, robust flight modification for Crimson Desert. 
Built and tested for **v1.04.02**.

Unlike closed-source alternatives, this mod is fully open, lightweight, and intercepts physics velocity updates securely at the SSE instruction level via a trampoline hook.

## Features
- **Smooth Ramping Physics**: Implements linear time-based ramp-up/down logic to gracefully apply velocity changes, eliminating wall collision bounce. *(Credit to [Bambozu](https://github.com/Bambozu) for the original smoothing math concept from EnhancedFlight!)*
- **Safe Delta Manipulation**: Intercepts the pure positional delta (`movaps xmm0, xmm6`) instead of absolute world coordinates. This completely eliminates collision deaths and physics glitches.
- **Delayed AOB Scanner**: Hook installs safely after the game unpacks in memory, ensuring maximum compatibility with ASI loaders.
- **Branchless Math**: Operates directly on SSE registers (`mulps`, `addps`) without stack/flag clobbering or branching logic, ensuring buttery smooth frame rates.
- **Simultaneous Flight**: X/Z vectors are multiplied based on camera trajectory, while Y vectors nullify gravity dynamically. This allows you to fly forward and upward at the same time seamlessly like a jet!
- **Auto-Config**: Creates a `CDFlight.ini` file automatically on first launch for easy keybinds and speed tuning.
- **True Open Source**: Clean C++ code, no obfuscation, no bloat.

## Default Controls
- **Left Shift**: Forward Boost (multiplies current movement based on camera angle).
- **Num 9**: Ascend (applies upward thrust).
- **Num 8**: Descend (applies downward thrust).

*(⚠️ **Note on Simultaneous Flight**: Windows translates `Shift + Numpad` combinations into navigation keys (e.g. Page Up / Arrow keys) depending on your Num Lock state. If you want to fly forward AND ascend simultaneously, we highly recommend changing your Ascend/Descend keys in `CDFlight.ini` to letter keys like `E` (69) and `Q` (81), or `Space` / `Caps Lock` to avoid keyboard ghosting!)*

## Installation
1. Install a mod loader like [Definitive Mod Manager (DMM)](https://www.nexusmods.com/crimsondesert/mods/14) or Ultimate ASI Loader.
2. Place `CDFlight.asi` into your `Crimson Desert/bin64` folder.
3. Launch the game once. `CDFlight.ini` will be automatically generated.
4. Edit `CDFlight.ini` to adjust speeds and keybinds to your liking.

## Key Codes Reference (Virtual-Key Codes)
You can find standard Windows key codes to use in the INI file here: 
[Microsoft Virtual-Key Codes](https://learn.microsoft.com/en-us/windows/win32/inputdev/virtual-key-codes)
- `E` = 69
- `Q` = 81
- `Space` = 32
- `Left Ctrl` = 162
- `Caps Lock` = 20

## Compilation
To compile from source, open `CDFlight.sln` in Visual Studio 2022 and build for `x64 Release`, or run the provided MSBuild script.

## Technical Details (For Modders)
The mod hooks the physics update loop at the SSE instructions right before the absolute coordinate addition:
```assembly
movaps xmm0, xmm6     ; xmm6 contains pure positional delta per tick
subss xmm9, xmm8
addps xmm0, [r13]     ; [r13] is previous absolute position
movups [r13], xmm0    ; written back to actor's physics context
```
By multiplying `xmm0` early on the X and Z axes, we preserve camera-based trajectory. By dynamically altering the Y vector based on keypresses and disabling its gravity-driven multiplier locally, we simulate jet thrust upward regardless of the base delta.

