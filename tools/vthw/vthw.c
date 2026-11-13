/*
 * vthw: makes WebKit say H.264 decoding is hardware ("power efficient").
 *
 * Why: on the 7T there is no hardware video decoder (AppleAVD), so VideoToolbox's
 * VTIsHardwareDecodeSupported('avc1') is false and Safari's MediaCapabilities answers
 * powerEfficient=false for every H.264 size. YouTube then only offers its single 360p file.
 * Test: answer true for 'avc1' in Safari's WebKit processes and see whether 720p/1080p appear.
 * Actual decoding is unchanged (mediaserverd's software H.264 decoder).
 *
 * Loaded through the kernel's sweetinject= (DYLD_INSERT_LIBRARIES at exec) for
 * com.apple.WebKit.GPU and com.apple.WebKit.WebContent. WebCore soft-links VideoToolbox
 * (dlsym), so dlsym is interposed too and hands back the replacement for that one name.
 */
#include <stdbool.h>
#include <stdint.h>

extern bool VTIsHardwareDecodeSupported(uint32_t codecType);
extern void *dlsym(void *handle, const char *symbol);
extern void syslog(int priority, const char *message, ...);
extern const char *getprogname(void);

#define FOURCC(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

static int said_hw, said_dlsym;

static bool
my_vt_hw(uint32_t codec)
{
	if (codec == FOURCC('a', 'v', 'c', '1')) {
		if (!said_hw) {
			said_hw = 1;
			syslog(5, "vthw: %s: VTIsHardwareDecodeSupported(avc1) -> true", getprogname());
		}
		return true;
	}
	return VTIsHardwareDecodeSupported(codec);
}

static bool
same(const char *a, const char *b)
{
	while (*a != '\0' && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

static void *
my_dlsym(void *handle, const char *symbol)
{
	if (symbol != 0 && same(symbol, "VTIsHardwareDecodeSupported")) {
		if (!said_dlsym) {
			said_dlsym = 1;
			syslog(5, "vthw: %s: dlsym(VTIsHardwareDecodeSupported) replaced", getprogname());
		}
		return (void *)my_vt_hw;
	}
	return dlsym(handle, symbol);	/* tail call: dlsym still sees the original caller */
}

__attribute__((constructor)) static void
vthw_loaded(void)
{
	syslog(5, "vthw: loaded in %s", getprogname());
}

__attribute__((used, section("__DATA,__interpose"))) static const struct {
	const void *with, *instead_of;
} interposers[] = {
	{ (const void *)my_vt_hw, (const void *)VTIsHardwareDecodeSupported },
	{ (const void *)my_dlsym, (const void *)dlsym },
};
