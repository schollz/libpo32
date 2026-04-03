#!/usr/bin/env bash

build/core/po32_kick_sequence_demo \
  --sr 96000 \
  --kick 0 \
  --snare -6 \
  --swap-prob 0.05 \
  --hihat -12 \
  --any -24 \
  --bpm 170 \
  --num 16 \
  --fill 0.3 \
  --syncopation 0.15 \
  demo_kick_160bpm.wav


play \
  demo_kick_160bpm.wav \
  compand 0.5,1.5 3:-70,-60,-30 -3 -90 0.2 \
  overdrive 20 20
