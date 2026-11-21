/*
 * Tap zones - code removed from src/imu.c at 0.7.58 (from 0.7.57; NOT BUILT).
 * Excerpts in source order; "imu.c:N" = line in the 0.7.57 imu.c.
 * Full sources: wavedemo (57).zip. See RESTORE.txt.
 */

/* ======== imu.c:131-139  defines ======== */
/* ---------- Tap zones / capture ---------- */
#define IMU_ZONES_MAX		16	/* IMU TAP XY: x * y at most */
#define IMU_RING		512	/* captured samples (0.57 s at 896 Hz) */
#define IMU_CAP_BACK_MS		300	/* tap spike searched this far back */
#define IMU_CAP_WIN_MS		15	/* gyro peaks within +-this of the spike */
#define IMU_TAP_DIR		SD_MOUNT_POINT "/sys"
#define IMU_TAP_PFX		"imutap_"
#define IMU_TAP_EXT		".cal"
#define IMU_TAP_FILE_MAX	2048	/* a .cal file */

/* ======== imu.c:211-228  statics (s_recOn: IMU REC owns the FIFO) ======== */
/* tap zones (runtime; calibration in NV imu.tap.xy / imu.tap.zone1..16, ROT 0) */
static bool     s_zoneEn[IMU_ZONES_MAX + 1][3];	/* per physical zone: single double triple */
static int      s_tapX = 1, s_tapY = 1;		/* IMU TAP XY (ROT 0) */
static bool     s_tapCalMode;			/* IMU TAP CAL START: taps without zones */
static int      s_tapZoneLast;			/* zone of the previous tap (2TAP / 3TAP rule) */

/* tap capture: chip FIFO -> ring of raw samples (ax ay az gx gy gz) */
/* gx gy gz: the largest-magnitude gyro samples around the spike (classified);
 * px py pz: first swing (mean over imu.tap.win from the tap's start, minus the
 * baseline before it; experiment, sent as sx sy sz) */
typedef struct { bool ok; float gx, gy, gz, a, px, py, pz; } imuTapVal_t;
static int16_t        (*s_ring)[6];		/* PSRAM, IMU_RING samples */
static uint8_t        *s_capRaw;		/* internal RAM, one FIFO read */
static int             s_ringHead, s_ringCnt;
static uint32_t        s_ringTotal;		/* samples ever put in the ring */
static int64_t         s_spikeLast = -1;	/* sample number of the last tap spike used */
static bool            s_capOn;		/* FIFO streaming for the capture */
static volatile bool   s_recOn;		/* IMU REC owns the FIFO */

/* ======== imu.c:256-259  prototypes ======== */
static void        imuTapOne(int64_t now, const imuTapVal_t *v);
static void        imuCapDrain(void);
static imuTapVal_t imuCapPeak(int skip, int skipHalf, int *where);
static void        imuTapUnion(void);

/* ======== imu.c:449-449  imuApply: FIFO back to bypass ======== */
	s_capOn = false;					/* tap capture restarts its FIFO */

/* ======== imu.c:638-676  imuPoll: spike search per chip tap ======== */
	bool     haveAcc = false, haveSt = false;
	uint32_t steps = 0;

	imuTapVal_t tv[2] = { { 0 }, { 0 } };
	int         taps  = 0;

	xSemaphoreTake(s_lock, portMAX_DELAY);
	imuCapDrain();					/* tap capture: FIFO -> ring */
	if ((imuCtrl8() & IMU_CTRL8_ENGINES) && imuRead(IMU_REG_STATUS1, &st, 1) == ESP_OK) {
		haveSt = true;
		if ((st & IMU_ST1_TAP) || s_dbg) {
			imuRead(IMU_REG_TAP_STATUS, &tap, 1);
		}
		if (st & IMU_ST1_TAP) {
			/* the tap's spike from the ring; a chip "double" in one poll is
			 * two taps only if there is a second spike >= 80 ms away */
			int k1 = -1, k2 = -1;
			taps   = 1;
			tv[0]  = imuCapPeak(-1, 0, &k1);
			if ((tap & 0x03) == 2 && tv[0].ok) {
				int half = (int)(imuOdr() * 80 / 1000.0f);
				tv[1] = imuCapPeak(k1, half, &k2);
				if (tv[1].ok) {
					taps = 2;
					if (k2 > k1) {			/* older first */
						imuTapVal_t t = tv[0];
						tv[0] = tv[1];
						tv[1] = t;
					}
				}
			} else if (!tv[0].ok) {
				/* capture on: no new spike = the chip saw a rebound */
				taps = s_capOn ? 0 : (tap & 0x03);
			}
			if (tv[0].ok) {				/* newest spike used */
				int kn = (taps == 2 && k2 >= 0 && k2 < k1) ? k2 : k1;
				s_spikeLast = (int64_t)s_ringTotal - 1 - kn;
			}
		}

/* ======== imu.c:689-697  imuPoll: taps from the ring, imu.tap.min ======== */

	if (haveSt) {
		float tmin = nvGetFloat("imu.tap.min");
		for (int i = 0; i < taps && i < 2; i++) {
			if (tv[i].ok && tv[i].a < tmin) {
				continue;			/* too weak: not a tap */
			}
			imuTapOne(now, &tv[i]);
		}

/* ======== imu.c:757-1088  capture, spike search, classifier, zone tap counting (imuTapOne) ======== */
/* ---------- tap capture and zones ---------- */

/* Capture runs while a tap zone (or CAL mode) is on and the rates match. */
static bool imuCapWanted(void)
{
	return IMU_TAP_ON() && s_arateCode == s_grateCode && !s_recOn;
}

/* Chip FIFO -> ring. Caller holds s_lock. */
static void imuCapDrain(void)
{
	if (s_recOn) {
		s_capOn = false;				/* REC owns the FIFO */
		return;
	}
	if (!imuCapWanted()) {
		if (s_capOn) {
			imuWrite(IMU_REG_FIFO_CTRL, 0);		/* bypass */
			s_capOn = false;
		}
		return;
	}
	if (!s_ring) {
		s_ring   = heap_caps_calloc(IMU_RING, sizeof(*s_ring), MALLOC_CAP_SPIRAM);
		s_capRaw = heap_caps_malloc(IMU_FIFO_SAMPLES * 12, MALLOC_CAP_INTERNAL);
		if (!s_ring || !s_capRaw) {
			free(s_ring);
			free(s_capRaw);
			s_ring   = NULL;
			s_capRaw = NULL;
			return;
		}
	}
	if (!s_capOn) {
		imuWrite(IMU_REG_FIFO_CTRL, 0);
		imuCtrl9(IMU_CMD_RST_FIFO, NULL, IMU_CTRL9_MS);
		imuWrite(IMU_REG_FIFO_CTRL, IMU_FIFO_STREAM_128);
		s_capOn     = true;
		s_ringCnt   = 0;
		s_spikeLast = -1;
		return;
	}
	uint8_t cnt[2] = { 0 };
	int     n      = 0;
	if (imuRead(IMU_REG_FIFO_CNT, cnt, 2) == ESP_OK) {
		n = (2 * (((cnt[1] & 0x03) << 8) | cnt[0])) / 12;	/* words -> samples */
		if (n > IMU_FIFO_SAMPLES) {
			n = IMU_FIFO_SAMPLES;
		}
	}
	if (n > 0 && imuCtrl9(IMU_CMD_REQ_FIFO, NULL, IMU_CTRL9_MS) == ESP_OK) {
		imuWrite(IMU_REG_CTRL1, IMU_CTRL1_INT1_EN);	/* no auto-increment: stay on FIFO_DATA */
		if (imuRead(IMU_REG_FIFO_DATA, s_capRaw, n * 12) != ESP_OK) {
			n = 0;
		}
		imuWrite(IMU_REG_CTRL1, IMU_CTRL1_ADDR_AI | IMU_CTRL1_INT1_EN);
		imuWrite(IMU_REG_FIFO_CTRL, IMU_FIFO_STREAM_128);	/* leave read mode */
	} else {
		n = 0;
	}
	for (int i = 0; i < n; i++) {
		const uint8_t *p = s_capRaw + i * 12;
		for (int k = 0; k < 6; k++) {
			s_ring[s_ringHead][k] = (int16_t)(p[2 * k] | (p[2 * k + 1] << 8));
		}
		s_ringHead = (s_ringHead + 1) % IMU_RING;
		s_ringTotal++;
		if (s_ringCnt < IMU_RING) {
			s_ringCnt++;
		}
	}
}

/* sample k back from the newest (0 = newest) */
static const int16_t *imuRingAt(int k)
{
	return s_ring[(s_ringHead - 1 - k + 2 * IMU_RING) % IMU_RING];
}

static float imuRingA(int k, float as)
{
	const int16_t *s = imuRingAt(k);
	return sqrtf((float)s[0] * s[0] + (float)s[1] * s[1] + (float)s[2] * s[2]) * as;
}

/* The tap: the largest |a| in the last IMU_CAP_BACK_MS (not within skipHalf
 * samples of sample skip, if skip >= 0; its position to *where). Its start
 * (onset) is the oldest sample before it still above a quarter of the spike;
 * gx gy gz = the largest-magnitude gyro sample within +-IMU_CAP_WIN_MS;
 * px py pz = mean gyro over imu.tap.win ms from the onset, minus the mean of
 * the 20 ms before it (the first swing). Only spikes more than 80 ms after
 * s_spikeLast count (a rebound re-finds the last one). A second spike
 * (skip >= 0) must reach imu.tap.min. Caller holds s_lock. */
static imuTapVal_t imuCapPeak(int skip, int skipHalf, int *where)
{
	imuTapVal_t v = { 0 };
	if (!s_capOn || !s_ring || s_ringCnt < 4) {
		return v;
	}
	float odr  = imuOdr();
	int   back = (int)(odr * IMU_CAP_BACK_MS / 1000.0f);
	int   win  = (int)(odr * IMU_CAP_WIN_MS / 1000.0f);
	float as   = s_arange / 32768.0f, gs = s_grange / 32768.0f;
	if (back > s_ringCnt) {
		back = s_ringCnt;
	}
	if (win < 1) {
		win = 1;
	}
	int   best = -1;
	float amax = -1.0f;
	int   gap  = (int)(odr * 80 / 1000.0f);		/* a rebound is not a new tap */
	for (int k = 0; k < back; k++) {
		if (s_spikeLast >= 0 && (int64_t)s_ringTotal - 1 - k <= s_spikeLast + gap) {
			break;				/* at or before the last tap's spike */
		}
		if (skip >= 0 && abs(k - skip) <= skipHalf) {
			continue;
		}
		float a = imuRingA(k, as);
		if (a > amax) {
			amax = a;
			best = k;
		}
	}
	if (best < 0 || (skip >= 0 && amax < nvGetFloat("imu.tap.min"))) {
		return v;
	}
	*where = best;

	/* old way: signed largest sample per axis */
	float p[3] = { 0, 0, 0 };
	for (int k = best - win; k <= best + win; k++) {
		if (k < 0 || k >= s_ringCnt) {
			continue;
		}
		const int16_t *s = imuRingAt(k);
		for (int ax = 0; ax < 3; ax++) {
			float val = s[3 + ax] * gs;
			if (fabsf(val) > fabsf(p[ax])) {
				p[ax] = val;
			}
		}
	}

	/* onset: walk to older samples while still above a quarter of the spike */
	float on    = 1.0f + 0.25f * (amax - 1.0f);
	int   onset = best;
	while (onset + 1 < s_ringCnt && onset - best < win * 2 && imuRingA(onset + 1, as) > on) {
		onset++;
	}
	/* baseline: 20 ms before the onset */
	int   nb = (int)(odr * 20 / 1000.0f), cnt = 0;
	float b[3] = { 0, 0, 0 };
	for (int k = onset + 2; k < onset + 2 + nb && k < s_ringCnt; k++, cnt++) {
		const int16_t *s = imuRingAt(k);
		for (int ax = 0; ax < 3; ax++) {
			b[ax] += s[3 + ax] * gs;
		}
	}
	for (int ax = 0; ax < 3 && cnt; ax++) {
		b[ax] /= cnt;
	}
	/* first swing: mean over imu.tap.win ms from the onset (towards newer) */
	int   nw = (int)(odr * nvGetInt("imu.tap.win") / 1000.0f + 0.5f), used = 0;
	float g[3] = { 0, 0, 0 };
	if (nw < 1) {
		nw = 1;
	}
	for (int k = onset; k > onset - nw && k >= 0; k--, used++) {
		const int16_t *s = imuRingAt(k);
		for (int ax = 0; ax < 3; ax++) {
			g[ax] += s[3 + ax] * gs - b[ax];
		}
	}
	for (int ax = 0; ax < 3 && used; ax++) {
		g[ax] /= used;
	}
	v.ok = true;
	v.gx = p[0];
	v.gy = p[1];
	v.gz = p[2];
	v.a  = amax;
	v.px = g[0];
	v.py = g[1];
	v.pz = g[2];
	return v;
}

/* NV "x:y:z" (gx/a : gy/a : gz/a; "x:y" = z 0) -> floats; false if not set (0:0:0) */
static bool imuZoneCal(int z, float c[3])
{
	char key[24];
	snprintf(key, sizeof(key), "imu.tap.zone%d", z);
	const char *v = nvGet(key);
	c[2] = 0.0f;
	return v && sscanf(v, "%f:%f:%f", &c[0], &c[1], &c[2]) >= 2 && (c[0] != 0.0f || c[1] != 0.0f || c[2] != 0.0f);
}

/* The calibration in NV is for the current XY and every zone has one (1x1:
 * none needed). */
static bool imuTapCalOk(void)
{
	int x = 0, y = 0;
	float c[3];
	if (s_tapX * s_tapY == 1) {
		return true;
	}
	const char *v = nvGet("imu.tap.xy");
	if (!v || sscanf(v, "%d:%d", &x, &y) != 2 || x != s_tapX || y != s_tapY) {
		return false;
	}
	for (int z = 1; z <= x * y; z++) {
		if (!imuZoneCal(z, c)) {
			return false;
		}
	}
	return true;
}

/* Physical zone of a tap: the nearest calibrated centre to (gx/a, gy/a);
 * 0 = not clearly nearer than the second (imu.tap.dead) or no data. */
static int imuTapZone(const imuTapVal_t *v)
{
	if (s_tapX * s_tapY == 1) {
		return 1;
	}
	if (!v->ok || v->a <= 0.0f || !imuTapCalOk()) {
		return 0;
	}
	float f[3] = { v->gx / v->a, v->gy / v->a, v->gz / v->a };
	float d1 = INFINITY, d2 = INFINITY;
	int   z1 = 0;
	for (int z = 1; z <= s_tapX * s_tapY; z++) {
		float c[3];
		if (!imuZoneCal(z, c)) {
			continue;
		}
		float d = sqrtf((f[0] - c[0]) * (f[0] - c[0]) + (f[1] - c[1]) * (f[1] - c[1]) + (f[2] - c[2]) * (f[2] - c[2]));
		if (d < d1) {
			d2 = d1;
			d1 = d;
			z1 = z;
		} else if (d < d2) {
			d2 = d;
		}
	}
	if (z1 && isfinite(d2) && d1 > nvGetFloat("imu.tap.dead") * d2) {
		return 0;					/* between two zones */
	}
	return z1;
}

/* Physical zone (ROT 0, 1-based) -> its number in reading order as the
 * screen is shown now (ROT, plus 180 deg for the ORIENT AUTO flip). */
static int imuTapN(int zone)
{
	if (zone <= 0) {
		return 0;
	}
	int x = s_tapX, y = s_tapY;
	int c = (zone - 1) % x, r = (zone - 1) / x;
	int rot = (lcdGetRotation() + (lcdGetViewFlip() ? 2 : 0)) % 4;
	switch (rot) {
	case 1:  return (x - 1 - c) * y + r + 1;		/* rows = ROT-0 columns, right first */
	case 2:  return (y - 1 - r) * x + (x - 1 - c) + 1;
	case 3:  return c * y + (y - 1 - r) + 1;
	default: return r * x + c + 1;
	}
}

/* s_tapEn = any zone's flags (IMU status, engines). */
static void imuTapUnion(void)
{
	memset(s_tapEn, 0, sizeof(s_tapEn));
	for (int z = 1; z <= s_tapX * s_tapY; z++) {
		for (int i = 0; i < 3; i++) {
			s_tapEn[i] |= s_zoneEn[z][i];
		}
	}
}

/* One tap from the chip. Taps within imu.tap.gap of each other in the same
 * zone form a burst; the n-th tap (1..3) is reported if the zone has that
 * order enabled; after its highest order the count starts again. A tap in
 * another zone starts a new burst. CAL mode: every tap is TAP n:0 zone:0. */
static void imuTapOne(int64_t now, const imuTapVal_t *v)
{
	static const char *names[3] = { "TAP", "2TAP", "3TAP" };
	char vals[112] = "", text[144];
	if (v->ok) {
		snprintf(vals, sizeof(vals), " gx:%.1f gy:%.1f gz:%.1f a:%.2f sx:%.1f sy:%.1f sz:%.1f", v->gx, v->gy,
		         v->gz, v->a, v->px, v->py, v->pz);
	}
	if (s_tapCalMode) {
		s_tapN = 0;
		snprintf(text, sizeof(text), "TAP n:0 zone:0%s", vals);
		imuEvent(v->ok ? text : "TAP");
		return;
	}
	int zone = (s_tapX * s_tapY == 1) ? 1 : imuTapZone(v);
	if (zone == 0) {					/* dead zone / no data */
		s_tapN        = 0;
		s_tapZoneLast = 0;
		if (s_tapEn[0]) {
			snprintf(text, sizeof(text), "TAP n:0 zone:0%s", vals);
			imuEvent(v->ok ? text : "TAP");
		}
		return;
	}
	const bool *en = s_zoneEn[zone];
	int hi = en[2] ? 3 : en[1] ? 2 : en[0] ? 1 : 0;
	if (hi == 0) {
		s_tapN        = 0;
		s_tapZoneLast = zone;
		return;
	}
	if (zone != s_tapZoneLast || now - s_tapLast > nvGetInt("imu.tap.gap") || s_tapN >= hi) {
		s_tapN = 0;
	}
	s_tapN++;
	s_tapLast     = now;
	s_tapZoneLast = zone;
	if (en[s_tapN - 1]) {
		if (v->ok) {
			snprintf(text, sizeof(text), "%s n:%d zone:%d%s", names[s_tapN - 1], imuTapN(zone), zone, vals);
			imuEvent(text);
		} else {
			imuEvent(names[s_tapN - 1]);		/* no capture: as before */
		}
	}
}

/* ======== imu.c:1194-1194  imuSleepRun ======== */
	s_capOn = false;				/* WoM: no tap capture */

/* ======== imu.c:1351-1357  IMU RESET ======== */
		xSemaphoreTake(s_lock, portMAX_DELAY);
		memset(s_tapEn, 0, sizeof(s_tapEn));
		memset(s_zoneEn, 0, sizeof(s_zoneEn));
		s_tapX        = s_tapY = 1;
		s_tapCalMode  = false;
		s_tapZoneLast = 0;
		s_motion   = s_step = s_orAuto = false;

/* ======== imu.c:1561-1927  IMU TAP command: zones, XY, CAL, SAVE, LOAD ======== */
#define IMU_TAP_USAGE	"usage: IMU TAP [n|* [s d t | CAL x:y:z] | NONE | XY [x y] | CAL START|STOP | SAVE [file] | LOAD [file]]"

/* "single:1 double:0 triple:0 zone:2 cal:-0.412:0.128" */
static void imuTapZoneLine(int z, cmdReply_t reply)
{
	char key[24], text[96];
	snprintf(key, sizeof(key), "imu.tap.zone%d", z);
	snprintf(text, sizeof(text), "single:%d double:%d triple:%d zone:%d cal:%s\n", s_zoneEn[z][0],
	         s_zoneEn[z][1], s_zoneEn[z][2], z, nvGet(key));
	reply(text);
}

/* SD:/sys/<name> (name with .cal) -> path */
static void imuTapPath(const char *name, char *path, size_t n)
{
	snprintf(path, n, "%s/%s", IMU_TAP_DIR, name);
}

/* Highest sequence number of any imutap_XxY_NNNN_*.cal (sx/sy > 0: of that
 * layout only, its file name to best). Caller holds sdLock(). */
static int imuTapLatest(int sx, int sy, char *best, size_t n)
{
	int  hi  = 0;
	DIR *dir = opendir(IMU_TAP_DIR);
	if (!dir) {
		return 0;
	}
	struct dirent *e;
	while ((e = readdir(dir)) != NULL) {
		int    x, y, seq;
		size_t l = strlen(e->d_name);
		if (strncasecmp(e->d_name, IMU_TAP_PFX, strlen(IMU_TAP_PFX)) != 0 || l < 5 ||
		    strcasecmp(e->d_name + l - 4, IMU_TAP_EXT) != 0 ||
		    sscanf(e->d_name + strlen(IMU_TAP_PFX), "%dx%d_%d", &x, &y, &seq) != 3) {
			continue;
		}
		if ((sx <= 0 || (x == sx && y == sy)) && seq > hi) {
			hi = seq;
			if (best) {
				snprintf(best, n, "%s", e->d_name);
			}
		}
	}
	closedir(dir);
	return hi;
}

/* IMU TAP SAVE [file]: imu.tap.xy and the zones to SD:/sys/<file>.cal, or
 * imutap_XxY_NNNN_<timestamp>.cal (NNNN = highest + 1). */
static const char *imuTapSave(const char *file, cmdReply_t reply)
{
	static char err[SD_NAME_MAX + 16];
	char        name[SD_NAME_MAX], path[sizeof(IMU_TAP_DIR) + SD_NAME_MAX + 1], stamp[24];
	int         x = 0, y = 0;
	const char *xy = nvGet("imu.tap.xy");
	if (!xy || sscanf(xy, "%d:%d", &x, &y) != 2) {
		return "no calibration data";
	}
	sdLock();
	if (sdEnsureMounted() != ESP_OK) {
		sdUnlock();
		return "no SD card";
	}
	if (mkdir(IMU_TAP_DIR, 0777) != 0 && errno != EEXIST) {
		sdUnlock();
		return "cannot create /sys";
	}
	if (file) {
		const char *e = sdNameMake(file, IMU_TAP_EXT, name, sizeof(name));
		if (e) {
			sdUnlock();
			return e;
		}
	} else {
		sysTimestamp(stamp, sizeof(stamp));
		snprintf(name, sizeof(name), IMU_TAP_PFX "%dx%d_%04d_%s" IMU_TAP_EXT, x, y,
		         imuTapLatest(0, 0, NULL, 0) + 1, stamp);
	}
	imuTapPath(name, path, sizeof(path));
	FILE *f  = fopen(path, "w");
	bool  ok = f && fprintf(f, "# IMU tap calibration (IMU TAP SAVE / LOAD; zones ROT 0)\nimu.tap.xy=%d:%d\n", x, y) > 0;
	for (int z = 1; ok && z <= x * y; z++) {
		char key[24];
		snprintf(key, sizeof(key), "imu.tap.zone%d", z);
		ok = fprintf(f, "%s=%s\n", key, nvGet(key)) > 0;
	}
	if (f && fclose(f) != 0) {
		ok = false;
	}
	sdUnlock();
	if (!ok) {
		return "cannot write file";
	}
	snprintf(err, sizeof(err), "saved %s\n", name);
	reply(err);
	return NULL;
}

/* IMU TAP LOAD [file]: SD:/sys/<file>.cal, or the latest imutap_XxY_*.cal for
 * the current XY. The file's imu.tap.xy (first setting) sets XY; a name of
 * the form imutap_XxY_... must agree with it. quiet: no reply on success. */
static const char *imuTapLoad(const char *file, cmdReply_t reply, bool quiet)
{
	static EXT_RAM_BSS_ATTR char buf[IMU_TAP_FILE_MAX + 1];
	static char msg[SD_NAME_MAX + 96];
	char        name[SD_NAME_MAX], path[sizeof(IMU_TAP_DIR) + SD_NAME_MAX + 1];
	size_t      n = 0;

	sdLock();
	if (sdEnsureMounted() != ESP_OK) {
		sdUnlock();
		return "no SD card";
	}
	if (file) {
		const char *e = sdNameMake(file, IMU_TAP_EXT, name, sizeof(name));
		if (e) {
			sdUnlock();
			return e;
		}
	} else if (imuTapLatest(s_tapX, s_tapY, name, sizeof(name)) == 0) {
		sdUnlock();
		snprintf(msg, sizeof(msg), "no calibration file for %dx%d", s_tapX, s_tapY);
		return msg;
	}
	imuTapPath(name, path, sizeof(path));
	FILE *f = fopen(path, "rb");
	if (f) {
		n = fread(buf, 1, IMU_TAP_FILE_MAX, f);
		fclose(f);
	}
	sdUnlock();
	if (!f) {
		return "no such file";
	}
	buf[n] = '\0';

	static char vals[IMU_ZONES_MAX + 1][40];
	const char *keys[IMU_ZONES_MAX + 1];
	const char *vp[IMU_ZONES_MAX + 1];
	int         x = 0, y = 0, line = 0;
	for (int z = 1; z <= IMU_ZONES_MAX; z++) {
		snprintf(vals[z], sizeof(vals[z]), "0:0:0");
	}
	for (char *l = strtok(buf, "\n"); l; l = strtok(NULL, "\n")) {
		line++;
		l[strcspn(l, "\r")] = '\0';
		while (*l == ' ' || *l == '\t') {
			l++;
		}
		if (*l == '\0' || *l == '#') {
			continue;
		}
		int   z;
		char  v[40];
		if (x == 0) {
			if (sscanf(l, "imu.tap.xy=%d:%d", &x, &y) != 2 || x < 1 || y < 1 || x * y > IMU_ZONES_MAX) {
				snprintf(msg, sizeof(msg), "%s: line %d: imu.tap.xy=x:y must come first", name, line);
				return msg;
			}
		} else if (sscanf(l, "imu.tap.zone%d=%39s", &z, v) == 2 && z >= 1 && z <= x * y) {
			snprintf(vals[z], sizeof(vals[z]), "%s", v);
		} else {
			snprintf(msg, sizeof(msg), "%s: line %d: expected imu.tap.zoneN=x:y:z", name, line);
			return msg;
		}
	}
	if (x == 0) {
		snprintf(msg, sizeof(msg), "%s: no imu.tap.xy", name);
		return msg;
	}
	int fx, fy;
	if (strncasecmp(name, IMU_TAP_PFX, strlen(IMU_TAP_PFX)) == 0 &&
	    sscanf(name + strlen(IMU_TAP_PFX), "%dx%d_", &fx, &fy) == 2 && (fx != x || fy != y)) {
		snprintf(msg, sizeof(msg), "%s: name says %dx%d, file says %dx%d", name, fx, fy, x, y);
		return msg;
	}
	char xy[16];
	snprintf(xy, sizeof(xy), "%d:%d", x, y);
	static char zk[IMU_ZONES_MAX][24];
	keys[0] = "imu.tap.xy";
	vp[0]   = xy;
	for (int z = 1; z <= IMU_ZONES_MAX; z++) {
		snprintf(zk[z - 1], sizeof(zk[0]), "imu.tap.zone%d", z);
		keys[z] = zk[z - 1];
		vp[z]   = (z <= x * y) ? vals[z] : "0:0:0";
	}
	int         bad = -1;
	const char *e   = nvSetMany(IMU_ZONES_MAX + 1, keys, vp, &bad);
	if (e) {
		snprintf(msg, sizeof(msg), "%s: %s", name, e);
		return msg;
	}
	xSemaphoreTake(s_lock, portMAX_DELAY);
	if (x != s_tapX || y != s_tapY) {
		s_tapX = x;
		s_tapY = y;
		memset(s_zoneEn, 0, sizeof(s_zoneEn));	/* another layout: zones off */
		imuTapUnion();
		imuEngines();
	}
	s_tapZoneLast = 0;
	xSemaphoreGive(s_lock);
	if (!quiet) {
		snprintf(msg, sizeof(msg), "loaded %s x:%d y:%d\n", name, x, y);
		reply(msg);
	}
	return NULL;
}

/* IMU TAP n CAL x:y[:z]: zone n's centre (gx/a : gy/a : gz/a). The first one
 * for a new XY clears the other zones. */
static const char *imuTapCalSet(int z, const char *val)
{
	float cx, cy, cz = 0.0f;
	char  extra;
	int   got = sscanf(val, "%f:%f:%f%c", &cx, &cy, &cz, &extra);
	if (got != 2 && got != 3) {
		return "usage: IMU TAP n CAL x:y[:z]";
	}
	char xy[16], v[40], zk[IMU_ZONES_MAX][24];
	const char *keys[IMU_ZONES_MAX + 1], *vp[IMU_ZONES_MAX + 1];
	int nk = 0, x = 0, y = 0;
	snprintf(xy, sizeof(xy), "%d:%d", s_tapX, s_tapY);
	snprintf(v, sizeof(v), "%.3f:%.3f:%.3f", cx, cy, cz);
	const char *cur = nvGet("imu.tap.xy");
	if (!cur || sscanf(cur, "%d:%d", &x, &y) != 2 || x != s_tapX || y != s_tapY) {
		keys[nk] = "imu.tap.xy";			/* calibration for this XY from now */
		vp[nk++] = xy;
		for (int i = 1; i <= IMU_ZONES_MAX; i++) {
			if (i != z) {
				snprintf(zk[i - 1], sizeof(zk[0]), "imu.tap.zone%d", i);
				keys[nk] = zk[i - 1];
				vp[nk++] = "0:0:0";
			}
		}
	}
	snprintf(zk[z - 1], sizeof(zk[0]), "imu.tap.zone%d", z);
	keys[nk] = zk[z - 1];
	vp[nk++] = v;
	return nvSetMany(nk, keys, vp, NULL);
}

static const char *imuCmdTap(int argc, char *argv[], cmdReply_t reply)
{
	int  v;
	char text[96];

	if (argc == 0) {				/* summary, then each zone */
		snprintf(text, sizeof(text), "x:%d y:%d mode:%s cal:%s\n", s_tapX, s_tapY, s_tapCalMode ? "cal" : "run",
		         imuTapCalOk() ? "ok" : "none");
		reply(text);
		for (int z = 1; z <= s_tapX * s_tapY; z++) {
			imuTapZoneLine(z, reply);
		}
		return NULL;
	}
	if (strcasecmp(argv[0], "XY") == 0) {
		int x, y;
		if (argc == 1) {
			snprintf(text, sizeof(text), "x:%d y:%d\n", s_tapX, s_tapY);
			reply(text);
			return NULL;
		}
		if (argc != 3 || !cmdParseInt(argv[1], 1, IMU_ZONES_MAX, &x) || !cmdParseInt(argv[2], 1, IMU_ZONES_MAX, &y) ||
		    x * y > IMU_ZONES_MAX) {
			return "usage: IMU TAP XY [x y] (x * y 1..16)";
		}
		if (x == s_tapX && y == s_tapY) {
			return NULL;
		}
		xSemaphoreTake(s_lock, portMAX_DELAY);
		s_tapX = x;
		s_tapY = y;
		memset(s_zoneEn, 0, sizeof(s_zoneEn));		/* new layout: zones off */
		s_tapCalMode  = false;
		s_tapZoneLast = 0;
		imuTapUnion();
		const char *err = (imuEngines() == ESP_OK) ? NULL : "IMU write failed";
		xSemaphoreGive(s_lock);
		if (err) {
			return err;
		}
		if (x * y > 1 && !imuTapCalOk()) {		/* latest calibration for this XY */
			const char *e = imuTapLoad(NULL, reply, false);
			if (e) {
				snprintf(text, sizeof(text), "%s\n", e);
				reply(text);
			}
		}
		return NULL;
	}
	if (strcasecmp(argv[0], "CAL") == 0) {
		if (argc != 2 || (strcasecmp(argv[1], "START") != 0 && strcasecmp(argv[1], "STOP") != 0)) {
			return "usage: IMU TAP CAL START|STOP";
		}
		s_tapCalMode  = (strcasecmp(argv[1], "START") == 0);
		s_tapN        = 0;
		s_tapZoneLast = 0;
		return NULL;
	}
	if (strcasecmp(argv[0], "SAVE") == 0 && argc <= 2) {
		return imuTapSave(argc == 2 ? argv[1] : NULL, reply);
	}
	if (strcasecmp(argv[0], "LOAD") == 0 && argc <= 2) {
		return imuTapLoad(argc == 2 ? argv[1] : NULL, reply, false);
	}

	bool en[3] = { false, false, false };
	int  z     = 0;					/* 0 = every zone (* / NONE) */
	if (argc == 1 && strcasecmp(argv[0], "NONE") == 0) {
		/* all off */
	} else {
		if (strcmp(argv[0], "*") != 0 && !cmdParseInt(argv[0], 1, s_tapX * s_tapY, &z)) {
			static char e[64];
			snprintf(e, sizeof(e), "zone must be 1..%d or * (IMU TAP XY)", s_tapX * s_tapY);
			return e;
		}
		if (argc == 1) {
			if (z == 0) {
				return IMU_TAP_USAGE;
			}
			imuTapZoneLine(z, reply);
			return NULL;
		}
		if (argc == 3 && strcasecmp(argv[1], "CAL") == 0) {
			if (z == 0) {
				return "usage: IMU TAP n CAL x:y:z (one zone)";
			}
			return imuTapCalSet(z, argv[2]);
		}
		if (argc != 4) {
			return IMU_TAP_USAGE;
		}
		for (int i = 0; i < 3; i++) {
			if (!cmdParseInt(argv[1 + i], 0, 1, &v)) {
				return "usage: IMU TAP n|* s d t (0|1 each)";
			}
			en[i] = v;
		}
	}
	bool any = en[0] || en[1] || en[2];
	if (any && IMU_OTHERS_ON()) {
		return imuTapClash();
	}
	if (any && s_tapX * s_tapY > 1 && !s_tapCalMode) {
		if (s_arateCode != s_grateCode) {
			return "tap zones need imu.arate = imu.grate";
		}
		if (!imuTapCalOk()) {
			return "no calibration data";
		}
	}
	xSemaphoreTake(s_lock, portMAX_DELAY);
	for (int zz = 1; zz <= IMU_ZONES_MAX; zz++) {
		if (z == 0 || zz == z) {
			memcpy(s_zoneEn[zz], en, sizeof(en));
		}
	}
	imuTapUnion();
	s_tapN        = 0;
	s_tapZoneLast = 0;
	const char *err = (imuEngines() == ESP_OK) ? NULL : "IMU write failed";
	xSemaphoreGive(s_lock);
	if (!err && imuOdr() < 200.0f && any) {
		reply("warning: tap works best with imu.arate 224 or more\n");
	}
	return err;

/* ======== imu.c:2368-2372  IMU REC: capture pauses ======== */

	xSemaphoreTake(s_lock, portMAX_DELAY);
	s_recOn = true;					/* tap capture pauses */
	s_capOn = false;
	imuWrite(IMU_REG_FIFO_CTRL, 0);

/* ======== imu.c:2428-2431  IMU REC: capture restarts ======== */
	imuWrite(IMU_REG_FIFO_CTRL, 0);
	imuCtrl9(IMU_CMD_RST_FIFO, NULL, IMU_CTRL9_MS);
	s_recOn = false;				/* capture restarts on its next poll */
	xSemaphoreGive(s_lock);
