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

## Tests

Our code is in `src/`. Never edit the `mpv/` submodule; changes mpv needs go in `patches/mpv.patch`.
Before committing a change to the filter, run `make` and `tools/regress.sh` with no mpv running. It
must print PASS. If a result changes on purpose (better quality, or an accepted trade-off), say so in
the commit and save the new numbers with `tools/regress.sh --update`.
