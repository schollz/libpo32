#!/usr/bin/env bash

build/core/po32_kick_sequence_demo \
  --bpm 170 \
  --num 8 \
  --fill 0.4 \
  --syncopation 0.1 \
  demo_kick_160bpm.wav

play \
  demo_kick_160bpm.wav \
  reverb 15 30 30 \
  compand 0.3,1 6:-70,-60,-20 -5 -90 0.2 \
  overdrive 20 20
