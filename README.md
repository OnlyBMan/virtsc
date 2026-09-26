<img src="logo.svg" alt="VirTSC logo" width="300" height="auto">

VirTSC is a virtual **Thunderstorm card (TSC)** device baked into **QEMU** which allows for proper virtualization of The Weather Channel's IntelliStar platform and it's A/V capabilities.

### Features: 
- Native full-framerate A/V viewing inside of the QEMU window
- Audio/video input via FIFO
- Configurable 'thunderstorm' QEMU device, with optional buffer output
- NDI utility
# Start Here!
To set up VirTSC, select the appropriate platform you wish to use:
- **[Debian-based Linux](docs/build/DEBIAN.md)**
- **[MacOS](docs/build/MACOS.md)**
- **[Windows](docs/build/WINDOWS.md)**
- **[WSL](docs/build/WSL.md)**
# Notes
- Despite our best efforts, this has been proven unsuccessful for the Raspberry Pi platform up to model 4B (5 has not been tested yet, good luck).
- VirTSC is pronounced "vert-see"
- AI was used to generate the QEMU fork, drivers, and helper tools, because writing C is hard. 
- If you have contributions to add, please open a PR!
