#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/*
 * Stored settings (NVS namespace "wavedemo"). Values are read by the modules
 * at boot; changing them affects the next boot only (imu.* changed through the
 * IMU commands apply at once). Every change is written
 * to flash at once (no commit step).
 *
 *   key          default          rule
 *   wifi.ssid    CS_wave_xx-yy-zz 1..32 chars, no spaces, ':' or '|' (xx-yy-zz:
 *                                 last 3 octets of the Wi-Fi AP MAC, lower case)
 *   wifi.pwd     ChangeMe_nnnn    8..63 chars, no spaces, ':' or '|' (secret;
 *                                 nnnn random 1000..9999, made and stored
 *                                 whenever the default is set)
 *   wifi.ip      192.168.4.1      a.b.c.d, host address valid for wifi.cidr
 *   wifi.cidr    /24              /8../30
 *   wifi.pwr     8.5              2..20 dBm, rounded to 0.25 (Wi-Fi only)
 *   wifi.gw      1                1 = DHCP offers a gateway (phones open the
 *                                 captive portal); 0 = none (engineering)
 *   wifi.maxconn 1                1..4 Wi-Fi clients at a time
 *   ble.name     CS_wave_xx-yy-zz 1..29 chars, no spaces, ':' or '|' (BT MAC)
 *   ble.pwr      9                -24..20 dBm, rounded to a step:
 *                                 -24 -21 .. 15 18 20 (BLE only)
 *   sys.ui       BLE              BLE|TELNET|WEB|UART|USB|MSC (MSC: hold BOOT
 *                                 on its Wait screen for USB disk mode;
 *                                 RESET! is not a boot choice)
 *   uart.baud    115200           9600|19200|38400|57600|115200|230400|460800|921600
 *                                 (UART UI speed; read when the UART UI starts)
 *   uart.tx      43               UART TX GPIO: 2..11, 43, 44 (next boot; not 0 = BOOT)
 *   uart.rx      44               UART RX GPIO, same set; tx != rx, neither in btn.gpio
 *   ser.echo     1                1 = serial UIs echo typed characters
 *   web.name     (empty)          web page title, 0..32 chars, no spaces (empty:
 *                                 the default "wavedemo"; SD:/sys/uiname.txt wins)
 *   web.port     80               1..65535 web console port
 *   web.captive  1                1 = captive portal (DNS answers every name,
 *                                 unknown URLs redirect); 0 = off (plain 404)
 *   telnet.port  23               1..65535 telnet console port
 *   scr.bl     100              backlight 0..100 %
 *   scr.pflip  0                1 = portrait upside down (ROT 0 <-> 2)
 *   scr.lflip  0                1 = landscape upside down (ROT 1 <-> 3)
 *   btn.deb    30               0..200 ms     (global button timings: debounce)
 *   btn.long   600              50..5000 ms   (long press)
 *   btn.dbl    300              50..5000 ms   (double-click window)
 *   btn.rpt    0                0..5000 ms    (repeat)
 *   btn.touch  0.015            0.001..0.5    (touch threshold ratio)
 *   btn.gpio   0 2 4 5 6 7 8 9 10 11  button n -> GPIO (next boot): 1..10 of
 *                               0, 2..11, 43, 44; first 0 (BOOT); no repeats
 *   web.hist   50               10..500       (web page command history entries)
 *   disk.read  0 255 0          r g b 0..255  (LED during USB disk reads)
 *   disk.write 255 0 0          r g b 0..255  (LED during USB disk writes)
 *   imu.arange 4                2|4|8|16 g            (IMU ACC; applied now)
 *   imu.arate  224              28|56|112|224|448|896 Hz
 *   imu.grange 512              16|32|..|2048 dps     (IMU GYR; applied now)
 *   imu.grate  224              28|56|112|224|448|896 Hz
 *   imu.wom    100              1..255 mg     (IMU SET / WOM / SLEEP threshold)
 *   imu.tap.thr 1.3             0.1..7.9 g    (tap peak, one decimal)
 *   imu.tap.gap 500             100..2000 ms  (max gap between taps of a burst)
 *   imu.mot.thr 100             32..2000 mg   (motion slope per sample, 31.25 mg steps)
 *   imu.still.thr 64            32..2000 mg   (quiet slope per sample)
 *   imu.step.thr 200            50..2000 mg   (step peak-to-peak)
 *   imu.step.min 10             1..255        (steps before counting starts)
 *                                             (step.*: unused, IMU STEP not implemented)
 *   imu.orient.hold 500         0..5000 ms    (orientation must hold this long)
 *   imu.orient.flat 30          5..80 deg     (tilt from horizontal still FLAT)
 *   run.files  4                1..16         data files with a position per script
 *                                             (default when the script has no .FILES)
 *   sys.autorun 0               0|1           run system scripts (onBoot ...);
 *                                             not used yet (scripting)
 *   sys.aron   0 255 0          r g b 0..255  (LED flash at start-up, autorun 1)
 *   sys.aroff  255 0 0          r g b 0..255  (same, autorun 0; 0 0 0 = no flash)
 *   sys.bootcnt 0               0..65535      firmware-owned boot counter: read-only
 *                                             (no NV SET / RESET key; exported;
 *                                             IMPORT ignores it with a warning;
 *                                             kept by NV RESET)
 *
 * Commands registered by nvInit():
 *   NV SET key value      check and store; "*" = the key's default (also in
 *                         .nvram files: default.nvram uses it for the
 *                         per-board keys; wifi.pwd gets a new number)
 *   NV SET key r g b      same, for colour keys (disk.*, sys.aron, sys.aroff)
 *   NV SET btn.gpio 0 2 4 ..   same, the GPIO list (several words = one value)
 *   NV GET [key]          show one or all (key=value); secrets (wifi.pwd) are
 *                         shown in clear - NV is a privileged command
 *   NV RESET [key]        back to default (all if no key; sys.bootcnt is kept)
 *   NV EXPORT name        write key=value lines to SD:/sys/name.nvram (secret keys,
 *                         wifi.pwd, as "!key=value": FCAT shows "!key=[redacted]")
 *   NV IMPORT name        set the keys found in SD:/sys/name.nvram; all lines are
 *                         checked first, nothing is written if any is bad;
 *                         read-only keys are skipped ("warning: ... ignored");
 *                         a leading '!' on a line is ignored
 *
 * In commands "\#" in a value stands for '#'.
 *
 * Reset when no UI works:
 *   - RESET! Wait screen (BOOT double press after MSC; see ui.h).
 *   - SD:/sys/reset.nvram at boot (reset.c): backup, defaults, then the file's
 *     settings; renamed reset.done (bad file: reset.bad, nothing changed).
 *   - USB: esptool.py --chip esp32s3 erase_region 0x9000 0x10000 (NVS partition).
 */

/* Load all values (defaults for missing/invalid), register NV. Call after
 * nvs_flash_init() and cmdInit(). */
esp_err_t nvInit(void);

/* Current stored value as text (default if never set). NULL for unknown keys. */
const char *nvGet(const char *key);
int         nvGetInt(const char *key);
float       nvGetFloat(const char *key);

/* Check n key/value pairs together, then store them all, or none.
 * Returns NULL, or an error text; *badIndex (if not NULL) gets the failing
 * pair's index, or -1 for a cross-check error (e.g. wifi.ip vs wifi.cidr). */
const char *nvSetMany(int n, const char *keys[], const char *vals[], int *badIndex);

/* NV EXPORT: write SD:/sys/<name>[.nvram]. NULL or an error. */
const char *nvExport(const char *name);

/* NV RESET (all): every key back to its default, except sys.bootcnt. */
const char *nvResetAll(void);

/* Export to a free timestamp name in SD:/sys (sysTimestampFile(), e.g.
 * "20261113-142530.nvram" or "b002A_001523.nvram"); name gets that file name.
 * NULL or an error ("no SD card", "cannot write file" ...). */
const char *nvBackup(char *name, size_t n);

/* Defaults, then the settings in SD:/sys/<name>.nvram (none = plain defaults).
 * The file is checked against the defaults first: if it is bad (or missing)
 * nothing is changed and the error is returned. */
const char *nvResetImport(const char *name);

/* Replace "\#" with '#' in place (command arguments). */
void nvUnescape(char *s);
