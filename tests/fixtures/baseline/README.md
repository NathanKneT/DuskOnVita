# Frozen baselines

Earlier versions of this port's own source files, kept byte for byte from its development history
before this repository's first commit. Each folder is named after the revision the files come from,
and keeps the files' original paths.

Host tests compare the current code against them, for example to check that a rewritten function
still gives the earlier results, or that an older shader generator fails a newer test as expected.
The fixture scripts that compare whole files pin the SHA-256 of the baseline they read, so a changed
copy is rejected. Do not edit these files; add a new folder if a test needs a newer baseline.

| Folder | Read by |
| --- | --- |
| `bb52f1c/` | `tools/counter_batch_test_fixture.py`, `tools/fragment_texcoord_test_fixture.py` |
| `964d278/` | `tools/sampler_unit_test_fixture.py`, `tools/sampler_unit_backend_fixture.py` |
| `24e9250/` | `tools/vertex_setup_test_fixture.py` |
