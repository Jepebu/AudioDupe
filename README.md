# AudioDupe
Lightweight audio duplicator for Windows 10+  

## Installers
- [x64 Architecture](https://jepebu.com/downloads/AudioDupe_Setup_x64.exe)  
- [x86 Architecture](https://jepebu.com/downloads/AudioDupe_Setup_x86.exe)  
- [ARM64 Architecture](https://jepebu.com/downloads/AudioDupe_Setup_ARM64.exe)  


## Purpose
I made AudioDupe as a simple way to send audio output to one additional output device.  

The original use case was playing audio through a pair of speakers and a headset so that you can share your Discord and/or game audio with friends in the same room.  

It is worth noting that this will only duplicate audio from your Windows default output. If you have an app that is playing to another output device, it won't be duplicated.  

## How It Works

The application operates using a dual-thread architecture to keep the UI responsive:

1. Loopback Capture: It hooks into your **Default Windows Playback Device** using `AUDCLNT_STREAMFLAGS_LOOPBACK`.

2. Audio Pump: A background thread reads the raw audio bytes as they are generated.

3. Target Render: It pushes those exact bytes to a secondary user-selected device, utilizing the Windows Audio Engine to handle any necessary sample rate conversions on the fly.


## Build
Because this project relies on native Windows APIs, you will need a C++ compiler equipped with the Windows SDK.  

### Prerequisites

 - Windows 10 or Windows 11

 - Visual Studio (Community Edition is free) with the "Desktop development with C++" workload installed.

### Compilation Steps

1. Open Visual Studio and create a new **"Windows Desktop Application (C++)"** project.

2. Replace the boilerplate code in the main `.cpp` file with the provided `main.cpp` source code.

3. Set your build configuration to Release and your architecture to x64 in the top toolbar.

4. Go to Build > Build Solution (or press `Ctrl + Shift + B`).

5. Your standalone `.exe` will be generated in the x64/Release folder. It requires no installation to run.

### Dependencies

The project links against the following standard Windows libraries (already included via `#pragma comment` in the source):

 - `ole32.lib` (COM initialization)

 - `user32.lib` (Win32 UI components)

 - `gdi32.lib` (Fonts and graphics)
