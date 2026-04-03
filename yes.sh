#!/usr/bin/env bash

build/core/po32_kick_sequence_demo \
  --sr 96000 \
  --kick 0 \
  --snare -6 \
  --hihat -8 \
  --any -12 \
  --bpm 170 \
  --num 16 \
  --fill 0.4 \
  --reverse 0.1 \
  --swap-prob 0.05 \
  --syncopation 0.1 \
#  --note 36 \
  demo_kick_160bpm.wav


play \
  demo_kick_160bpm.wav \
  compand 0.5,1.5 3:-70,-60,-30 -3 -90 0.2 \
  lowpass 5000 \
  overdrive 10 10
