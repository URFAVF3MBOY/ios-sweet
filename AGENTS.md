# Working on ios-sweet

- Read `NOTES.md` first: project goal, hardware facts, current blocker, where the
  local artifacts are, and the next concrete step.
- Keep the status section at the top of `NOTES.md` current after each milestone
  and before stopping: commits, CI run IDs, exact build/boot results and what is
  unfinished. Keep observations apart from guesses.
- Commit completed changes. Kernel builds run on GitHub Actions (`gh` for runs
  and logs). A local single-file cross-compile is a cheap check before a CI run.
- Device bring-up uses temporary `fastboot boot` images. Keep the partition
  backups. Decided 2026-10-15: internal UFS is used for iOS and Android may be
  deleted; the iOS root lives in the existing `userdata` partition (no GPT
  changes). Bootloaders, recovery, modem, persist and the backups stay intact;
  every destructive step gets a second look before it runs.
- Device dumps, serial numbers and downloaded Apple files stay out of git.
