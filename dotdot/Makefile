SHELL := /bin/bash

# You need to create this file (with /your/ LAN details):
# It's where "wavedemo[ (n)].zip" will be served.
#   HOST=192.168.1.100
#   PORT=80
-include secrets.mk

# remote files are "wavedemo[ (n)].zip" and contain "/wavedemo/{src,...}"
ROOT ?= wavedemo

# local git repo directory is here
DEST ?= ../idf-waveshare

# identify the most recent download folder
SRC   = $(shell ls -d w_*/ | sort | tail -n 1)

.PHONY: get www move drymove removed

# default only by nature of being the first recipe!
default:
	@grep '^\.PHONY' Makefile

move:
	@echo "These files ARE BEING updated:"
	@rsync    -rlc --out-format='%n' "$(SRC)" "$(DEST)/"
	@echo "[EOL]"

drymove:
	@echo "These files WILL BE updated:"
	@rsync -n -rlc --out-format='%n' "$(SRC)" "$(DEST)/"
	@echo "[EOL]"

removed:
	@echo "These files have NOT been auto-removed from $(DEST):"
	@(\
		awk 'NR==FNR{a[$$0];next} !($$0 in a)' \
		<(cd "$(SRC)" && find . -type f -printf '%P\n') \
		<(cd "$(DEST)" && git ls-files -co --exclude-standard | while IFS= read -r f; do [ -e "$$f" ] && echo "$$f"; done) \
	)
	@echo "[EOL]"

get:
	@echo "Yank files from server..."
	@(\
		WDFN="w_`date +%Y%m%d_%H%M%S`" && \
		wget -q -O "$${WDFN}.zip" "http://$(IP):$(PORT)//`\
			wget -q -O - "http://$(IP):$(PORT)/" \
				| grep "$(ROOT)" \
				| sed -E 's/.*>(w[^<]*).*/\1/' \
				| sort -t. -k1,1 -V \
				| tail -n 1 \
				| sed 's/ /%20/g' \
			`" && \
		unzip -q "$${WDFN}.zip" -d . && \
		mv wavedemo/ "$${WDFN}" && \
		echo "cd $${WDFN}" \
	)


www:
	@echo "Start simple web file server..."
	python -c "import http.server as h; h.SimpleHTTPRequestHandler.index_pages=(); h.test(HandlerClass=h.SimpleHTTPRequestHandler, port=8000)"
