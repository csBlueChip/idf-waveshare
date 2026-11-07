idf_component_register(
	SRCS          "main.c" "ble_serial.c" "cmd.c" "led.c" "lcd.c" "gfx.c" "text.c" "font.c" "font_mono.c" "font_serif.c" "i2c.c" "imu.c" "sd.c" "img.c" "usbdisk.c" "fm.c"
	              "ui.c" "wifi_ap.c" "telnet.c" "btn.c" "scrtr.c"
	              "nv.c" "rad.c" "bat.c" "sys.c" "dns.c" "web.c" "webpath.c" "uiscr.c"
	              "ser.c" "log.c" "script.c" "scr_load.c" "scr_eval.c" "scr_exec.c"
	INCLUDE_DIRS  "."
	EMBED_FILES   "../assets/usbdrive.png"
	EMBED_TXTFILES "../assets/index.html" "../assets/fm.html" "../assets/syntax.cfg"
	PRIV_REQUIRES bt nvs_flash esp_lcd esp_driver_spi esp_driver_ledc esp_driver_i2c
	              fatfs sdmmc esp_driver_sdmmc
	              esp_wifi esp_netif esp_event lwip vfs
	              esp_driver_gpio esp_driver_touch_sens esp_timer
	              esp_adc app_update esp_psram spi_flash esp_app_format esp_http_server
	              esp_driver_uart esp_driver_usb_serial_jtag
)

# USB disk mode LED: route card sector I/O through usbdisk.c (__wrap_*)
target_link_libraries(${COMPONENT_LIB} INTERFACE
	"-Wl,--wrap=sdmmc_read_sectors"
	"-Wl,--wrap=sdmmc_write_sectors")
