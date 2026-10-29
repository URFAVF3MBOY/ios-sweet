#pragma once
#include <stdint.h>

/* Boot the SWEETXNU payload found in the ABL ramdisk; returns only on error. */
const char *loader_boot(uintptr_t dtb);

/* Payload cmdline (NUL-terminated, inside the ramdisk), or 0 if no payload. */
const char *loader_cmdline(uintptr_t dtb);
