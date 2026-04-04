/*
 * po32_kick_sequence_demo
 *
 * Renders a 16-step kick+snare+hat+morph pattern at 160 BPM:
 * kick on 1, 5, 9, 13; snare on 3, 7, 11, 15; hihat on every step;
 * any-voice morph on 2, 3, 7, 9, 11, 15.
 *
 * Hihat morph oscillates between HH A and HH B patches:
 *   4 steps A->B, then 4 steps B->A.
 *
 * Any-voice morph ramps between ANY A and ANY B over 8 steps,
 * then repeats.
 *
 * Build:
 *   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
 *   cmake --build build --target po32_kick_sequence_demo
 *
 * Run:
 *   ./build/po32_kick_sequence_demo [output.wav] [kick_patch.mtdrum] [snare_patch.mtdrum]
 *                                  [hh_a.mtdrum] [hh_b.mtdrum] [any_a.mtdrum] [any_b.mtdrum]
 */

#include "po32.h"
#include "po32_synth.h"

#include <dirent.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DEMO_DEFAULT_SAMPLE_RATE             44100u
#define DEMO_SEQUENCE_STEPS                  16u
#define DEMO_DEFAULT_SEQUENCE_COUNT          8u
#define DEMO_DEFAULT_BPM                     160.0f
#define DEMO_DEFAULT_SYNCOPATION_PROBABILITY 0.10f
#define DEMO_DEFAULT_FILL_PROBABILITY        0.25f
#define DEMO_FILL_EVERY_N_SEQUENCES          4u
#define DEMO_CHANGE_EVERY_N_SEQUENCES        8u
#define DEMO_DEFAULT_FOUR_FLOOR_PROBABILITY  0.0f
#define DEMO_DEFAULT_SWAP_PROBABILITY        0.0f
#define DEMO_DEFAULT_REVERSE_PROBABILITY     0.0f
#define DEMO_OSC_FREQ_MIN_HZ                 20.0f
#define DEMO_OSC_FREQ_MAX_HZ                 20000.0f
#define DEMO_KICK_VELOCITY                   120
#define DEMO_SNARE_VELOCITY                  118
#define DEMO_HH_VELOCITY                     110
#define DEMO_ANY_VELOCITY                    114
#define DEMO_HIT_SECONDS                     0.50f
#define DEMO_HH_SECONDS                      0.25f
#define DEMO_ANY_SECONDS                     0.40f
#define DEMO_PATCH_CATEGORY_DIR   "/Library/Audio/Presets/Sonic Charge/Microtonic Drum Patches/By Category"
#define DEMO_PATCH_CATEGORY_KICK  "Bass Drum Patches"
#define DEMO_PATCH_CATEGORY_SNARE "Snare Drum Patches"
#define DEMO_PATCH_CATEGORY_HIHAT "Hi-hats And Cymbal Patches"
#define DEMO_KICK_PATCH_DIR       DEMO_PATCH_CATEGORY_DIR "/" DEMO_PATCH_CATEGORY_KICK
#define DEMO_SNARE_PATCH_DIR      DEMO_PATCH_CATEGORY_DIR "/" DEMO_PATCH_CATEGORY_SNARE
#define DEMO_HIHAT_PATCH_DIR      DEMO_PATCH_CATEGORY_DIR "/" DEMO_PATCH_CATEGORY_HIHAT
#define DEMO_PATH_MAX             4096u

static int write_wav(const char *path, const float *samples, size_t sample_count,
                     uint32_t sample_rate_hz) {
  FILE *fp = fopen(path, "wb");
  if (fp == NULL)
    return 0;

  {
    uint32_t data_bytes = (uint32_t)(sample_count * 3u);
    uint32_t file_size = 36u + data_bytes;
    uint16_t channels = 1u;
    uint16_t bits = 24u;
    uint32_t byte_rate = sample_rate_hz * channels * (uint32_t)(bits / 8u);
    uint16_t block_align = (uint16_t)(channels * (bits / 8u));
    uint16_t pcm = 1u;
    uint32_t fmt_size = 16u;

    fwrite("RIFF", 1u, 4u, fp);
    fwrite(&file_size, 4u, 1u, fp);
    fwrite("WAVEfmt ", 1u, 8u, fp);
    fwrite(&fmt_size, 4u, 1u, fp);
    fwrite(&pcm, 2u, 1u, fp);
    fwrite(&channels, 2u, 1u, fp);
    fwrite(&sample_rate_hz, 4u, 1u, fp);
    fwrite(&byte_rate, 4u, 1u, fp);
    fwrite(&block_align, 2u, 1u, fp);
    fwrite(&bits, 2u, 1u, fp);
    fwrite("data", 1u, 4u, fp);
    fwrite(&data_bytes, 4u, 1u, fp);
  }

  for (size_t i = 0u; i < sample_count; ++i) {
    float sample = samples[i];
    int32_t pcm_sample;
    uint32_t packed;
    uint8_t bytes[3];
    if (sample > 1.0f)
      sample = 1.0f;
    if (sample < -1.0f)
      sample = -1.0f;
    pcm_sample = (int32_t)(sample * 8388607.0f);
    packed = (uint32_t)pcm_sample & 0x00FFFFFFu;
    bytes[0] = (uint8_t)(packed & 0xFFu);
    bytes[1] = (uint8_t)((packed >> 8u) & 0xFFu);
    bytes[2] = (uint8_t)((packed >> 16u) & 0xFFu);
    fwrite(bytes, 1u, sizeof(bytes), fp);
  }

  fclose(fp);
  return 1;
}

static int is_any_step(uint8_t step16) {
  return step16 == 1u || step16 == 2u || step16 == 6u || step16 == 8u || step16 == 10u ||
         step16 == 14u;
}

static int chance_percent(int percent) {
  return (rand() % 100) < percent;
}

static int chance_probability(float probability) {
  if (probability <= 0.0f)
    return 0;
  if (probability >= 1.0f)
    return 1;
  return ((float)rand() / ((float)RAND_MAX + 1.0f)) < probability;
}

static int parse_float_value(const char *text, float *out_value) {
  char *endptr = NULL;
  float parsed = strtof(text, &endptr);
  if (endptr == text || *endptr != '\0' || !isfinite(parsed))
    return 0;
  *out_value = parsed;
  return 1;
}

static float db_to_gain(float db) {
  return powf(10.0f, db / 20.0f);
}

static void reverse_patch_attack_decay(po32_patch_params_t *params) {
  float tmp = params->OscAtk;
  params->OscAtk = params->OscDcy;
  params->OscDcy = tmp;

  tmp = params->NEnvAtk;
  params->NEnvAtk = params->NEnvDcy;
  params->NEnvDcy = tmp;
}

static float osc_param_to_hz(float param) {
  float clamped = param;
  if (clamped < 0.0f)
    clamped = 0.0f;
  if (clamped > 1.0f)
    clamped = 1.0f;
  return DEMO_OSC_FREQ_MIN_HZ * powf(1000.0f, clamped);
}

static float hz_to_osc_param(float hz) {
  float clamped = hz;
  if (clamped < DEMO_OSC_FREQ_MIN_HZ)
    clamped = DEMO_OSC_FREQ_MIN_HZ;
  if (clamped > DEMO_OSC_FREQ_MAX_HZ)
    clamped = DEMO_OSC_FREQ_MAX_HZ;
  return logf(clamped / DEMO_OSC_FREQ_MIN_HZ) / logf(1000.0f);
}

static float midi_to_hz(float midi_note) {
  return 440.0f * powf(2.0f, (midi_note - 69.0f) / 12.0f);
}

static float hz_to_midi(float hz) {
  return 69.0f + 12.0f * (logf(hz / 440.0f) / logf(2.0f));
}

static int pitch_class_mod12(int note) {
  int pc = note % 12;
  return pc < 0 ? pc + 12 : pc;
}

static int nearest_midi_for_pitch_class(float midi_note, int pitch_class, int min_note,
                                        int max_note) {
  int k_center = (int)lroundf((midi_note - (float)pitch_class) / 12.0f);
  int best = min_note;
  float best_dist = 1e9f;

  for (int dk = -3; dk <= 3; ++dk) {
    int candidate = pitch_class + 12 * (k_center + dk);
    float dist;
    if (candidate < min_note || candidate > max_note)
      continue;
    dist = fabsf(midi_note - (float)candidate);
    if (dist < best_dist) {
      best_dist = dist;
      best = candidate;
    }
  }
  return best;
}

static void quantize_patch_oscfreq_to_root_fifth(po32_patch_params_t *params, int root_midi_note) {
  int root_pc = pitch_class_mod12(root_midi_note);
  int fifth_pc = (root_pc + 7) % 12;
  float original_hz = osc_param_to_hz(params->OscFreq);
  float original_midi = hz_to_midi(original_hz);
  float min_midi = hz_to_midi(DEMO_OSC_FREQ_MIN_HZ);
  float max_midi = hz_to_midi(DEMO_OSC_FREQ_MAX_HZ);
  int min_note = (int)ceilf(min_midi);
  int max_note = (int)floorf(max_midi);
  int root_candidate = nearest_midi_for_pitch_class(original_midi, root_pc, min_note, max_note);
  int fifth_candidate = nearest_midi_for_pitch_class(original_midi, fifth_pc, min_note, max_note);
  float root_dist = fabsf(original_midi - (float)root_candidate);
  float fifth_dist = fabsf(original_midi - (float)fifth_candidate);
  int selected_midi = (root_dist <= fifth_dist) ? root_candidate : fifth_candidate;
  float quantized_hz = midi_to_hz((float)selected_midi);

  params->OscFreq = hz_to_osc_param(quantized_hz);
}

static uint8_t random_fill_length(void) {
  switch (rand() % 3) {
  case 0:
    return 8u;
  case 1:
    return 12u;
  default:
    return 16u;
  }
}

static void add_four_on_the_floor_kicks(uint8_t *kick_steps, uint8_t *silence_steps,
                                         size_t sequence_count, float probability,
                                         size_t *out_added_count) {
  static const uint8_t four_floor_offsets[4] = {0u, 4u, 8u, 12u};
  size_t added_count = 0u;

  if (probability <= 0.0f) {
    *out_added_count = 0u;
    return;
  }

  for (size_t seq = 0u; seq < sequence_count; ++seq) {
    size_t base = (size_t)seq * DEMO_SEQUENCE_STEPS;
    for (size_t i = 0u; i < 4u; ++i) {
      size_t idx = base + four_floor_offsets[i];
      if (kick_steps[idx] != 0u)
        continue;
      if (!chance_probability(probability))
        continue;
      kick_steps[idx] = 1u;
      silence_steps[idx] = 0u;
      ++added_count;
    }
  }

  *out_added_count = added_count;
}

static void copy_sequence_state(size_t src_seq, size_t dst_seq, uint8_t *kick_steps,
                                uint8_t *snare_steps, uint8_t *hihat_steps,
                                uint8_t *silence_steps, uint8_t *fill_lengths) {
  size_t src_base = src_seq * DEMO_SEQUENCE_STEPS;
  size_t dst_base = dst_seq * DEMO_SEQUENCE_STEPS;
  memcpy(kick_steps + dst_base, kick_steps + src_base, DEMO_SEQUENCE_STEPS);
  memcpy(snare_steps + dst_base, snare_steps + src_base, DEMO_SEQUENCE_STEPS);
  memcpy(hihat_steps + dst_base, hihat_steps + src_base, DEMO_SEQUENCE_STEPS);
  memcpy(silence_steps + dst_base, silence_steps + src_base, DEMO_SEQUENCE_STEPS);
  fill_lengths[dst_seq] = fill_lengths[src_seq];
}

static void enforce_eight_bar_grouping(uint8_t *kick_steps, uint8_t *snare_steps,
                                       uint8_t *hihat_steps, uint8_t *silence_steps,
                                       uint8_t *fill_lengths, size_t sequence_count) {
  static const uint8_t always_same_offsets[] = {1u, 2u, 4u, 5u, 6u};
  static const uint8_t variation_offsets[] = {3u, 7u};

  for (size_t group_start = 0u; group_start < sequence_count;
       group_start += DEMO_CHANGE_EVERY_N_SEQUENCES) {
    for (size_t i = 0u; i < sizeof(always_same_offsets) / sizeof(always_same_offsets[0]); ++i) {
      size_t seq = group_start + always_same_offsets[i];
      if (seq >= sequence_count)
        continue;
      copy_sequence_state(group_start, seq, kick_steps, snare_steps, hihat_steps, silence_steps,
                          fill_lengths);
    }

    for (size_t i = 0u; i < sizeof(variation_offsets) / sizeof(variation_offsets[0]); ++i) {
      size_t seq = group_start + variation_offsets[i];
      if (seq >= sequence_count)
        continue;
      if (fill_lengths[seq] == 0u) {
        copy_sequence_state(group_start, seq, kick_steps, snare_steps, hihat_steps, silence_steps,
                            fill_lengths);
      }
    }
  }
}

static void generate_step_patterns(uint8_t *kick_steps, uint8_t *snare_steps, uint8_t *hihat_steps,
                                   uint8_t *fill_lengths, uint8_t *silence_steps,
                                   size_t sequence_count, size_t base_steps, float fill_probability,
                                   float syncopation_probability,
                                   float four_floor_probability, size_t *four_floor_added_count) {
  memset(kick_steps, 0, base_steps);
  memset(snare_steps, 0, base_steps);
  memset(hihat_steps, 0, base_steps);
  memset(silence_steps, 0, base_steps);

  for (size_t seq = 0u; seq < sequence_count; ++seq) {
    size_t base = (size_t)seq * DEMO_SEQUENCE_STEPS;
    uint8_t fill_len = 0u;

    /* Jungle-ish 16-step backbone for this sequence. */
    kick_steps[base + 0u] = 1u;
    kick_steps[base + 7u] = 1u;
    if (chance_percent(60))
      kick_steps[base + 3u] = 1u;
    if (chance_percent(55))
      kick_steps[base + 10u] = 1u;
    if (chance_percent(40))
      kick_steps[base + 14u] = 1u;

    snare_steps[base + 4u] = 1u;
    snare_steps[base + 12u] = 1u;
    if (chance_percent(35))
      snare_steps[base + 2u] = 1u;
    if (chance_percent(25))
      snare_steps[base + 6u] = 1u;
    if (chance_percent(30))
      snare_steps[base + 14u] = 1u;

    for (uint8_t s = 0u; s < DEMO_SEQUENCE_STEPS; ++s) {
      if ((s % 2u) == 0u || chance_percent(35))
        hihat_steps[base + s] = 1u;
    }

    /* Configurable chance this 16-step block ends in a fill on sequence 4, 8, 12, ... */
    if (((seq + 1u) % DEMO_FILL_EVERY_N_SEQUENCES) == 0u && chance_probability(fill_probability)) {
      fill_len = random_fill_length();
      for (uint8_t s = (uint8_t)(DEMO_SEQUENCE_STEPS - fill_len); s < DEMO_SEQUENCE_STEPS; ++s) {
        size_t idx = base + s;
        uint8_t rel = (uint8_t)(s - (DEMO_SEQUENCE_STEPS - fill_len));

        hihat_steps[idx] = 1u;
        if ((rel % 2u) == 0u || chance_percent(45))
          kick_steps[idx] = 1u;
        if ((rel % 2u) == 1u || chance_percent(65))
          snare_steps[idx] = 1u;
        if (chance_percent(25)) {
          kick_steps[idx] = 1u;
          snare_steps[idx] = 1u;
        }
      }

      /* Force a strong handoff to the next sequence start. */
      kick_steps[base + 15u] = 1u;
      snare_steps[base + 15u] = 1u;
      hihat_steps[base + 15u] = 1u;
    }

    fill_lengths[seq] = fill_len;
  }

  /* Post-pass syncopation: force true silent steps with configured probability. */
  for (size_t i = 0u; i < base_steps; ++i) {
    if (!chance_probability(syncopation_probability))
      continue;
    silence_steps[i] = 1u;
    kick_steps[i] = 0u;
    snare_steps[i] = 0u;
    hihat_steps[i] = 0u;
  }

  /* Keep each 16-step phrase minimally populated. */
  for (size_t seq = 0u; seq < sequence_count; ++seq) {
    size_t base = (size_t)seq * DEMO_SEQUENCE_STEPS;
    int has_kick = 0;
    int has_snare = 0;
    int has_hihat = 0;
    for (uint8_t s = 0u; s < DEMO_SEQUENCE_STEPS; ++s) {
      size_t idx = base + s;
      has_kick |= kick_steps[idx] != 0u;
      has_snare |= snare_steps[idx] != 0u;
      has_hihat |= hihat_steps[idx] != 0u;
    }
    if (!has_kick) {
      for (uint8_t s = 0u; s < DEMO_SEQUENCE_STEPS; ++s) {
        size_t idx = base + s;
        if (silence_steps[idx] == 0u) {
          kick_steps[idx] = 1u;
          break;
        }
      }
    }
    if (!has_snare) {
      for (uint8_t s = 0u; s < DEMO_SEQUENCE_STEPS; ++s) {
        size_t idx = base + s;
        if (silence_steps[idx] == 0u) {
          snare_steps[idx] = 1u;
          break;
        }
      }
    }
    if (!has_hihat) {
      for (uint8_t s = 0u; s < DEMO_SEQUENCE_STEPS; ++s) {
        size_t idx = base + s;
        if (silence_steps[idx] == 0u) {
          hihat_steps[idx] = 1u;
          break;
        }
      }
    }
  }

  add_four_on_the_floor_kicks(kick_steps, silence_steps, sequence_count, four_floor_probability,
                              four_floor_added_count);

  enforce_eight_bar_grouping(kick_steps, snare_steps, hihat_steps, silence_steps, fill_lengths,
                             sequence_count);
}

static void make_kick_patch(po32_patch_params_t *params) {
  po32_patch_params_zero(params);
  params->OscWave = 0.0f;
  params->OscFreq = 0.25f;
  params->OscDcy = 0.55f;
  params->ModMode = 0.0f;
  params->ModRate = 0.3f;
  params->ModAmt = 0.3f;
  params->Mix = 0.3f;
  params->Level = 0.836f;
}

static void make_kick_patch_b(po32_patch_params_t *params) {
  po32_patch_params_zero(params);
  params->OscWave = 0.5f;
  params->OscFreq = 0.30f;
  params->OscAtk = 0.0f;
  params->OscDcy = 0.42f;
  params->ModMode = 0.0f;
  params->ModRate = 0.22f;
  params->ModAmt = 0.20f;
  params->NFilFrq = 0.40f;
  params->NEnvDcy = 0.18f;
  params->Mix = 0.42f;
  params->DistAmt = 0.15f;
  params->Level = 0.84f;
}

static void make_snare_patch(po32_patch_params_t *params) {
  po32_patch_params_zero(params);
  params->OscWave = 0.5f;
  params->OscFreq = 0.44f;
  params->OscDcy = 0.18f;
  params->ModMode = 0.0f;
  params->ModRate = 0.20f;
  params->ModAmt = 0.18f;
  params->NFilMod = 0.5f;
  params->NFilFrq = 0.68f;
  params->NFilQ = 0.28f;
  params->NEnvMod = 0.5f;
  params->NEnvAtk = 0.0f;
  params->NEnvDcy = 0.20f;
  params->Mix = 0.78f;
  params->DistAmt = 0.12f;
  params->Level = 0.82f;
}

static void make_snare_patch_b(po32_patch_params_t *params) {
  po32_patch_params_zero(params);
  params->OscWave = 1.0f;
  params->OscFreq = 0.52f;
  params->OscDcy = 0.14f;
  params->ModMode = 0.0f;
  params->ModRate = 0.34f;
  params->ModAmt = 0.16f;
  params->NFilMod = 1.0f;
  params->NFilFrq = 0.76f;
  params->NFilQ = 0.22f;
  params->NEnvMod = 1.0f;
  params->NEnvAtk = 0.0f;
  params->NEnvDcy = 0.14f;
  params->Mix = 0.86f;
  params->DistAmt = 0.18f;
  params->Level = 0.80f;
}

static void make_hh_patch_a(po32_patch_params_t *params) {
  po32_patch_params_zero(params);
  params->OscWave = 1.0f;
  params->OscFreq = 0.78f;
  params->OscAtk = 0.0f;
  params->OscDcy = 0.08f;
  params->ModMode = 0.5f;
  params->ModRate = 0.60f;
  params->ModAmt = 0.12f;
  params->NFilMod = 1.0f;
  params->NFilFrq = 0.88f;
  params->NFilQ = 0.20f;
  params->NEnvMod = 1.0f;
  params->NEnvAtk = 0.0f;
  params->NEnvDcy = 0.10f;
  params->Mix = 0.90f;
  params->DistAmt = 0.20f;
  params->EQFreq = 0.70f;
  params->EQGain = 0.70f;
  params->Level = 0.65f;
}

static void make_hh_patch_b(po32_patch_params_t *params) {
  po32_patch_params_zero(params);
  params->OscWave = 0.5f;
  params->OscFreq = 0.68f;
  params->OscAtk = 0.0f;
  params->OscDcy = 0.16f;
  params->ModMode = 0.0f;
  params->ModRate = 0.32f;
  params->ModAmt = 0.08f;
  params->NFilMod = 0.5f;
  params->NFilFrq = 0.74f;
  params->NFilQ = 0.36f;
  params->NEnvMod = 0.5f;
  params->NEnvAtk = 0.0f;
  params->NEnvDcy = 0.18f;
  params->Mix = 0.82f;
  params->DistAmt = 0.10f;
  params->EQFreq = 0.62f;
  params->EQGain = 0.55f;
  params->Level = 0.62f;
}

static void make_any_patch_a(po32_patch_params_t *params) {
  po32_patch_params_zero(params);
  params->OscWave = 0.0f;
  params->OscFreq = 0.30f;
  params->OscAtk = 0.0f;
  params->OscDcy = 0.30f;
  params->ModMode = 0.0f;
  params->ModRate = 0.22f;
  params->ModAmt = 0.25f;
  params->NFilMod = 0.0f;
  params->NFilFrq = 0.50f;
  params->NFilQ = 0.35f;
  params->NEnvMod = 0.0f;
  params->NEnvAtk = 0.0f;
  params->NEnvDcy = 0.25f;
  params->Mix = 0.45f;
  params->DistAmt = 0.18f;
  params->EQFreq = 0.52f;
  params->EQGain = 0.58f;
  params->Level = 0.74f;
}

static void make_any_patch_b(po32_patch_params_t *params) {
  po32_patch_params_zero(params);
  params->OscWave = 1.0f;
  params->OscFreq = 0.62f;
  params->OscAtk = 0.0f;
  params->OscDcy = 0.14f;
  params->ModMode = 0.5f;
  params->ModRate = 0.52f;
  params->ModAmt = 0.16f;
  params->NFilMod = 1.0f;
  params->NFilFrq = 0.76f;
  params->NFilQ = 0.18f;
  params->NEnvMod = 1.0f;
  params->NEnvAtk = 0.0f;
  params->NEnvDcy = 0.16f;
  params->Mix = 0.72f;
  params->DistAmt = 0.22f;
  params->EQFreq = 0.64f;
  params->EQGain = 0.62f;
  params->Level = 0.70f;
}

static int has_suffix(const char *text, const char *suffix) {
  size_t text_len = strlen(text);
  size_t suffix_len = strlen(suffix);
  if (suffix_len > text_len)
    return 0;
  return strcmp(text + text_len - suffix_len, suffix) == 0;
}

static int is_patch_file_name(const char *name) {
  if (!has_suffix(name, ".mtdrum"))
    return 0;
  if (name[0] == '.' || (name[0] == '_' && name[1] == '.'))
    return 0;
  return 1;
}

static int category_name_is_excluded_from_any(const char *name) {
  return strcmp(name, DEMO_PATCH_CATEGORY_KICK) == 0 ||
         strcmp(name, DEMO_PATCH_CATEGORY_SNARE) == 0 ||
         strcmp(name, DEMO_PATCH_CATEGORY_HIHAT) == 0;
}

static int pick_random_patch_from_directory(const char *directory_path, char *out_path,
                                            size_t out_path_capacity,
                                            const char *exclude_full_path) {
  DIR *dir = opendir(directory_path);
  struct dirent *entry;
  size_t seen = 0u;
  char chosen[DEMO_PATH_MAX];

  if (dir == NULL)
    return 0;

  chosen[0] = '\0';
  while ((entry = readdir(dir)) != NULL) {
    int pick_this;
    char candidate[DEMO_PATH_MAX];
    if (!is_patch_file_name(entry->d_name))
      continue;
    (void)snprintf(candidate, sizeof(candidate), "%s/%s", directory_path, entry->d_name);
    if (exclude_full_path != NULL && strcmp(candidate, exclude_full_path) == 0)
      continue;
    ++seen;
    pick_this = ((size_t)rand() % seen) == 0u;
    if (pick_this) {
      memcpy(chosen, candidate, strlen(candidate) + 1u);
    }
  }
  (void)closedir(dir);

  if (seen == 0u || chosen[0] == '\0')
    return 0;
  if (strlen(chosen) + 1u > out_path_capacity)
    return 0;
  memcpy(out_path, chosen, strlen(chosen) + 1u);
  return 1;
}

static int pick_random_any_patch(char *out_path, size_t out_path_capacity,
                                 const char *exclude_full_path) {
  DIR *categories_dir = opendir(DEMO_PATCH_CATEGORY_DIR);
  struct dirent *category_entry;
  size_t seen = 0u;
  char chosen[DEMO_PATH_MAX];

  if (categories_dir == NULL)
    return 0;

  chosen[0] = '\0';
  while ((category_entry = readdir(categories_dir)) != NULL) {
    const char *category_name = category_entry->d_name;
    char category_path[DEMO_PATH_MAX];
    DIR *category_dir;
    struct dirent *patch_entry;
    if (category_name[0] == '.' || (category_name[0] == '_' && category_name[1] == '.'))
      continue;
    if (category_name_is_excluded_from_any(category_name))
      continue;
    (void)snprintf(category_path, sizeof(category_path), "%s/%s", DEMO_PATCH_CATEGORY_DIR,
                   category_name);
    category_dir = opendir(category_path);
    if (category_dir == NULL)
      continue;

    while ((patch_entry = readdir(category_dir)) != NULL) {
      int pick_this;
      char candidate[DEMO_PATH_MAX];
      if (!is_patch_file_name(patch_entry->d_name))
        continue;
      (void)snprintf(candidate, sizeof(candidate), "%s/%s", category_path, patch_entry->d_name);
      if (exclude_full_path != NULL && strcmp(candidate, exclude_full_path) == 0)
        continue;
      ++seen;
      pick_this = ((size_t)rand() % seen) == 0u;
      if (pick_this) {
        memcpy(chosen, candidate, strlen(candidate) + 1u);
      }
    }

    (void)closedir(category_dir);
  }

  (void)closedir(categories_dir);

  if (seen == 0u || chosen[0] == '\0')
    return 0;
  if (strlen(chosen) + 1u > out_path_capacity)
    return 0;
  memcpy(out_path, chosen, strlen(chosen) + 1u);
  return 1;
}

static int pick_random_kick_patch(char *out_path, size_t out_path_capacity) {
  return pick_random_patch_from_directory(DEMO_KICK_PATCH_DIR, out_path, out_path_capacity, NULL);
}

static int pick_random_kick_patch_excluding(char *out_path, size_t out_path_capacity,
                                            const char *exclude_full_path) {
  return pick_random_patch_from_directory(DEMO_KICK_PATCH_DIR, out_path, out_path_capacity,
                                          exclude_full_path);
}

static int pick_random_snare_patch(char *out_path, size_t out_path_capacity,
                                   const char *exclude_full_path) {
  return pick_random_patch_from_directory(DEMO_SNARE_PATCH_DIR, out_path, out_path_capacity,
                                          exclude_full_path);
}

static int pick_random_hihat_patch(char *out_path, size_t out_path_capacity,
                                   const char *exclude_full_path) {
  return pick_random_patch_from_directory(DEMO_HIHAT_PATCH_DIR, out_path, out_path_capacity,
                                          exclude_full_path);
}

typedef enum {
  PATCH_VOICE_KICK = 0,
  PATCH_VOICE_SNARE = 1,
  PATCH_VOICE_HIHAT = 2,
  PATCH_VOICE_ANY = 3
} patch_voice_t;

static int pick_random_patch_for_voice(patch_voice_t voice, char *out_path,
                                       size_t out_path_capacity, const char *exclude_full_path) {
  switch (voice) {
  case PATCH_VOICE_KICK:
    return pick_random_kick_patch_excluding(out_path, out_path_capacity, exclude_full_path);
  case PATCH_VOICE_SNARE:
    return pick_random_snare_patch(out_path, out_path_capacity, exclude_full_path);
  case PATCH_VOICE_HIHAT:
    return pick_random_hihat_patch(out_path, out_path_capacity, exclude_full_path);
  case PATCH_VOICE_ANY:
    return pick_random_any_patch(out_path, out_path_capacity, exclude_full_path);
  default:
    return 0;
  }
}

static int read_text_file(const char *path, char **out_text, size_t *out_len) {
  FILE *fp = fopen(path, "rb");
  long file_size;
  char *buffer;

  if (fp == NULL || out_text == NULL || out_len == NULL)
    return 0;
  if (fseek(fp, 0L, SEEK_END) != 0) {
    fclose(fp);
    return 0;
  }
  file_size = ftell(fp);
  if (file_size < 0L) {
    fclose(fp);
    return 0;
  }
  if (fseek(fp, 0L, SEEK_SET) != 0) {
    fclose(fp);
    return 0;
  }

  buffer = (char *)malloc((size_t)file_size + 1u);
  if (buffer == NULL) {
    fclose(fp);
    return 0;
  }
  if ((size_t)fread(buffer, 1u, (size_t)file_size, fp) != (size_t)file_size) {
    free(buffer);
    fclose(fp);
    return 0;
  }
  buffer[file_size] = '\0';
  fclose(fp);

  *out_text = buffer;
  *out_len = (size_t)file_size;
  return 1;
}

static int load_patch_from_mtdrum(const char *path, po32_patch_params_t *out_params) {
  char *text = NULL;
  char *normalized_text = NULL;
  size_t text_len = 0u;
  size_t normalized_len = 0u;
  po32_status_t status;

  if (!read_text_file(path, &text, &text_len))
    return 0;
  status = po32_patch_parse_mtdrum_text(text, text_len, out_params);

  if (status != PO32_OK) {
    normalized_text = (char *)malloc(text_len + 1u);
    if (normalized_text != NULL) {
      int replaced_equals_on_line = 0;
      for (size_t i = 0u; i < text_len; ++i) {
        char ch = text[i];
        if (ch == '\r')
          continue;
        if (ch == '\n') {
          normalized_text[normalized_len++] = ch;
          replaced_equals_on_line = 0;
          continue;
        }
        if (ch == '=' && !replaced_equals_on_line) {
          normalized_text[normalized_len++] = ':';
          replaced_equals_on_line = 1;
          continue;
        }
        normalized_text[normalized_len++] = ch;
      }
      normalized_text[normalized_len] = '\0';
      status = po32_patch_parse_mtdrum_text(normalized_text, normalized_len, out_params);
    }
  }

  free(normalized_text);
  free(text);
  return status == PO32_OK;
}

static int refresh_unmorphed_patch(patch_voice_t voice, po32_patch_params_t *patch_to_replace,
                                   char *replace_path, int *replace_has_path,
                                   const char *active_other_path, const char *old_path_to_avoid) {
  char candidate_path[DEMO_PATH_MAX];
  size_t attempts = 0u;
  while (attempts < 16u) {
    attempts++;
    if (!pick_random_patch_for_voice(voice, candidate_path, sizeof(candidate_path),
                                     active_other_path))
      return 0;
    if (old_path_to_avoid != NULL && strcmp(candidate_path, old_path_to_avoid) == 0)
      continue;
    if (!load_patch_from_mtdrum(candidate_path, patch_to_replace))
      continue;
    memcpy(replace_path, candidate_path, strlen(candidate_path) + 1u);
    *replace_has_path = 1;
    return 1;
  }
  return 0;
}

typedef enum {
  MORPH_ENDPOINT_NONE = 0,
  MORPH_ENDPOINT_A = 1,
  MORPH_ENDPOINT_B = 2
} morph_endpoint_t;

static morph_endpoint_t morph_endpoint_from_value(float morph) {
  const float epsilon = 1e-6f;
  if (morph <= epsilon)
    return MORPH_ENDPOINT_A;
  if (morph >= 1.0f - epsilon)
    return MORPH_ENDPOINT_B;
  return MORPH_ENDPOINT_NONE;
}

static float hh_morph_for_step(size_t step_index) {
  uint8_t phase = (uint8_t)(step_index % 8u);
  if (phase <= 4u)
    return (float)phase / 4.0f;
  return (float)(8u - phase) / 4.0f;
}

static float kick_morph_for_step(size_t step_index) {
  const uint8_t period_steps = 12u; /* 3 beats at 16th-note resolution */
  const uint8_t half_period = period_steps / 2u;
  uint8_t phase = (uint8_t)(step_index % period_steps);
  if (phase <= half_period)
    return (float)phase / (float)half_period;
  return (float)(period_steps - phase) / (float)half_period;
}

static float snare_morph_for_step(size_t step_index) {
  const uint8_t half_period = 7u; /* 7 steps up, 7 steps down */
  const uint8_t period_steps = (uint8_t)(half_period * 2u);
  uint8_t phase = (uint8_t)(step_index % period_steps);
  if (phase <= half_period)
    return (float)phase / (float)half_period;
  return (float)(period_steps - phase) / (float)half_period;
}

static float any_morph_for_step(size_t step_index) {
  uint8_t phase = (uint8_t)(step_index % 8u);
  return (float)phase / 7.0f;
}

static float lerp01(float a, float b, float t) {
  return a + (b - a) * t;
}

static void print_usage(const char *program_name) {
  fprintf(stderr,
          "usage: %s [--sample-rate 96000] [--bpm 160] [--num 8] [--fill 0.25] "
          "[--syncopation 0.1] [--four-on-the-floor 0.0] [--swap-prob 0.0] "
          "[--reverse 0.0] [--note 36] "
          "[--kick -3] [--snare -2] [--hihat -6] [--any -4] [output.wav] "
          "[kick_patch.mtdrum] [snare_patch.mtdrum] [hh_a.mtdrum] [hh_b.mtdrum] "
          "[any_a.mtdrum] [any_b.mtdrum]\n",
          program_name);
  fprintf(stderr, "  --sample-rate/--sr must be 8000..384000 (default %u)\n",
          (unsigned)DEMO_DEFAULT_SAMPLE_RATE);
  fprintf(stderr, "  --bpm must be > 0 (default %.1f)\n", (double)DEMO_DEFAULT_BPM);
  fprintf(stderr, "  --num must be >= 1 (default %u)\n", (unsigned)DEMO_DEFAULT_SEQUENCE_COUNT);
  fprintf(stderr, "  --fill must be between 0.0 and 1.0 (default %.2f)\n",
          (double)DEMO_DEFAULT_FILL_PROBABILITY);
  fprintf(stderr, "  --syncopation must be between 0.0 and 1.0 (default %.2f)\n",
          (double)DEMO_DEFAULT_SYNCOPATION_PROBABILITY);
  fprintf(stderr,
          "  --four-on-the-floor/--four-floor must be 0.0..1.0 (default %.2f), "
          "adds missing kick on steps 1/5/9/13\n",
          (double)DEMO_DEFAULT_FOUR_FLOOR_PROBABILITY);
  fprintf(stderr, "  --swap-prob must be between 0.0 and 1.0 (default %.2f)\n",
          (double)DEMO_DEFAULT_SWAP_PROBABILITY);
  fprintf(stderr, "  --reverse must be between 0.0 and 1.0 (default %.2f)\n",
          (double)DEMO_DEFAULT_REVERSE_PROBABILITY);
  fprintf(stderr, "  --note must be MIDI note 0..127 (quantizes OscFreq to root/fifth)\n");
  fprintf(stderr, "  --kick/--snare/--hihat/--any are dB trims (default 0.0 dB)\n");
}

static const char *patch_source_label(int has_path, const char *path) {
  if (has_path && path[0] != '\0')
    return path;
  return "built-in fallback";
}

static void interpolate_patch_params(const po32_patch_params_t *a, const po32_patch_params_t *b,
                                     float t, po32_patch_params_t *out) {
#define LERP_FIELD(field) out->field = lerp01(a->field, b->field, t)
  LERP_FIELD(OscWave);
  LERP_FIELD(OscFreq);
  LERP_FIELD(OscAtk);
  LERP_FIELD(OscDcy);
  LERP_FIELD(ModMode);
  LERP_FIELD(ModRate);
  LERP_FIELD(ModAmt);
  LERP_FIELD(NFilMod);
  LERP_FIELD(NFilFrq);
  LERP_FIELD(NFilQ);
  LERP_FIELD(NEnvMod);
  LERP_FIELD(NEnvAtk);
  LERP_FIELD(NEnvDcy);
  LERP_FIELD(Mix);
  LERP_FIELD(DistAmt);
  LERP_FIELD(EQFreq);
  LERP_FIELD(EQGain);
  LERP_FIELD(Level);
  LERP_FIELD(OscVel);
  LERP_FIELD(NVel);
  LERP_FIELD(ModVel);
#undef LERP_FIELD
}

int main(int argc, char **argv) {
  const char *wav_path = "demo_kick_160bpm.wav";
  const char *kick_patch_a_arg = NULL;
  const char *kick_patch_b_arg = NULL;
  const char *snare_patch_a_arg = NULL;
  const char *snare_patch_b_arg = NULL;
  const char *hh_patch_a_arg = NULL;
  const char *hh_patch_b_arg = NULL;
  const char *any_patch_a_arg = NULL;
  const char *any_patch_b_arg = NULL;
  const char *positionals[7] = {0};
  size_t positional_count = 0u;
  uint32_t sample_rate_hz = DEMO_DEFAULT_SAMPLE_RATE;
  float bpm = DEMO_DEFAULT_BPM;
  size_t sequence_count = DEMO_DEFAULT_SEQUENCE_COUNT;
  float fill_probability = DEMO_DEFAULT_FILL_PROBABILITY;
  float syncopation_probability = DEMO_DEFAULT_SYNCOPATION_PROBABILITY;
  float four_floor_probability = DEMO_DEFAULT_FOUR_FLOOR_PROBABILITY;
  float swap_probability = DEMO_DEFAULT_SWAP_PROBABILITY;
  float reverse_probability = DEMO_DEFAULT_REVERSE_PROBABILITY;
  int note_enabled = 0;
  int note_midi = 0;
  float kick_db = 0.0f;
  float snare_db = 0.0f;
  float hihat_db = 0.0f;
  float any_db = 0.0f;
  float kick_gain = 1.0f;
  float snare_gain = 1.0f;
  float hihat_gain = 1.0f;
  float any_gain = 1.0f;
  size_t base_steps = 0u;
  size_t total_steps = 0u;

  char random_kick_patch_a_path[DEMO_PATH_MAX];
  char random_kick_patch_b_path[DEMO_PATH_MAX];
  char random_snare_patch_a_path[DEMO_PATH_MAX];
  char random_snare_patch_b_path[DEMO_PATH_MAX];
  char random_hh_patch_a_path[DEMO_PATH_MAX];
  char random_hh_patch_b_path[DEMO_PATH_MAX];
  char random_any_patch_a_path[DEMO_PATH_MAX];
  char random_any_patch_b_path[DEMO_PATH_MAX];
  char kick_a_path[DEMO_PATH_MAX] = {0};
  char kick_b_path[DEMO_PATH_MAX] = {0};
  char snare_a_path[DEMO_PATH_MAX] = {0};
  char snare_b_path[DEMO_PATH_MAX] = {0};
  char hihat_a_path[DEMO_PATH_MAX] = {0};
  char hihat_b_path[DEMO_PATH_MAX] = {0};
  char any_a_path[DEMO_PATH_MAX] = {0};
  char any_b_path[DEMO_PATH_MAX] = {0};
  int kick_a_has_path = 0;
  int kick_b_has_path = 0;
  int snare_a_has_path = 0;
  int snare_b_has_path = 0;
  int hihat_a_has_path = 0;
  int hihat_b_has_path = 0;
  int any_a_has_path = 0;
  int any_b_has_path = 0;
  size_t kick_a_swaps = 0u;
  size_t kick_b_swaps = 0u;
  size_t snare_a_swaps = 0u;
  size_t snare_b_swaps = 0u;
  size_t hihat_a_swaps = 0u;
  size_t hihat_b_swaps = 0u;
  size_t any_a_swaps = 0u;
  size_t any_b_swaps = 0u;
  size_t reversed_steps = 0u;
  morph_endpoint_t kick_prev_endpoint = morph_endpoint_from_value(kick_morph_for_step(0u));
  morph_endpoint_t snare_prev_endpoint = morph_endpoint_from_value(snare_morph_for_step(0u));
  morph_endpoint_t hihat_prev_endpoint = morph_endpoint_from_value(hh_morph_for_step(0u));
  morph_endpoint_t any_prev_endpoint = morph_endpoint_from_value(any_morph_for_step(0u));

  po32_synth_t synth;
  po32_patch_params_t kick_a;
  po32_patch_params_t kick_b;
  po32_patch_params_t snare_a;
  po32_patch_params_t snare_b;
  po32_patch_params_t hh_a;
  po32_patch_params_t hh_b;
  po32_patch_params_t any_a;
  po32_patch_params_t any_b;
  po32_patch_params_t kick_step_patch;
  po32_patch_params_t snare_step_patch;
  po32_patch_params_t hh_step_patch;
  po32_patch_params_t any_step_patch;
  po32_status_t status;

  float step_seconds;
  size_t step_samples;
  size_t total_samples;
  size_t hit_capacity;
  size_t hh_capacity;
  size_t any_capacity;
  size_t kick_len = 0u;
  size_t snare_step_len = 0u;
  size_t hh_len = 0u;
  size_t any_len = 0u;
  size_t longest_hit_len;
  size_t output_len;
  uint8_t *kick_steps = NULL;
  uint8_t *snare_steps = NULL;
  uint8_t *hihat_steps = NULL;
  uint8_t *silence_steps = NULL;
  uint8_t *fill_lengths = NULL;
  size_t silence_count = 0u;
  size_t four_floor_added_count = 0u;

  float *kick_hit = NULL;
  float *snare_hit = NULL;
  float *hh_hit = NULL;
  float *any_hit = NULL;
  float *output = NULL;
  float peak = 0.0f;

  srand((unsigned int)time(NULL) ^ (unsigned int)getpid());

  for (int argi = 1; argi < argc; ++argi) {
    const char *arg = argv[argi];
    char *endptr = NULL;

    if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
      print_usage(argv[0]);
      return 0;
    }

    if (strcmp(arg, "--sample-rate") == 0 || strcmp(arg, "--sr") == 0) {
      unsigned long parsed = 0ul;
      if (argi + 1 >= argc) {
        fprintf(stderr, "missing value for %s\n", arg);
        print_usage(argv[0]);
        return 1;
      }
      parsed = strtoul(argv[++argi], &endptr, 10);
      if (endptr == argv[argi] || *endptr != '\0' || parsed < 8000ul || parsed > 384000ul) {
        fprintf(stderr, "invalid sample-rate value: %s\n", argv[argi]);
        print_usage(argv[0]);
        return 1;
      }
      sample_rate_hz = (uint32_t)parsed;
      continue;
    }

    if (strncmp(arg, "--sample-rate=", 14u) == 0) {
      const char *value = arg + 14;
      unsigned long parsed = strtoul(value, &endptr, 10);
      if (endptr == value || *endptr != '\0' || parsed < 8000ul || parsed > 384000ul) {
        fprintf(stderr, "invalid sample-rate value: %s\n", value);
        print_usage(argv[0]);
        return 1;
      }
      sample_rate_hz = (uint32_t)parsed;
      continue;
    }

    if (strncmp(arg, "--sr=", 5u) == 0) {
      const char *value = arg + 5;
      unsigned long parsed = strtoul(value, &endptr, 10);
      if (endptr == value || *endptr != '\0' || parsed < 8000ul || parsed > 384000ul) {
        fprintf(stderr, "invalid sample-rate value: %s\n", value);
        print_usage(argv[0]);
        return 1;
      }
      sample_rate_hz = (uint32_t)parsed;
      continue;
    }

    if (strcmp(arg, "--swap-prob") == 0 || strcmp(arg, "--swap") == 0) {
      float parsed = 0.0f;
      if (argi + 1 >= argc) {
        fprintf(stderr, "missing value for %s\n", arg);
        print_usage(argv[0]);
        return 1;
      }
      if (!parse_float_value(argv[++argi], &parsed) || parsed < 0.0f || parsed > 1.0f) {
        fprintf(stderr, "invalid %s value: %s\n", arg, argv[argi]);
        print_usage(argv[0]);
        return 1;
      }
      swap_probability = parsed;
      continue;
    }

    if (strncmp(arg, "--swap-prob=", 12u) == 0 || strncmp(arg, "--swap=", 7u) == 0) {
      const char *value = NULL;
      float parsed = 0.0f;
      if (strncmp(arg, "--swap-prob=", 12u) == 0)
        value = arg + 12;
      else
        value = arg + 7;
      if (!parse_float_value(value, &parsed) || parsed < 0.0f || parsed > 1.0f) {
        fprintf(stderr, "invalid swap probability value: %s\n", value);
        print_usage(argv[0]);
        return 1;
      }
      swap_probability = parsed;
      continue;
    }

    if (strcmp(arg, "--reverse") == 0 || strcmp(arg, "--reverse-prob") == 0) {
      float parsed = 0.0f;
      if (argi + 1 >= argc) {
        fprintf(stderr, "missing value for %s\n", arg);
        print_usage(argv[0]);
        return 1;
      }
      if (!parse_float_value(argv[++argi], &parsed) || parsed < 0.0f || parsed > 1.0f) {
        fprintf(stderr, "invalid %s value: %s\n", arg, argv[argi]);
        print_usage(argv[0]);
        return 1;
      }
      reverse_probability = parsed;
      continue;
    }

    if (strncmp(arg, "--reverse=", 10u) == 0 || strncmp(arg, "--reverse-prob=", 15u) == 0) {
      const char *value = NULL;
      float parsed = 0.0f;
      if (strncmp(arg, "--reverse=", 10u) == 0)
        value = arg + 10;
      else
        value = arg + 15;
      if (!parse_float_value(value, &parsed) || parsed < 0.0f || parsed > 1.0f) {
        fprintf(stderr, "invalid reverse probability value: %s\n", value);
        print_usage(argv[0]);
        return 1;
      }
      reverse_probability = parsed;
      continue;
    }

    if (strcmp(arg, "--note") == 0) {
      long parsed = 0;
      if (argi + 1 >= argc) {
        fprintf(stderr, "missing value for --note\n");
        print_usage(argv[0]);
        return 1;
      }
      parsed = strtol(argv[++argi], &endptr, 10);
      if (endptr == argv[argi] || *endptr != '\0' || parsed < 0 || parsed > 127) {
        fprintf(stderr, "invalid --note value: %s\n", argv[argi]);
        print_usage(argv[0]);
        return 1;
      }
      note_enabled = 1;
      note_midi = (int)parsed;
      continue;
    }

    if (strncmp(arg, "--note=", 7u) == 0) {
      const char *value = arg + 7;
      long parsed = strtol(value, &endptr, 10);
      if (endptr == value || *endptr != '\0' || parsed < 0 || parsed > 127) {
        fprintf(stderr, "invalid --note value: %s\n", value);
        print_usage(argv[0]);
        return 1;
      }
      note_enabled = 1;
      note_midi = (int)parsed;
      continue;
    }

    if (strcmp(arg, "--kick") == 0 || strcmp(arg, "--snare") == 0 || strcmp(arg, "--hihat") == 0 ||
        strcmp(arg, "--any") == 0) {
      float parsed = 0.0f;
      float *target_db = NULL;
      if (argi + 1 >= argc) {
        fprintf(stderr, "missing value for %s\n", arg);
        print_usage(argv[0]);
        return 1;
      }
      if (strcmp(arg, "--kick") == 0)
        target_db = &kick_db;
      else if (strcmp(arg, "--snare") == 0)
        target_db = &snare_db;
      else if (strcmp(arg, "--hihat") == 0)
        target_db = &hihat_db;
      else
        target_db = &any_db;
      if (!parse_float_value(argv[++argi], &parsed)) {
        fprintf(stderr, "invalid %s value: %s\n", arg, argv[argi]);
        print_usage(argv[0]);
        return 1;
      }
      *target_db = parsed;
      continue;
    }

    if (strncmp(arg, "--kick=", 7u) == 0 || strncmp(arg, "--snare=", 8u) == 0 ||
        strncmp(arg, "--hihat=", 8u) == 0 || strncmp(arg, "--any=", 6u) == 0) {
      const char *value = NULL;
      float parsed = 0.0f;
      float *target_db = NULL;
      if (strncmp(arg, "--kick=", 7u) == 0) {
        value = arg + 7;
        target_db = &kick_db;
      } else if (strncmp(arg, "--snare=", 8u) == 0) {
        value = arg + 8;
        target_db = &snare_db;
      } else if (strncmp(arg, "--hihat=", 8u) == 0) {
        value = arg + 8;
        target_db = &hihat_db;
      } else {
        value = arg + 6;
        target_db = &any_db;
      }
      if (!parse_float_value(value, &parsed)) {
        fprintf(stderr, "invalid dB option value: %s\n", value);
        print_usage(argv[0]);
        return 1;
      }
      *target_db = parsed;
      continue;
    }

    if (strcmp(arg, "--bpm") == 0) {
      float parsed = 0.0f;
      if (argi + 1 >= argc) {
        fputs("missing value for --bpm\n", stderr);
        print_usage(argv[0]);
        return 1;
      }
      parsed = strtof(argv[++argi], &endptr);
      if (endptr == argv[argi] || *endptr != '\0' || parsed <= 0.0f) {
        fprintf(stderr, "invalid --bpm value: %s\n", argv[argi]);
        print_usage(argv[0]);
        return 1;
      }
      bpm = parsed;
      continue;
    }

    if (strncmp(arg, "--bpm=", 6u) == 0) {
      const char *value = arg + 6;
      float parsed = strtof(value, &endptr);
      if (endptr == value || *endptr != '\0' || parsed <= 0.0f) {
        fprintf(stderr, "invalid --bpm value: %s\n", value);
        print_usage(argv[0]);
        return 1;
      }
      bpm = parsed;
      continue;
    }

    if (strcmp(arg, "--num") == 0) {
      unsigned long parsed = 0ul;
      if (argi + 1 >= argc) {
        fputs("missing value for --num\n", stderr);
        print_usage(argv[0]);
        return 1;
      }
      parsed = strtoul(argv[++argi], &endptr, 10);
      if (endptr == argv[argi] || *endptr != '\0' || parsed == 0ul ||
          parsed > (unsigned long)(SIZE_MAX / DEMO_SEQUENCE_STEPS)) {
        fprintf(stderr, "invalid --num value: %s\n", argv[argi]);
        print_usage(argv[0]);
        return 1;
      }
      sequence_count = (size_t)parsed;
      continue;
    }

    if (strncmp(arg, "--num=", 6u) == 0) {
      const char *value = arg + 6;
      unsigned long parsed = strtoul(value, &endptr, 10);
      if (endptr == value || *endptr != '\0' || parsed == 0ul ||
          parsed > (unsigned long)(SIZE_MAX / DEMO_SEQUENCE_STEPS)) {
        fprintf(stderr, "invalid --num value: %s\n", value);
        print_usage(argv[0]);
        return 1;
      }
      sequence_count = (size_t)parsed;
      continue;
    }

    if (strcmp(arg, "--fill") == 0 || strcmp(arg, "--fill-prob") == 0) {
      float parsed = 0.0f;
      if (argi + 1 >= argc) {
        fprintf(stderr, "missing value for %s\n", arg);
        print_usage(argv[0]);
        return 1;
      }
      parsed = strtof(argv[++argi], &endptr);
      if (endptr == argv[argi] || *endptr != '\0' || parsed < 0.0f || parsed > 1.0f) {
        fprintf(stderr, "invalid --fill value: %s\n", argv[argi]);
        print_usage(argv[0]);
        return 1;
      }
      fill_probability = parsed;
      continue;
    }

    if (strncmp(arg, "--fill=", 7u) == 0 || strncmp(arg, "--fill-prob=", 12u) == 0) {
      const char *value = (arg[6] == '=') ? (arg + 7) : (arg + 12);
      float parsed = strtof(value, &endptr);
      if (endptr == value || *endptr != '\0' || parsed < 0.0f || parsed > 1.0f) {
        fprintf(stderr, "invalid --fill value: %s\n", value);
        print_usage(argv[0]);
        return 1;
      }
      fill_probability = parsed;
      continue;
    }

    if (strcmp(arg, "--syncopation") == 0) {
      float parsed = 0.0f;
      if (argi + 1 >= argc) {
        fputs("missing value for --syncopation\n", stderr);
        print_usage(argv[0]);
        return 1;
      }
      parsed = strtof(argv[++argi], &endptr);
      if (endptr == argv[argi] || *endptr != '\0' || parsed < 0.0f || parsed > 1.0f) {
        fprintf(stderr, "invalid --syncopation value: %s\n", argv[argi]);
        print_usage(argv[0]);
        return 1;
      }
      syncopation_probability = parsed;
      continue;
    }

    if (strncmp(arg, "--syncopation=", 14u) == 0) {
      const char *value = arg + 14;
      float parsed = strtof(value, &endptr);
      if (endptr == value || *endptr != '\0' || parsed < 0.0f || parsed > 1.0f) {
        fprintf(stderr, "invalid --syncopation value: %s\n", value);
        print_usage(argv[0]);
        return 1;
      }
      syncopation_probability = parsed;
      continue;
    }

    if (strcmp(arg, "--four-on-the-floor") == 0 || strcmp(arg, "--four-floor") == 0 ||
        strcmp(arg, "--four-floor-prob") == 0) {
      float parsed = 0.0f;
      if (argi + 1 >= argc) {
        fprintf(stderr, "missing value for %s\n", arg);
        print_usage(argv[0]);
        return 1;
      }
      if (!parse_float_value(argv[++argi], &parsed) || parsed < 0.0f || parsed > 1.0f) {
        fprintf(stderr, "invalid %s value: %s\n", arg, argv[argi]);
        print_usage(argv[0]);
        return 1;
      }
      four_floor_probability = parsed;
      continue;
    }

    if (strncmp(arg, "--four-on-the-floor=", 20u) == 0 || strncmp(arg, "--four-floor=", 13u) == 0 ||
        strncmp(arg, "--four-floor-prob=", 18u) == 0) {
      const char *value = NULL;
      float parsed = 0.0f;
      if (strncmp(arg, "--four-on-the-floor=", 20u) == 0) {
        value = arg + 20;
      } else if (strncmp(arg, "--four-floor=", 13u) == 0) {
        value = arg + 13;
      } else {
        value = arg + 18;
      }
      if (!parse_float_value(value, &parsed) || parsed < 0.0f || parsed > 1.0f) {
        fprintf(stderr, "invalid four-on-the-floor probability value: %s\n", value);
        print_usage(argv[0]);
        return 1;
      }
      four_floor_probability = parsed;
      continue;
    }

    if (arg[0] == '-' && arg[1] != '\0') {
      fprintf(stderr, "unknown option: %s\n", arg);
      print_usage(argv[0]);
      return 1;
    }

    if (positional_count >= 7u) {
      fputs("too many positional arguments\n", stderr);
      print_usage(argv[0]);
      return 1;
    }
    positionals[positional_count++] = arg;
  }

  if (positional_count >= 1u)
    wav_path = positionals[0];
  if (positional_count >= 2u)
    kick_patch_a_arg = positionals[1];
  if (positional_count >= 3u)
    snare_patch_a_arg = positionals[2];
  if (positional_count >= 4u)
    hh_patch_a_arg = positionals[3];
  if (positional_count >= 5u)
    hh_patch_b_arg = positionals[4];
  if (positional_count >= 6u)
    any_patch_a_arg = positionals[5];
  if (positional_count >= 7u)
    any_patch_b_arg = positionals[6];

  if (sequence_count > SIZE_MAX / DEMO_SEQUENCE_STEPS) {
    fputs("--num is too large\n", stderr);
    return 1;
  }
  base_steps = sequence_count * DEMO_SEQUENCE_STEPS;
  total_steps = base_steps;
  step_seconds = (60.0f / bpm) / 4.0f;
  step_samples = (size_t)(step_seconds * (float)sample_rate_hz + 0.5f);
  if (step_samples == 0u)
    step_samples = 1u;
  if (total_steps != 0u && step_samples > SIZE_MAX / total_steps) {
    fputs("requested pattern is too large\n", stderr);
    return 1;
  }
  total_samples = step_samples * total_steps;

  kick_gain = db_to_gain(kick_db);
  snare_gain = db_to_gain(snare_db);
  hihat_gain = db_to_gain(hihat_db);
  any_gain = db_to_gain(any_db);
  if (!isfinite(kick_gain) || !isfinite(snare_gain) || !isfinite(hihat_gain) ||
      !isfinite(any_gain)) {
    fputs("one or more dB trims produced invalid gain values\n", stderr);
    return 1;
  }

  po32_synth_init(&synth, sample_rate_hz);

  if (kick_patch_a_arg != NULL) {
    if (!load_patch_from_mtdrum(kick_patch_a_arg, &kick_a)) {
      fprintf(stderr, "failed to parse kick patch A file: %s\n", kick_patch_a_arg);
      return 1;
    }
    memcpy(kick_a_path, kick_patch_a_arg, strlen(kick_patch_a_arg) + 1u);
    kick_a_has_path = 1;
  } else if (pick_random_kick_patch(random_kick_patch_a_path, sizeof(random_kick_patch_a_path)) &&
             load_patch_from_mtdrum(random_kick_patch_a_path, &kick_a)) {
    kick_patch_a_arg = random_kick_patch_a_path;
    memcpy(kick_a_path, random_kick_patch_a_path, strlen(random_kick_patch_a_path) + 1u);
    kick_a_has_path = 1;
  } else {
    make_kick_patch(&kick_a);
  }

  if (pick_random_kick_patch_excluding(random_kick_patch_b_path, sizeof(random_kick_patch_b_path),
                                       kick_patch_a_arg) &&
      load_patch_from_mtdrum(random_kick_patch_b_path, &kick_b)) {
    kick_patch_b_arg = random_kick_patch_b_path;
    memcpy(kick_b_path, random_kick_patch_b_path, strlen(random_kick_patch_b_path) + 1u);
    kick_b_has_path = 1;
  } else {
    make_kick_patch_b(&kick_b);
  }

  if (snare_patch_a_arg != NULL) {
    if (!load_patch_from_mtdrum(snare_patch_a_arg, &snare_a)) {
      fprintf(stderr, "failed to parse snare patch A file: %s\n", snare_patch_a_arg);
      return 1;
    }
    memcpy(snare_a_path, snare_patch_a_arg, strlen(snare_patch_a_arg) + 1u);
    snare_a_has_path = 1;
  } else if (pick_random_snare_patch(random_snare_patch_a_path, sizeof(random_snare_patch_a_path),
                                     NULL) &&
             load_patch_from_mtdrum(random_snare_patch_a_path, &snare_a)) {
    snare_patch_a_arg = random_snare_patch_a_path;
    memcpy(snare_a_path, random_snare_patch_a_path, strlen(random_snare_patch_a_path) + 1u);
    snare_a_has_path = 1;
  } else {
    make_snare_patch(&snare_a);
  }

  if (pick_random_snare_patch(random_snare_patch_b_path, sizeof(random_snare_patch_b_path),
                              snare_patch_a_arg) &&
      load_patch_from_mtdrum(random_snare_patch_b_path, &snare_b)) {
    snare_patch_b_arg = random_snare_patch_b_path;
    memcpy(snare_b_path, random_snare_patch_b_path, strlen(random_snare_patch_b_path) + 1u);
    snare_b_has_path = 1;
  } else {
    make_snare_patch_b(&snare_b);
  }

  if (hh_patch_a_arg != NULL) {
    if (!load_patch_from_mtdrum(hh_patch_a_arg, &hh_a)) {
      fprintf(stderr, "failed to parse hihat A patch file: %s\n", hh_patch_a_arg);
      return 1;
    }
    memcpy(hihat_a_path, hh_patch_a_arg, strlen(hh_patch_a_arg) + 1u);
    hihat_a_has_path = 1;
  } else if (pick_random_hihat_patch(random_hh_patch_a_path, sizeof(random_hh_patch_a_path),
                                     NULL) &&
             load_patch_from_mtdrum(random_hh_patch_a_path, &hh_a)) {
    hh_patch_a_arg = random_hh_patch_a_path;
    memcpy(hihat_a_path, random_hh_patch_a_path, strlen(random_hh_patch_a_path) + 1u);
    hihat_a_has_path = 1;
  } else {
    make_hh_patch_a(&hh_a);
  }

  if (hh_patch_b_arg != NULL) {
    if (!load_patch_from_mtdrum(hh_patch_b_arg, &hh_b)) {
      fprintf(stderr, "failed to parse hihat B patch file: %s\n", hh_patch_b_arg);
      return 1;
    }
    memcpy(hihat_b_path, hh_patch_b_arg, strlen(hh_patch_b_arg) + 1u);
    hihat_b_has_path = 1;
  } else if (pick_random_hihat_patch(random_hh_patch_b_path, sizeof(random_hh_patch_b_path),
                                     hh_patch_a_arg) &&
             load_patch_from_mtdrum(random_hh_patch_b_path, &hh_b)) {
    hh_patch_b_arg = random_hh_patch_b_path;
    memcpy(hihat_b_path, random_hh_patch_b_path, strlen(random_hh_patch_b_path) + 1u);
    hihat_b_has_path = 1;
  } else {
    make_hh_patch_b(&hh_b);
  }

  if (any_patch_a_arg != NULL) {
    if (!load_patch_from_mtdrum(any_patch_a_arg, &any_a)) {
      fprintf(stderr, "failed to parse any A patch file: %s\n", any_patch_a_arg);
      return 1;
    }
    memcpy(any_a_path, any_patch_a_arg, strlen(any_patch_a_arg) + 1u);
    any_a_has_path = 1;
  } else if (pick_random_any_patch(random_any_patch_a_path, sizeof(random_any_patch_a_path),
                                   NULL) &&
             load_patch_from_mtdrum(random_any_patch_a_path, &any_a)) {
    any_patch_a_arg = random_any_patch_a_path;
    memcpy(any_a_path, random_any_patch_a_path, strlen(random_any_patch_a_path) + 1u);
    any_a_has_path = 1;
  } else {
    make_any_patch_a(&any_a);
  }

  if (any_patch_b_arg != NULL) {
    if (!load_patch_from_mtdrum(any_patch_b_arg, &any_b)) {
      fprintf(stderr, "failed to parse any B patch file: %s\n", any_patch_b_arg);
      return 1;
    }
    memcpy(any_b_path, any_patch_b_arg, strlen(any_patch_b_arg) + 1u);
    any_b_has_path = 1;
  } else if (pick_random_any_patch(random_any_patch_b_path, sizeof(random_any_patch_b_path),
                                   any_patch_a_arg) &&
             load_patch_from_mtdrum(random_any_patch_b_path, &any_b)) {
    any_patch_b_arg = random_any_patch_b_path;
    memcpy(any_b_path, random_any_patch_b_path, strlen(random_any_patch_b_path) + 1u);
    any_b_has_path = 1;
  } else {
    make_any_patch_b(&any_b);
  }

  kick_steps = (uint8_t *)malloc(base_steps * sizeof(*kick_steps));
  snare_steps = (uint8_t *)malloc(base_steps * sizeof(*snare_steps));
  hihat_steps = (uint8_t *)malloc(base_steps * sizeof(*hihat_steps));
  silence_steps = (uint8_t *)malloc(base_steps * sizeof(*silence_steps));
  fill_lengths = (uint8_t *)malloc(sequence_count * sizeof(*fill_lengths));
  if (kick_steps == NULL || snare_steps == NULL || hihat_steps == NULL || silence_steps == NULL ||
      fill_lengths == NULL) {
    fputs("failed to allocate step buffers\n", stderr);
    free(kick_steps);
    free(snare_steps);
    free(hihat_steps);
    free(silence_steps);
    free(fill_lengths);
    return 1;
  }

  generate_step_patterns(kick_steps, snare_steps, hihat_steps, fill_lengths, silence_steps,
                         sequence_count, base_steps, fill_probability, syncopation_probability,
                         four_floor_probability, &four_floor_added_count);
  for (size_t i = 0u; i < base_steps; ++i) {
    if (silence_steps[i] != 0u)
      ++silence_count;
  }

  hit_capacity = po32_synth_samples_for_duration(&synth, DEMO_HIT_SECONDS);
  hh_capacity = po32_synth_samples_for_duration(&synth, DEMO_HH_SECONDS);
  any_capacity = po32_synth_samples_for_duration(&synth, DEMO_ANY_SECONDS);

  kick_hit = (float *)malloc(hit_capacity * sizeof(*kick_hit));
  snare_hit = (float *)malloc(hit_capacity * sizeof(*snare_hit));
  hh_hit = (float *)malloc(hh_capacity * sizeof(*hh_hit));
  any_hit = (float *)malloc(any_capacity * sizeof(*any_hit));
  if (kick_hit == NULL || snare_hit == NULL || hh_hit == NULL || any_hit == NULL) {
    fputs("failed to allocate hit buffers\n", stderr);
    free(kick_hit);
    free(snare_hit);
    free(hh_hit);
    free(any_hit);
    free(kick_steps);
    free(snare_steps);
    free(hihat_steps);
    free(silence_steps);
    free(fill_lengths);
    return 1;
  }

  longest_hit_len = hit_capacity;
  if (hh_capacity > longest_hit_len)
    longest_hit_len = hh_capacity;
  if (any_capacity > longest_hit_len)
    longest_hit_len = any_capacity;

  output_len = total_samples + longest_hit_len;
  output = (float *)calloc(output_len, sizeof(*output));
  if (output == NULL) {
    fputs("failed to allocate output buffer\n", stderr);
    free(kick_hit);
    free(snare_hit);
    free(hh_hit);
    free(any_hit);
    free(kick_steps);
    free(snare_steps);
    free(hihat_steps);
    free(silence_steps);
    free(fill_lengths);
    return 1;
  }

  for (size_t step = 0u; step < total_steps; ++step) {
    size_t step_index = step % base_steps;
    uint8_t step16 = (uint8_t)(step_index % DEMO_SEQUENCE_STEPS);
    size_t start = step * step_samples;
    float kick_morph = kick_morph_for_step(step);
    float snare_morph = snare_morph_for_step(step);
    float hihat_morph = hh_morph_for_step(step_index);
    float any_morph = any_morph_for_step(step_index);
    int is_silent_step = silence_steps[step_index] != 0u;
    int reverse_step = chance_probability(reverse_probability);
    morph_endpoint_t endpoint;

    if (reverse_step)
      ++reversed_steps;

    endpoint = morph_endpoint_from_value(kick_morph);
    if (endpoint != MORPH_ENDPOINT_NONE && endpoint != kick_prev_endpoint) {
      if (chance_probability(swap_probability)) {
        if (endpoint == MORPH_ENDPOINT_B) {
          if (refresh_unmorphed_patch(PATCH_VOICE_KICK, &kick_a, kick_a_path, &kick_a_has_path,
                                      kick_b_has_path ? kick_b_path : NULL,
                                      kick_a_has_path ? kick_a_path : NULL)) {
            ++kick_a_swaps;
          }
        } else {
          if (refresh_unmorphed_patch(PATCH_VOICE_KICK, &kick_b, kick_b_path, &kick_b_has_path,
                                      kick_a_has_path ? kick_a_path : NULL,
                                      kick_b_has_path ? kick_b_path : NULL)) {
            ++kick_b_swaps;
          }
        }
      }
    }
    kick_prev_endpoint = endpoint;

    endpoint = morph_endpoint_from_value(snare_morph);
    if (endpoint != MORPH_ENDPOINT_NONE && endpoint != snare_prev_endpoint) {
      if (chance_probability(swap_probability)) {
        if (endpoint == MORPH_ENDPOINT_B) {
          if (refresh_unmorphed_patch(PATCH_VOICE_SNARE, &snare_a, snare_a_path, &snare_a_has_path,
                                      snare_b_has_path ? snare_b_path : NULL,
                                      snare_a_has_path ? snare_a_path : NULL)) {
            ++snare_a_swaps;
          }
        } else {
          if (refresh_unmorphed_patch(PATCH_VOICE_SNARE, &snare_b, snare_b_path, &snare_b_has_path,
                                      snare_a_has_path ? snare_a_path : NULL,
                                      snare_b_has_path ? snare_b_path : NULL)) {
            ++snare_b_swaps;
          }
        }
      }
    }
    snare_prev_endpoint = endpoint;

    endpoint = morph_endpoint_from_value(hihat_morph);
    if (endpoint != MORPH_ENDPOINT_NONE && endpoint != hihat_prev_endpoint) {
      if (chance_probability(swap_probability)) {
        if (endpoint == MORPH_ENDPOINT_B) {
          if (refresh_unmorphed_patch(PATCH_VOICE_HIHAT, &hh_a, hihat_a_path, &hihat_a_has_path,
                                      hihat_b_has_path ? hihat_b_path : NULL,
                                      hihat_a_has_path ? hihat_a_path : NULL)) {
            ++hihat_a_swaps;
          }
        } else {
          if (refresh_unmorphed_patch(PATCH_VOICE_HIHAT, &hh_b, hihat_b_path, &hihat_b_has_path,
                                      hihat_a_has_path ? hihat_a_path : NULL,
                                      hihat_b_has_path ? hihat_b_path : NULL)) {
            ++hihat_b_swaps;
          }
        }
      }
    }
    hihat_prev_endpoint = endpoint;

    endpoint = morph_endpoint_from_value(any_morph);
    if (endpoint != MORPH_ENDPOINT_NONE && endpoint != any_prev_endpoint) {
      if (chance_probability(swap_probability)) {
        if (endpoint == MORPH_ENDPOINT_B) {
          if (refresh_unmorphed_patch(PATCH_VOICE_ANY, &any_a, any_a_path, &any_a_has_path,
                                      any_b_has_path ? any_b_path : NULL,
                                      any_a_has_path ? any_a_path : NULL)) {
            ++any_a_swaps;
          }
        } else {
          if (refresh_unmorphed_patch(PATCH_VOICE_ANY, &any_b, any_b_path, &any_b_has_path,
                                      any_a_has_path ? any_a_path : NULL,
                                      any_b_has_path ? any_b_path : NULL)) {
            ++any_b_swaps;
          }
        }
      }
    }
    any_prev_endpoint = endpoint;

    if (is_silent_step)
      continue;

    if (kick_steps[step_index] != 0u) {
      interpolate_patch_params(&kick_a, &kick_b, kick_morph, &kick_step_patch);
      if (note_enabled)
        quantize_patch_oscfreq_to_root_fifth(&kick_step_patch, note_midi);
      if (reverse_step)
        reverse_patch_attack_decay(&kick_step_patch);
      status = po32_synth_render(&synth, &kick_step_patch, DEMO_KICK_VELOCITY, DEMO_HIT_SECONDS,
                                 kick_hit, hit_capacity, &kick_len);
      if (status != PO32_OK) {
        fprintf(stderr, "failed to render kick hit on step %u: %d\n", (unsigned)(step + 1u),
                status);
        free(output);
        free(kick_hit);
        free(snare_hit);
        free(hh_hit);
        free(any_hit);
        free(kick_steps);
        free(snare_steps);
        free(hihat_steps);
        free(silence_steps);
        free(fill_lengths);
        return 1;
      }
      for (size_t i = 0u; i < kick_len && start + i < output_len; ++i)
        output[start + i] += kick_hit[i] * kick_gain;
    }

    if (snare_steps[step_index] != 0u) {
      interpolate_patch_params(&snare_a, &snare_b, snare_morph, &snare_step_patch);
      if (note_enabled)
        quantize_patch_oscfreq_to_root_fifth(&snare_step_patch, note_midi);
      if (reverse_step)
        reverse_patch_attack_decay(&snare_step_patch);
      status = po32_synth_render(&synth, &snare_step_patch, DEMO_SNARE_VELOCITY, DEMO_HIT_SECONDS,
                                 snare_hit, hit_capacity, &snare_step_len);
      if (status != PO32_OK) {
        fprintf(stderr, "failed to render snare hit on step %u: %d\n", (unsigned)(step + 1u),
                status);
        free(output);
        free(kick_hit);
        free(snare_hit);
        free(hh_hit);
        free(any_hit);
        free(kick_steps);
        free(snare_steps);
        free(hihat_steps);
        free(silence_steps);
        free(fill_lengths);
        return 1;
      }
      for (size_t i = 0u; i < snare_step_len && start + i < output_len; ++i)
        output[start + i] += snare_hit[i] * snare_gain;
    }

    if (hihat_steps[step_index] != 0u) {
      interpolate_patch_params(&hh_a, &hh_b, hihat_morph, &hh_step_patch);
      if (note_enabled)
        quantize_patch_oscfreq_to_root_fifth(&hh_step_patch, note_midi);
      if (reverse_step)
        reverse_patch_attack_decay(&hh_step_patch);
      status = po32_synth_render(&synth, &hh_step_patch, DEMO_HH_VELOCITY, DEMO_HH_SECONDS, hh_hit,
                                 hh_capacity, &hh_len);
      if (status != PO32_OK) {
        fprintf(stderr, "failed to render hihat hit on step %u: %d\n", (unsigned)(step + 1u),
                status);
        free(output);
        free(kick_hit);
        free(snare_hit);
        free(hh_hit);
        free(any_hit);
        free(kick_steps);
        free(snare_steps);
        free(hihat_steps);
        free(silence_steps);
        free(fill_lengths);
        return 1;
      }
      for (size_t i = 0u; i < hh_len && start + i < output_len; ++i)
        output[start + i] += hh_hit[i] * hihat_gain;
    }

    if (is_any_step(step16)) {
      interpolate_patch_params(&any_a, &any_b, any_morph, &any_step_patch);
      if (note_enabled)
        quantize_patch_oscfreq_to_root_fifth(&any_step_patch, note_midi);
      if (reverse_step)
        reverse_patch_attack_decay(&any_step_patch);
      status = po32_synth_render(&synth, &any_step_patch, DEMO_ANY_VELOCITY, DEMO_ANY_SECONDS,
                                 any_hit, any_capacity, &any_len);
      if (status != PO32_OK) {
        fprintf(stderr, "failed to render any morph hit on step %u: %d\n", (unsigned)(step + 1u),
                status);
        free(output);
        free(kick_hit);
        free(snare_hit);
        free(hh_hit);
        free(any_hit);
        free(kick_steps);
        free(snare_steps);
        free(hihat_steps);
        free(silence_steps);
        free(fill_lengths);
        return 1;
      }
      for (size_t i = 0u; i < any_len && start + i < output_len; ++i)
        output[start + i] += any_hit[i] * any_gain;
    }
  }

  for (size_t i = 0u; i < output_len; ++i) {
    float mag = output[i];
    if (mag < 0.0f)
      mag = -mag;
    if (mag > peak)
      peak = mag;
  }
  if (peak > 1.0f) {
    float gain = 1.0f / peak;
    for (size_t i = 0u; i < output_len; ++i)
      output[i] *= gain;
  }

  if (!write_wav(wav_path, output, output_len, sample_rate_hz)) {
    fprintf(stderr, "failed to write wav file: %s\n", wav_path);
    free(output);
    free(kick_hit);
    free(snare_hit);
    free(hh_hit);
    free(any_hit);
    free(kick_steps);
    free(snare_steps);
    free(hihat_steps);
    free(silence_steps);
    free(fill_lengths);
    return 1;
  }

  printf("wrote %s\n", wav_path);
  printf("sample rate: %u Hz\n", (unsigned)sample_rate_hz);
  printf("pattern: generated %ux16-step sequences (%u total steps) at %.0f BPM\n",
         (unsigned)sequence_count, (unsigned)total_steps, bpm);
  printf("fill chance: %.1f%% on sequences %u, %u, %u, ... (last 8, 12, or 16 steps)\n",
         (double)(fill_probability * 100.0f), (unsigned)DEMO_FILL_EVERY_N_SEQUENCES,
         (unsigned)(DEMO_FILL_EVERY_N_SEQUENCES * 2u), (unsigned)(DEMO_FILL_EVERY_N_SEQUENCES * 3u));
  printf("sequence grouping: in each 8-sequence block, seq 1/2/3/5/6/7 are identical; "
         "seq 4 and 8 vary only when their fill chance hits\n");
  for (size_t seq = 0u; seq < sequence_count; ++seq) {
    if (fill_lengths[seq] == 0u) {
      printf("  seq %u fill: none\n", (unsigned)(seq + 1u));
    } else {
      printf("  seq %u fill: last %u steps\n", (unsigned)(seq + 1u), (unsigned)fill_lengths[seq]);
    }
  }
  printf("kick morph: Kick A -> Kick B -> Kick A over 3 beats (triangle), continuous across all "
         "steps\n");
  printf(
      "snare morph: Snare A -> Snare B -> Snare A over 7 steps (triangle), continuous across all "
      "steps\n");
  printf("hihat morph: HH A -> HH B over 4 steps, then back over 4 steps (oscillating)\n");
  printf("any morph: ANY A -> ANY B over 8 steps (repeats every 8 steps)\n");
  printf("syncopation: %zu/%u base steps forced silent (configured %.1f%% dropout)\n",
         silence_count, (unsigned)base_steps, (double)(syncopation_probability * 100.0f));
  printf("four-on-the-floor add: %.1f%% chance on missing kick steps 1/5/9/13 "
         "(added %zu steps)\n",
         (double)(four_floor_probability * 100.0f), four_floor_added_count);
  printf("swap probability: %.1f%% when a morph endpoint is reached\n",
         (double)(swap_probability * 100.0f));
  printf("reverse probability: %.1f%% per step (reversed steps: %zu/%u)\n",
         (double)(reverse_probability * 100.0f), reversed_steps, (unsigned)total_steps);
  if (note_enabled) {
    printf("note quantize: MIDI %d root/fifth only (pitch classes %d and %d)\n", note_midi,
           pitch_class_mod12(note_midi), (pitch_class_mod12(note_midi) + 7) % 12);
  } else {
    printf("note quantize: disabled\n");
  }
  printf("mix trim: kick %.1f dB, snare %.1f dB, hihat %.1f dB, any %.1f dB\n", (double)kick_db,
         (double)snare_db, (double)hihat_db, (double)any_db);
  printf("kick patches: A=%s, B=%s (A swaps=%zu, B swaps=%zu)\n",
         patch_source_label(kick_a_has_path, kick_a_path),
         patch_source_label(kick_b_has_path, kick_b_path), kick_a_swaps, kick_b_swaps);
  printf("snare patches: A=%s, B=%s (A swaps=%zu, B swaps=%zu)\n",
         patch_source_label(snare_a_has_path, snare_a_path),
         patch_source_label(snare_b_has_path, snare_b_path), snare_a_swaps, snare_b_swaps);
  printf("hihat patches: A=%s, B=%s (A swaps=%zu, B swaps=%zu)\n",
         patch_source_label(hihat_a_has_path, hihat_a_path),
         patch_source_label(hihat_b_has_path, hihat_b_path), hihat_a_swaps, hihat_b_swaps);
  printf("any patches: A=%s, B=%s (A swaps=%zu, B swaps=%zu)\n",
         patch_source_label(any_a_has_path, any_a_path),
         patch_source_label(any_b_has_path, any_b_path), any_a_swaps, any_b_swaps);

  printf("duration: %.3f s (%zu samples @ %u Hz)\n", (double)output_len / (double)sample_rate_hz,
         output_len, (unsigned)sample_rate_hz);

  free(output);
  free(kick_hit);
  free(snare_hit);
  free(hh_hit);
  free(any_hit);
  free(kick_steps);
  free(snare_steps);
  free(hihat_steps);
  free(silence_steps);
  free(fill_lengths);
  return 0;
}
