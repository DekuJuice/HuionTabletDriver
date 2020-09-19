# HuionTabletDriver
Linux kernel drivers for the Huion H610PROv2 drawing tablet, made as a learning exercise

Supports basic pen input with tilt and pressure.

## Usage

```
$ make
# insmod huion.ko
```
Make sure to also copy 10-huion.conf to /etc/X11/xorg.conf.d

 
