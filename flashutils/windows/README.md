# F1C100s/200s Windows Flash tools

## Install Zadig Driver
1. Open zadig driver installer 
2. Choose "Device"->"Create New Device"
>![](res/Create%20New%20Device.png)
3. Install FEL mode driver
>![](res/FEL%20Driver.png)
4. Click "Install Driver"
5. Choose "Device"->"Create New Device"
>![](res/Create%20New%20Device.png)
6. Install DFU mode driver
>![](res/DFU%20Driver.png)
7. Click "Install Driver"

## Bundled tools
- `bin\xfel.exe` - xfel v1.3.2 (MIT), https://github.com/xboot/xfel
- `bin\sunxi-fel.exe` - sunxi-fel from Icenowy/sunxi-tools f1c100s-spiflash branch (GPL-2.0+)
- `bin\mksunxi.exe` - add eGON.BT0 boot header to a raw firmware bin (vendor SDK tool)
- `bin\dfu-util.exe` - DFU download tool for U-Boot DFU mode

## Flash SPI-NOR via FEL
1. Put the device into FEL mode. A board with a working U-Boot can be forced
   into FEL from the U-Boot console (see `../linux/README.md`), a device
   without a valid bootloader powers up into FEL automatically.
2. Run `fel-nor-all.bat`, or invoke the tools manually:

```bat
bin\xfel.exe spinor write 0x0 ..\..\output\images\sysimage-nor.img
bin\xfel.exe reset
```

Alternative writer:

```bat
bin\sunxi-fel.exe -p spiflash-write 0x0 ..\..\output\images\sysimage-nor.img
```
