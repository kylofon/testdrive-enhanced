# Changelog

## v1.0.3

- Increased framerate for smooth gameplay.
- Cars, signs, poles, roadside pieces and hazards always use their largest, most detailed sprite.
- Far sprites no longer sparkle as they are scaled down.
- Test Drive II's distance and time readout in the top right replaces the stage clock.
- Minor scenery and police adjustments.

### Parameters

- `--sprite-detail max|auto`: sprite sizes, the most detailed everywhere (default) or chosen by distance.
- `TD_ENH_STATS=1`: render and overlay times on stderr every 300 frames.

## v1.0.2

- `--check` no longer opens an error box when `TDEGA.EXE` cannot be loaded. The reason goes to the console
  and the exit status is 1, so scripts and `make check` no longer wait for a click.
- Building: a `Makefile` wraps the CMake build (`make build`, `make check`, `make run`, ...). Contributed by
  @felipe-dos-santos81.
- README: step-by-step "How to play" instructions for Windows at the top.
- The zip now includes the SDL3 and libiconv licences.

## v1.0.1

- Smoother horizon: the valley floor and the foot of the mountains now meet at the same haze level, so the
  bright mist band between them is gone. The mist builds up gradually towards the horizon and thins out
  evenly a few rows up the mountains.

## v0.1.0

- First release: enhanced renderer on the faithful Test Drive engine, higher-resolution road view, sprite
  detail levels.
