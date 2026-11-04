SHELL = /bin/bash

ESPDEV ?= $(shell \
	for d in /sys/bus/usb/devices/*; do \
		grep -qx Espressif "$$d/manufacturer" 2>/dev/null && \
		for t in "$$d"/*/tty/*; do \
			[ -e "$$t" ] && echo "/dev/$${t##*/}"; \
		done; \
	done | head -1)

.PHONY : all mon clean flash

flash: all
	@test -n "$(ESPDEV)" || { echo "No Espressif device found"; exit 1; }
	idf.py -p $(ESPDEV) flash monitor

all:
	ip route | grep default
#	ip route del default via 192.168.4.1 dev ens33
#	ip route del default via 10.1.1.1 dev ens33
	idf.py build

mon:
	idf.py -p $(ESPDEV) monitor

clean:
	rm -f sdkconfig
	idf.py fullclean