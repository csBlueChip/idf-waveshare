#pragma once

#include "esp_err.h"

/*
 * USB disk mode: expose the SD card to a PC as a USB mass-storage drive.
 *
 * Command registered by usbDiskInit():
 *   FILES   reply "Disk Mode Enabled", shut down the active user interface,
 *           show the drive image (SD:/sys/usbdrive.png if present and usable,
 *           else the built-in assets/usbdrive.png), LED off, backlight 30 %, then hand the
 *           card to USB. While a PC reads/writes the card the LED shows
 *           NV disk.read / disk.write colours (off between accesses).
 *
 * The device reboots back to normal mode when the drive is ejected, the cable
 * is unplugged, or no PC connects within USB_DISK_ATTACH_TIMEOUT_MS.
 *
 * While in disk mode the USB port is the drive, so the USB serial/JTAG
 * console (idf.py monitor) is unavailable until the reboot.
 */

#define USB_DISK_ATTACH_TIMEOUT_MS	30000

/* Register FILES. Call after cmdInit(), lcdInit(), sdInit() and uiInit(). */
esp_err_t usbDiskInit(void);

/* Enter disk mode from a separate task (returns immediately). */
esp_err_t usbDiskStart(void);
