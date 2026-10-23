## What and why

<!-- One focused change. Say what it changes and why. -->

## How it was checked

- [ ] `bash tools/host_tests.sh` passes
- [ ] `bash tools/vita_build.sh build-vita` builds
- [ ] Performance or visual claims: before and after console runs (same scene, switches and length),
      read with `tools/vita_run_report.py`. Emulator runs only show that nothing changed.
- [ ] Console testing still pending (say so if it is)

## Ground rules

- [ ] No game data, extracted files, shader compiler or warm-up bundles in this change
- [ ] Game behaviour unchanged, or explained in the patch description
