# Notes for agents

## Version

The plain text file `VERSION` at the root of this repository holds the version of the motionblade
filter (`major.minor.patch`, one line). It is separate from mpv's own `MPV_VERSION`.

Every commit that changes code must increase it by one step:

- major: breaks something users rely on (option removed or renamed, behaviour changed on purpose)
- minor: new option or feature, or a visible quality change
- patch: bug fix, tuning, or cleanup with no visible change

When raising major or minor, reset the parts after it to 0 (1.4.2 -> 2.0.0, 1.4.2 -> 1.5.0).
Commits that only touch documentation do not change the version.

## Code

- `core/` must not include Vulkan, ffmpeg or mpv headers; `tests/check_layers.sh` checks this.
- Every GPU pass has a C version in `core/src`. When a shader changes, change its C version the same
  way; `make test` compares them pass by pass.
- No code comments. Small functions with clear names, `static` for everything not in a header.
- Never edit the `mpv/` submodule; changes mpv needs go in `patches/mpv.patch`.

## Tests

Before committing a change to the filter, run `make test` and `tools/regress.sh` with no mpv running. It
must print PASS. If a result changes on purpose (better quality, or an accepted trade-off), say so in
the commit and save the new numbers with `tools/regress.sh --update`. `tools/regress.sh --quality` skips
the speed checks when the GPU is busy.
