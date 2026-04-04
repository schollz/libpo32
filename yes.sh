#!/usr/bin/env bash

build/core/po32_kick_sequence_demo \
  --sr 96000 \
  --kick 0 \
  --snare -6 \
  --hihat -8 \
  --any -4 \
  --bpm 160 \
  --num 64 \
  --fill 0.9 \
  --reverse 0.025 \
  --swap-prob 0.05 \
  --four-on-the-floor 0.9 \
  --syncopation 0.2 \
  demo_kick_160bpm.wav


play \
  demo_kick_160bpm.wav \
  compand 0.5,1.5 3:-70,-60,-30 -3 -90 0.2 \
  lowpass 6000 \
  overdrive 10 10
