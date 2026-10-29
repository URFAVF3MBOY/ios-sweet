/*
 * libsweetpb.dylib (kitty, OnePlus 7T): keeps PosterBoard from stopping at start-up, so the lock
 * and home screen get wallpapers.
 *
 * Every PosterBoard start, -[PBFPosterExtensionDataStore _stateLock_updateExtensions:...] runs
 * -_stateLock_setupSwitcherConfigurationForFirstTime once (its "done" flag, ivar at +0xe9, is set
 * only in memory, right after that call), and that method begins with
 *     NSCAssert(!_stateLock_switcherConfiguration, ...)        PBFPosterExtensionDataStore.m:3431
 * (disassembly of 16.5 20F66's PosterBoard.framework, 2026-10-24). Here the configuration has
 * already been built by then -- the getter -_stateLock_switcherConfiguration builds it lazily
 * from what is on disk when anything asks first -- so PosterBoard dies on the assertion
 * (SIGTRAP, "brk #0"), every start, and nothing is ever set up: the black wallpaper.
 *
 * This puts the method back in the state it expects: if the lazily built configuration is there,
 * it is dropped (the ivar set to nil, released), then the original runs and builds it itself, as
 * on a phone where nothing asked earlier. Only in PosterBoard; anywhere else it does nothing.
 *
 * Injected by the kernel (0091 + 0112): sweetinject=...,PosterBoard=/redwood/lib/libsweetpb.dylib
 * and AMFI (0090): sweetdyld=backboardd,PosterBoard.
 */
#include <objc/runtime.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static IMP original;
static unsigned dropped, calls;

static void
sweetpb_setup(id self, SEL cmd)
{
	Ivar iv = class_getInstanceVariable(object_getClass(self), "_stateLock_switcherConfiguration");
	id built = iv != NULL ? object_getIvar(self, iv) : NULL;

	calls++;
	if (built != NULL) {
		dropped++;
		object_setIvarWithStrongDefault(self, iv, NULL);      /* (strong ivar: releases it) */
		fprintf(stderr, "sweetpb: switcher configuration %p was built before the first-time setup: dropped (%u of %u)\n",
		    (void *)built, dropped, calls);
	}
	((void (*)(id, SEL))original)(self, cmd);
}

__attribute__((constructor)) static void
sweetpb_install(void)
{
	const char *me = getprogname();
	Class c;
	Method m;

	if (me == NULL || strcmp(me, "PosterBoard") != 0) {
		return;
	}
	c = objc_getClass("PBFPosterExtensionDataStore");
	m = c != NULL ? class_getInstanceMethod(c, sel_registerName("_stateLock_setupSwitcherConfigurationForFirstTime")) : NULL;
	if (m == NULL) {
		fprintf(stderr, "sweetpb: PBFPosterExtensionDataStore / its first-time setup not found: nothing done\n");
		return;
	}
	original = method_setImplementation(m, (IMP)sweetpb_setup);
	fprintf(stderr, "sweetpb: first-time setup of PosterBoard's switcher configuration is watched\n");
}
