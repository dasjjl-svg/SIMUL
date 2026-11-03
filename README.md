# SIMUL

Loopback simulator for interface devices.

Author: Jeongmin Lee

## Overview

SIMUL sends a framed packet from the PC through an interface device
(Com0Com / VSPE virtual serial pair, TCP, or UDP) and expects the same
packet to come back. Every 50 ms a 32-byte random payload is wrapped in
`STX | C0 C1 | DATA | CRC16 | FF FF | ETX`, DLE-escaped, transmitted,
and verified on return. The user picks the test duration (up to 72
hours). The run is a success only if every single echoed frame matches.

## Build

Requires Windows, Visual Studio 2019 or newer, and CMake 3.15+.

```bat
cmake -S . -B build -A x64
cmake --build build --config Release
```
