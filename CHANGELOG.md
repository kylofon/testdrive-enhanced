# Changelog

## v1.0.3

- Cars, signs, poles, roadside pieces and hazards are drawn with their most detailed sprite at every
  distance, scaled down from reduced copies that keep small far sprites from sparkling instead of flickering
  between pixels. `--sprite-detail auto` restores the previous behaviour, where the sprite size is chosen by
  distance.
- The rock face beside the road and the hillside under the left road edge have uneven, notched outlines
  instead of straight edges, so they read as rock and earth rather than as flat shapes.
- Smooth motion at speed: the simulation moves the car 12.5 times a second, and the road used to be drawn
  at exactly those positions, so it jumped once per step and stood still in between. The car, the traffic
  and the police car are now carried on between steps, and the road markings, roadside objects and the
  scenery follow them.
- The police car sat up to two road units away from where it should be, drifting against the road and
  snapping back once per unit, most visible during a chase.
- The stage clock is now Test Drive II's distance and time readout: miles left to the end of the stage
  beside the clock, in its style.

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
