/*
 * bthook.dylib: userland test for Bluetooth on the OnePlus 7T, injected into bluetoothd only (kernel sweetinject=, dyld interposing).
 * Logs every option bluetoothd sets/gets on its kernel control (level SYSPROTO_CONTROL = 2) with its bytes,
 * and answers success where the kernel says ENOPROTOOPT, so we can see what bluetoothd's H4 driver wants
 * before changing sweet_bt.c. Logs go to syslog ("bthook:" in oslog).
 */
#include <stddef.h>
#include <stdint.h>

typedef unsigned int socklen_t;
extern int setsockopt(int, int, int, const void *, socklen_t);
extern int getsockopt(int, int, int, void *, socklen_t *);
extern int *__error(void);
extern void syslog(int, const char *, ...);
extern int snprintf(char *, size_t, const char *, ...);
extern int setenv(const char *, const char *, int);
extern char *getenv(const char *);
extern int ioctl(int, unsigned long, ...);
typedef long ssize_t;
extern ssize_t read(int, void *, size_t);
extern ssize_t write(int, const void *, size_t);
extern ssize_t recv(int, void *, size_t, int);
extern ssize_t send(int, const void *, size_t, int);
extern const void *_dyld_get_image_header(uint32_t);
extern uint32_t _dyld_image_count(void);
extern const char *_dyld_get_image_name(uint32_t);

#define ENOPROTOOPT 42
#define SYSPROTO_CONTROL 2
#define LOG_NOTICE 5


/* quiet unless BTHOOK_LOG is set in bluetoothd's environment: every H4 packet and option otherwise reaches the log */
static int verbose;
#define VLOG(...) do { if (verbose) syslog(__VA_ARGS__); } while (0)

static int bt_fd = -1;
static int bt_fresh;	/* new H4 connection: drop the receive parser's and queue's leftovers before the next read */
static unsigned bt_logged;

static void hex(char *out, size_t cap, const void *p, size_t n)
{
	size_t at = 0;
	out[0] = 0;
	for (size_t i = 0; p != NULL && i < n && i < 32 && at + 4 < cap; i++) {
		at += (size_t)snprintf(out + at, cap - at, "%02x ", ((const uint8_t *)p)[i]);
	}
}

static int my_set(int s, int lvl, int opt, const void *v, socklen_t l)
{
	int r = setsockopt(s, lvl, opt, v, l);
	if (lvl == SYSPROTO_CONTROL && (unsigned)opt == 0x80487414u) {
		bt_fd = s;	/* TIOCSETA on the Bluetooth control: from here on, this fd carries H4 */
		bt_fresh = 1;	/* (Bluetooth off/on reconnects: the old stream may have ended mid-packet) */
	}
	if (lvl == SYSPROTO_CONTROL) {
		int e = r < 0 ? *__error() : 0;
		char h[160];
		hex(h, sizeof h, v, l);
		VLOG(LOG_NOTICE, "bthook: setsockopt fd %d opt %#x len %u -> %d errno %d%s : %s", s, (unsigned)opt, l, r, e,
		    (r < 0 && e == ENOPROTOOPT) ? " (faked 0)" : "", h);
		if (r < 0 && e == ENOPROTOOPT) {
			return 0;
		}
		*__error() = e;
	}
	return r;
}

static int my_get(int s, int lvl, int opt, void *v, socklen_t *l)
{
	socklen_t want = l ? *l : 0;
	int r = getsockopt(s, lvl, opt, v, l);
	if (lvl == SYSPROTO_CONTROL) {
		int e = r < 0 ? *__error() : 0;
		char h[160];
		hex(h, sizeof h, r == 0 ? v : NULL, l ? *l : 0);
		VLOG(LOG_NOTICE, "bthook: getsockopt fd %d opt %#x want %u -> %d errno %d len %u : %s", s, (unsigned)opt, want, r, e,
		    l ? *l : 0, h);
		if (r < 0 && e == ENOPROTOOPT && v != NULL && l != NULL) {
			/* faked answer: zeros of the size asked for (what bluetoothd does with it shows next) */
			for (socklen_t i = 0; i < want; i++) {
				((uint8_t *)v)[i] = 0;
			}
			*l = want;
			VLOG(LOG_NOTICE, "bthook: getsockopt opt %#x faked: %u zero bytes", (unsigned)opt, want);
			return 0;
		}
		*__error() = e;
	}
	return r;
}

/*
 * Control name. bluetoothd's H4/POSIX open (bluetoothd 16.5 0x10006669c) takes "name,speed" from
 * BT_HCI_TRANSPORT=<transport>:<driver>:<device>; with no device it uses com.apple.uart.sk.bluetooth, and for
 * exactly that name it takes the Skywalk path (setsockopt 'T'20 {1024,63,63}, getsockopt 'T'18 = 16-byte
 * nexus UUID, os_channel_create). Any other name takes the termios path (TIOCGETA, cfmakeraw, cfsetspeed,
 * TIOCSETA, then a byte stream on the socket), which 0178's kctl already serves. So: our own name for
 * bluetoothd, and its CTLIOCGINFO lookup rewritten to the name the kernel registered.
 */
#define BT_FAKE_NAME "com.sweet.bt"
#define BT_KERNEL_NAME "com.apple.uart.sk.bluetooth"
#define CTLIOCGINFO 0xc0644e03UL

static int same(const char *a, const char *b)
{
	while (*a != 0 && *a == *b) {
		a++, b++;
	}
	return *a == *b;
}

static int my_ioctl(int fd, unsigned long req, ...)
{
	__builtin_va_list ap;
	void *arg;

	__builtin_va_start(ap, req);
	arg = __builtin_va_arg(ap, void *);
	__builtin_va_end(ap);
	if (req == CTLIOCGINFO && arg != NULL) {
		char *name = (char *)arg + 4;	/* struct ctl_info { u_int32_t ctl_id; char ctl_name[96]; } */

		if (same(name, BT_FAKE_NAME)) {
			const char *k = BT_KERNEL_NAME;
			size_t i = 0;
			int r, e;

			for (; k[i] != 0; i++) {
				name[i] = k[i];
			}
			name[i] = 0;
			r = ioctl(fd, req, arg);
			e = r < 0 ? *__error() : 0;
			VLOG(LOG_NOTICE, "bthook: CTLIOCGINFO %s -> %s: %d errno %d id %u", BT_FAKE_NAME, BT_KERNEL_NAME, r, e,
			    *(uint32_t *)arg);
			*__error() = e;
			return r;
		}
	}
	return ioctl(fd, req, arg);
}

/* H4 traffic on bt_fd: first 32 bytes of every read/write, at most 400 lines per process */
static void tap(const char *dir, int fd, const void *b, ssize_t n)
{
	char h[160];

	if (!verbose || fd != bt_fd || n <= 0 || bt_logged >= 400) {
		return;
	}
	bt_logged++;
	hex(h, sizeof h, b, (size_t)n);
	VLOG(LOG_NOTICE, "bthook: %s %ld: %s", dir, (long)n, h);
}

/*
 * H4 receive side, rebuilt per packet. Reads from the kernel are parsed into whole H4 packets; each complete packet
 * is (maybe) edited and appended to a queue, and bluetoothd's read()/recv() are served from that queue. (The first
 * version edited bytes in place and appended padding at the end of a read: when the read also held the start of the
 * next packet, the padding landed inside it -> "unknown packet type received 8", "OI_HCIIfc_DeviceHasBeenReset",
 * 2026-11-10 03:00:18.)
 * Edits, all for bluetoothd's expectations of Apple's (Broadcom) chips on a Qualcomm WCN3990:
 *   Command Complete 0x1002 (Read Local Supported Commands): octet 18 bit 3 set (0x0C5B supported); without it
 *     bluetoothd quits: "OI_HCICMD_WriteDefaultErroneousDataReporting (status=628)"
 *   Command Complete 0x0C5B: status -> 0
 *   Command Complete for a vendor command (OGF 0x3F) with status 1 (unknown command) and nothing else: status 0,
 *     then the command's first parameter byte, then zeros, VSC_PAD bytes in all. bcm_vsc.c's reply parsers assert
 *     "ByteStream_NumReadBytesAvail >= N" or time out ("BD_VSC_... timedout waiting for controller").
 */
#define VSC_PAD 32
static unsigned vsc_said;
static uint8_t vsc_first[64];	/* first parameter byte of the last vendor command, by OCF & 63 */

static struct {
	uint8_t buf[4096];
	unsigned n, need;	/* bytes collected; total size once the header is complete (0 = unknown yet) */
} pk;

static uint8_t q[65536];
static unsigned qh, qn;	/* queue head, bytes queued */

static void q_put(const uint8_t *b, unsigned n)
{
	for (unsigned i = 0; i < n && qn < sizeof q; i++) {
		q[(qh + qn++) % sizeof q] = b[i];
	}
}

static void pk_done(void)
{
	uint8_t *p = pk.buf;

	if (p[0] == 0x04 && p[1] == 0x0e && pk.n >= 7) {
		uint16_t op = (uint16_t)(p[4] | p[5] << 8);

		if (op == 0x1002 && pk.n > 7 + 18 && !(p[7 + 18] & 0x08)) {
			VLOG(LOG_NOTICE, "bthook: supported commands octet 18 %#x -> %#x (0x0C5B)", p[7 + 18], p[7 + 18] | 0x08);
			p[7 + 18] |= 0x08;
		} else if (op == 0x0c5b && p[6] != 0) {
			VLOG(LOG_NOTICE, "bthook: 0x0C5B status %#x -> 0", p[6]);
			p[6] = 0;
		} else if ((op >> 10) == 0x3f && p[2] == 4 && p[6] == 0x01) {
			uint8_t out[7 + VSC_PAD];

			for (unsigned i = 0; i < sizeof out; i++) {
				out[i] = 0;
			}
			out[0] = 0x04, out[1] = 0x0e, out[2] = 4 + VSC_PAD, out[3] = p[3], out[4] = p[4], out[5] = p[5];
			out[6] = 0;
			out[7] = vsc_first[op & 63];
			if (vsc_said++ < 60) {
				VLOG(LOG_NOTICE, "bthook: VSC %#06x (first byte %#x): status 1 -> 0, + %d bytes", op, out[7], VSC_PAD);
			}
			q_put(out, sizeof out);
			pk.n = pk.need = 0;
			return;
		}
	}
	q_put(p, pk.n);
	pk.n = pk.need = 0;
}

static void rx_feed(const uint8_t *b, ssize_t n)
{
	for (ssize_t i = 0; i < n; i++) {
		if (pk.n == 0 && b[i] != 0x02 && b[i] != 0x03 && b[i] != 0x04) {
			q_put(&b[i], 1);	/* not a packet start: pass it on untouched */
			continue;
		}
		if (pk.n < sizeof pk.buf) {
			pk.buf[pk.n] = b[i];
		}
		pk.n++;
		if (pk.need == 0) {
			uint8_t t = pk.buf[0];

			if (t == 0x04 && pk.n == 3) {
				pk.need = 3 + pk.buf[2];
			} else if (t == 0x03 && pk.n == 4) {
				pk.need = 4 + pk.buf[3];
			} else if (t == 0x02 && pk.n == 5) {
				pk.need = 5 + (unsigned)(pk.buf[3] | pk.buf[4] << 8);
			}
		}
		if (pk.need != 0 && pk.n >= pk.need) {
			if (pk.need > sizeof pk.buf) {	/* too big to hold: should not happen; drop the framing */
				VLOG(LOG_NOTICE, "bthook: packet of %u bytes too big, passed on", pk.need);
				pk.n = pk.need = 0;
				continue;
			}
			pk_done();
		}
	}
}

static ssize_t q_get(void *b, size_t n)
{
	size_t k = 0;

	while (k < n && qn > 0) {
		((uint8_t *)b)[k++] = q[qh];
		qh = (qh + 1) % sizeof q;
		qn--;
	}
	return (ssize_t)k;
}

/* bluetoothd's read on the H4 socket: serve from the queue, reading from the kernel until something is queued */
static ssize_t h4_read(int fd, void *b, size_t n, int f, int use_recv)
{
	uint8_t tmp[2048];

	if (bt_fresh) {
		if (pk.n != 0 || qn != 0) {
			VLOG(LOG_NOTICE, "bthook: new connection: dropped %u queued + %u partial bytes from the old one", qn, pk.n);
		}
		pk.n = pk.need = 0;
		qh = qn = 0;
		bt_fresh = 0;
	}
	while (qn == 0) {
		ssize_t r = use_recv ? recv(fd, tmp, sizeof tmp, f) : read(fd, tmp, sizeof tmp);

		if (r <= 0) {
			return r;	/* error (EAGAIN on a non-blocking socket) or EOF: as the kernel said */
		}
		tap("rx", fd, tmp, r);
		rx_feed(tmp, r);
	}
	return q_get(b, n);
}

static ssize_t my_read(int fd, void *b, size_t n)
{
	if (fd == bt_fd) {
		return h4_read(fd, b, n, 0, 0);
	}
	return read(fd, b, n);
}

static ssize_t my_write(int fd, const void *b, size_t n)
{
	if (fd == bt_fd && n >= 4 && ((const uint8_t *)b)[0] == 0x01 && (((const uint8_t *)b)[2] >> 2) == 0x3f) {
		/* an Apple (Broadcom) vendor command: remember its first parameter byte for a faked reply */
		vsc_first[((const uint8_t *)b)[1] & 63] = n >= 5 ? ((const uint8_t *)b)[4] : 0;
	}
	ssize_t r = write(fd, b, n);
	int e = *__error();
	tap("tx", fd, b, r);
	*__error() = e;
	return r;
}

static ssize_t my_recv(int fd, void *b, size_t n, int f)
{
	if (fd == bt_fd) {
		return h4_read(fd, b, n, f, 1);
	}
	return recv(fd, b, n, f);
}

static ssize_t my_send(int fd, const void *b, size_t n, int f)
{
	if (fd == bt_fd && n >= 4 && ((const uint8_t *)b)[0] == 0x01 && (((const uint8_t *)b)[2] >> 2) == 0x3f) {
		/* an Apple (Broadcom) vendor command: remember its first parameter byte for a faked reply */
		vsc_first[((const uint8_t *)b)[1] & 63] = n >= 5 ? ((const uint8_t *)b)[4] : 0;
	}
	ssize_t r = send(fd, b, n, f);
	int e = *__error();
	tap("tx(send)", fd, b, r);
	*__error() = e;
	return r;
}

/*
 * dyld interposing (the library is inserted at launch by the kernel's sweetinject=): every call to
 * setsockopt/getsockopt from other images goes to my_set/my_get; calls from inside this library reach
 * the real ones. (An ElleKit MSHookFunction inline hook made bluetoothd die with SIGSYS right after
 * connecting, 2026-11-10.)
 */
__attribute__((used, section("__DATA,__interpose"))) static const struct {
	const void *with, *instead_of;
} interposers[] = {
	{ (const void *)my_set, (const void *)setsockopt },
	{ (const void *)my_get, (const void *)getsockopt },
	{ (const void *)my_ioctl, (const void *)ioctl },
	{ (const void *)my_read, (const void *)read },
	{ (const void *)my_write, (const void *)write },
	{ (const void *)my_recv, (const void *)recv },
	{ (const void *)my_send, (const void *)send },
};

/*
 * bluetoothd 16.5 (20F66) power-off crash: its transport table entry for H4 (0x100844218, type 1, name "H4")
 * leaves the slot at +0x60 empty; the stack teardown (0x1000e656c, from "Terminating stack..." 0x100089030 ->
 * 0x1000f3970) calls that slot for every transport but USB without a check -> jump to 0, SIGSEGV whenever
 * Bluetooth is switched off (2026-11-10 02:48:38, 02:55:15). The H4BC/PCIe/H5 entries fill it. Real iPhones never
 * use plain H4. Filled here with a do-nothing function, after checking it really is that table entry.
 */
static int h4_slot60(void)
{
	VLOG(LOG_NOTICE, "bthook: H4 transport +0x60 called (stack teardown): nothing to do");
	return 0;
}

static void fix_h4_teardown(void)
{
	const uint8_t *mh = NULL;	/* bluetoothd itself (vmaddr 0x100000000): found by name, not as image 0 */
	uint64_t *ent;
	const char *name;

	for (uint32_t i = 0, n = _dyld_image_count(); i < n; i++) {
		const char *path = _dyld_get_image_name(i);
		size_t len = 0;

		while (path != NULL && path[len] != 0) {
			len++;
		}
		if (len >= 11 && same(path + len - 11, "/bluetoothd")) {
			mh = _dyld_get_image_header(i);
			break;
		}
	}

	if (mh == NULL) {
		return;
	}
	ent = (uint64_t *)(void *)(mh + 0x844218);
	name = (const char *)(mh + 0x70f3b5);
	if (ent[0] != 1 || ent[1] != (uint64_t)(uintptr_t)name || name[0] != 'H' || name[1] != '4' || name[2] != 0) {
		/* diagnostics: where (if anywhere) does __DATA point at the "H4" string */
		unsigned hits = 0;
		for (const uint64_t *w = (const uint64_t *)(const void *)(mh + 0x834000);
		    w < (const uint64_t *)(const void *)(mh + 0x848000) && hits < 4; w++) {
			if (*w == (uint64_t)(uintptr_t)name) {
				VLOG(LOG_NOTICE, "bthook: pointer to \"%s\" at +%#lx ([-1] %#llx)", name,
				    (unsigned long)((const uint8_t *)w - mh), (unsigned long long)w[-1]);
				hits++;
			}
		}
		VLOG(LOG_NOTICE, "bthook: name bytes %02x %02x %02x, %u pointers found", name[0], name[1], name[2], hits);
		syslog(LOG_NOTICE, "bthook: H4 transport entry not where 20F66 has it (mh %p, [0] %#llx, [1] %#llx, want %p): "
		    "teardown slot left alone", mh, (unsigned long long)ent[0], (unsigned long long)ent[1], name);
		return;
	}
	if (ent[12] == 0) {
		ent[12] = (uint64_t)(uintptr_t)h4_slot60;
		VLOG(LOG_NOTICE, "bthook: H4 transport +0x60 was NULL: filled (power-off crash fix)");
	}
}

__attribute__((constructor)) static void init(void)
{
	verbose = getenv("BTHOOK_LOG") != NULL;
	fix_h4_teardown();
	if (getenv("BT_HCI_TRANSPORT") == NULL) {
		setenv("BT_HCI_TRANSPORT", "H4:POSIX:" BT_FAKE_NAME ",115200", 1);
	}
	syslog(LOG_NOTICE, "bthook: loaded (interpose), BT_HCI_TRANSPORT=%s", getenv("BT_HCI_TRANSPORT"));
}
