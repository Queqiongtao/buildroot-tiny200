# Flashing Tools

## Install driver
- [Linux](linux/README.md)
- [Windows](windows/README.md)

## Bundled tools
Prebuilt tools are tracked in this repository, a fresh clone can flash without
building anything:
- Linux x86-64: `linux/bin/xfel`, `linux/bin/sunxi-fel`, `linux/bin/mksunxi`
- Windows x86: `windows/bin/xfel.exe`, `windows/bin/sunxi-fel.exe`, `windows/bin/mksunxi.exe`
- Origin: `QF1cx00s_Project/tools` (xfel v1.3.2, MIT; sunxi-fel `f1c100s-spiflash`
  branch, GPL-2.0+; mksunxi vendor SDK tool, only used to add the eGON.BT0
  header to a raw firmware bin)

## Helper script
- fel-nor-all.sh/bat: Write the whole SPI-NOR image (`sysimage-nor.img`) via FEL
  mode directly. Works even without U-Boot/DFU on the target.
- fel-uboot.sh/bat: Run U-Boot in RAM by FEL mode.
- fel-linux.sh/bat: Run the whole firmware in RAM by FEL mode.
- dfu-nand-all.sh/bat: Flash SPI-NAND by DFU mode.
- dfu-nor-all.sh/bat: Flash SPI-NOR by DFU mode.
- dfu-nand-all.sh/bat: Flash MMC Device by DFU mode.
### Dangerous script
- flash-mmc-flasher.sh: Create a TF card for flashing SPI NOR/NAND to **/dev/sdb**
- flash-mmc-all.sh: Flash sysimage-sdcard.img to **/dev/sdb**

## How to use
- Install drivers first
- Launch one of dfu script you need, the script will wait for the device.
- Put the device in U-Boot mode.(Press RESET key or use fel-uboot.sh/bat for a device without bootloader)
- Wait for the dfu script finish.

### FEL direct write (SPI-NOR, no bootloader needed)
- Put the device into FEL mode. With a working U-Boot, interrupt autoboot from
  the serial console and run `sf probe; sf erase 0x0 0x80000; reset` (remove
  any SD card first). A device without a valid bootloader enters FEL
  automatically.
- Linux: `./linux/fel-nor-all.sh`, Windows: `windows\fel-nor-all.bat`
- See [linux/README.md](linux/README.md) for manual commands and details.

## Known Issues
- Flashing NAND target may report error message, mostly can ignore.
- Flashing MMC target need partition first, empty mmc device may fail.
