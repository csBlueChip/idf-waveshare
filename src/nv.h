#pragma once

#include <stdbool.h>
#include "esp_err.h"

/*
 * Stored settings (NVS namespace "wavedemo"). Values are read by the modules
 * at boot; changing them affects the next boot only (imu.* changed through the
 * IMU commands apply at once). Every change is written
 * to flash at once (no commit step).
 *
 *   key        default          rule
 *   rad.ssid   wavedemo         1..32 chars, no spaces, ':' or '|'
 *   rad.pwd    waveP@55word     8..63 chars, no spaces, ':' or '|'
 *   rad.ble    ESP32-S3-Demo    1..29 chars, no spaces, ':' or '|'
 *   rad.ip     192.168.4.1      a.b.c.d, host address valid for rad.mask
 *   rad.mask   /24              /8../30
 *   rad.pwr    8.5              2..20 dBm, rounded to 0.25
 *   rad.nogw   0                1 = DHCP offers no gateway (router)
 *   rad.maxcli 1                1..4 Wi-Fi clients at a time
 *   ui.boot    BLE              BLE|TELNET|WEB|UART|USB
 *   ui.baud    115200           9600|19200|38400|57600|115200|230400|460800|921600
 *                               (UART UI speed; read when the UART UI starts)
 *   ui.echo    1                1 = serial UIs echo typed characters
 *   scr.bl     100              backlight 0..100 %
 *   scr.pflip  0                1 = portrait upside down (ROT 0 <-> 2)
 *   scr.lflip  0                1 = landscape upside down (ROT 1 <-> 3)
 *   btn.deb    30               0..200 ms     (global button timings: debounce)
 *   btn.long   600              50..5000 ms   (long press)
 *   btn.dbl    300              50..5000 ms   (double-click window)
 *   btn.rpt    0                0..5000 ms    (repeat)
 *   btn.touch  0.015            0.001..0.5    (touch threshold ratio)
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
 *   NV SET key value      check and store
 *   NV SET key r g b      same, for colour keys (disk.*, sys.aron, sys.aroff)
 *   NV GET [key]          show one or all (key=value)
 *   NV RESET [key]        back to default (all if no key; sys.bootcnt is kept)
 *   NV EXPORT name        write key=value lines to SD:/sys/name.nvram
 *   NV IMPORT name        set the keys found in SD:/sys/name.nvram; all lines are
 *                         checked first, nothing is written if any is bad;
 *                         read-only keys are skipped ("warning: ... ignored")
 *
 * In commands "\#" in a value stands for '#'.
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
 * pair's index, or -1 for a cross-check error (e.g. rad.ip vs rad.mask). */
const char *nvSetMany(int n, const char *keys[], const char *vals[], int *badIndex);

/* Replace "\#" with '#' in place (command arguments). */
void nvUnescape(char *s);
