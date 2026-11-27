/*
 * launchapp BUNDLE-ID: bring an app to the front through SpringBoard (SBSLaunchApplicationWithIdentifier), for
 * tests run over ssh (uiopen from an ssh shell trips an XPC lookup fault). Needs the entitlement in ent.plist.
 */
#include <stdint.h>
#include <stddef.h>

extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
extern int printf(const char *, ...);

int
main(int argc, char **argv)
{
	if (argc < 2) {
		printf("usage: launchapp BUNDLE-ID\n");
		return 2;
	}
	void *sbs = dlopen("/System/Library/PrivateFrameworks/SpringBoardServices.framework/SpringBoardServices", 2);
	void *cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", 2);
	int (*launch)(void *, unsigned char) = sbs ? dlsym(sbs, "SBSLaunchApplicationWithIdentifier") : NULL;
	void *(*mkstr)(void *, const char *, uint32_t) = cf ? dlsym(cf, "CFStringCreateWithCString") : NULL;

	if (launch == NULL || mkstr == NULL) {
		printf("launchapp: SpringBoardServices not found\n");
		return 1;
	}
	int r = launch(mkstr(NULL, argv[1], 0x08000100), 0);
	printf("launchapp %s -> %d\n", argv[1], r);
	return r != 0;
}
