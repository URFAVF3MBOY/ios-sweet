#pragma once

/* OP7T Adreno 640 bring-up before xnu; no-op unless "sweetgpu=N" is in cmdline. */
void gpu_stage(const char *cmdline);
