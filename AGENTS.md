# Notes for agents

## Version

The plain text file `MEMC_VERSION` at the root of this repository holds the version of the memc
filter (`major.minor.patch`, one line). It is separate from mpv's own `MPV_VERSION`, which must not
be changed.

Every commit that changes code must increase it by one step:

- major: breaks something users rely on (option removed or renamed, behaviour changed on purpose)
- minor: new option or feature, or a visible quality change
- patch: bug fix, tuning, or cleanup with no visible change

When raising major or minor, reset the parts after it to 0 (1.4.2 -> 2.0.0, 1.4.2 -> 1.5.0).
Commits that only touch documentation do not change the version.

## Tests

The development copy and tools live in `../mpv-memc` (copy changes here with
`../mpv-memc/mpv/apply.sh .`). Before committing a change to the filter, run
`../mpv-memc/tools/regress.sh` with no mpv running. It must print PASS. If a result changes on purpose
(better quality, or an accepted trade-off), say so in the commit and save the new numbers with
`../mpv-memc/tools/regress.sh --update`.
