# F1C100s/200s Linux Flash tools

## Bundled FEL tools (recommended, no build required)
- `bin/xfel` - xfel v1.3.2 (MIT), https://github.com/xboot/xfel
  Detect/erase/read/write SPI NOR and SPI NAND in FEL mode, reset the device.
- `bin/sunxi-fel` - sunxi-fel from Icenowy/sunxi-tools `f1c100s-spiflash` branch (GPL-2.0+)
  SPI flash read/write in FEL mode (`spiflash-info`, `spiflash-read`, `spiflash-write`).
- `bin/mksunxi` - add the eGON.BT0 boot header to a raw firmware bin (vendor SDK tool,
  only needed when building a bootable image from a bare `.bin` by hand).

The bundled Linux binaries are x86-64 and need the libusb-1.0 runtime
(`libusb-1.0-0`), which is normally already installed (dfu-util depends on it).
All tools are tracked in this repository, so a fresh clone can flash without
building anything.

## Enter FEL mode
- Board with a working U-Boot: interrupt autoboot from the serial console
  (hold a key while resetting, `CONFIG_BOOTDELAY=0` still checks for a pending
  key), then erase the bootloader area so the BootROM falls back to FEL:
  ```
  => sf probe
  => sf erase 0x0 0x80000
  => reset
  ```
  Remove any SD card first. This is safe: the BootROM FEL is in on-chip ROM,
  a board with a broken/blank bootloader always powers up into FEL.
- Board without a valid bootloader: powers up into FEL automatically
  (USB ID `1f3a:efe8`).
- Hardware alternative: short the SPI flash pins while powering up.

## Flash SPI-NOR via FEL
From the repository root:

```shell
./flashutils/linux/fel-nor-all.sh          # optional argument: image path
```

The script waits for the FEL device, writes `output/images/sysimage-nor.img`
(16MB, sunxi-fel erases internally) and resets the device. Manual equivalent
(same usage as `QF1cx00s_Project/build.sh`):

```shell
./flashutils/linux/bin/sunxi-fel -p spiflash-write 0 output/images/sysimage-nor.img
./flashutils/linux/bin/xfel reset
```

Equivalent xfel writer (explicit erase, then write):

```shell
./flashutils/linux/bin/xfel spinor erase 0x0 0x1000000
./flashutils/linux/bin/xfel spinor write 0x0 output/images/sysimage-nor.img
./flashutils/linux/bin/xfel reset
```

## Other FEL usages (QF1cx00s_Project compatible)
SPI NAND, bare program (same as `build.sh flash nand`):

```shell
./flashutils/linux/bin/xfel spinand splwrite 1024 0x40000 firmware.bin
./flashutils/linux/bin/xfel reset
```

Add the eGON.BT0 boot header to a raw firmware bin (in place):

```shell
./flashutils/linux/bin/mksunxi firmware.bin
```

## Install dfu-util from apt repository
```shell
sudo apt install dfu-util
```

## Add udev rules for download gadget
A ready-to-use copy lives in this directory:
```shell
sudo cp 99-allwinnerdl.rules /etc/udev/rules.d/
```
The file grants access to the FEL gadget (`1f3a:efe8`) and the U-Boot DFU
gadget (`1f3a:1010`).

## Build tools from source (optional)
```shell
sudo apt install pkg-config zlib1g-dev libusb-dev libusb-1.0-0-dev
git clone https://github.com/Icenowy/sunxi-tools.git -b f1c100s-spiflash
cd sunxi-tools
make
sudo make install
```
