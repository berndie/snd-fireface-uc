# Write a linux driver for the RME Fireface UC card

The goal of this project is to write a Linux driver for the RME Fireface UC card. Some reverse engineering was done before here: https://github.com/agfline/RME-Fireface-UC-Drivers.


## Scope

Not all functionalities should be supported for now. Current desiderata:

1. Low latency sound output/input through the available channels.
2. Set and/or fix the sample rate of the FireFace
3. Set the output power (e.g. Hi-Gain)
4. Compatibility with ALSA, Pulseaudio and libportaudio
5. Possiboolility to use with sounddevice in Python3

Nice to haves:

1. Support both in PC and Mac mode, probing which mode and activating the correct communication mode.

