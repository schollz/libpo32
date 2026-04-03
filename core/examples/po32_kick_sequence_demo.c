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

#include <ctype.h>
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DEMO_SAMPLE_RATE    44100u
#define DEMO_BASE_STEPS     64u
#define DEMO_TOTAL_STEPS    128u
#define DEMO_BPM            160.0f
#define DEMO_KICK_VELOCITY  120
#define DEMO_SNARE_VELOCITY 118
#define DEMO_HH_VELOCITY    110
#define DEMO_ANY_VELOCITY   114
#define DEMO_HIT_SECONDS    0.50f
#define DEMO_HH_SECONDS     0.25f
#define DEMO_ANY_SECONDS    0.40f
#define DEMO_PATCH_DIR      "/Library/Audio/Presets/Sonic Charge/Microtonic Drum Patches/All"
#define DEMO_PATH_MAX       4096u

static int write_wav(const char *path, const float *samples, size_t sample_count,
                     uint32_t sample_rate_hz) {
  FILE *fp = fopen(path, "wb");
  if (fp == NULL)
    return 0;

  {
    uint32_t data_bytes = (uint32_t)(sample_count * 2u);
    uint32_t file_size = 36u + data_bytes;
    uint16_t channels = 1u;
    uint16_t bits = 16u;
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
    int16_t pcm_sample;
    if (sample > 1.0f)
      sample = 1.0f;
    if (sample < -1.0f)
      sample = -1.0f;
    pcm_sample = (int16_t)(sample * 32767.0f);
    fwrite(&pcm_sample, 2u, 1u, fp);
  }

  fclose(fp);
  return 1;
}

static int is_kick_step(uint8_t step_index) {
  if (step_index >= 56u) {
    /* 8-step fill: dense kicks driving back to step 1. */
    switch (step_index) {
    case 56u:
    case 58u:
    case 59u:
    case 60u:
    case 62u:
    case 63u:
      return 1;
    default:
      return 0;
    }
  }

  step_index = (uint8_t)(step_index % 32u);
  switch (step_index) {
  case 0u:
  case 3u:
  case 6u:
  case 10u:
  case 15u:
  case 18u:
  case 22u:
  case 25u:
  case 30u:
    return 1;
  default:
    return 0;
  }
}

static int is_snare_step(uint8_t step_index) {
  if (step_index >= 56u) {
    /* 8-step fill: snare barrage into the loop reset. */
    switch (step_index) {
    case 57u:
    case 58u:
    case 59u:
    case 61u:
    case 62u:
    case 63u:
      return 1;
    default:
      return 0;
    }
  }

  step_index = (uint8_t)(step_index % 32u);
  switch (step_index) {
  case 4u:
  case 11u:
  case 12u:
  case 20u:
  case 27u:
  case 28u:
    return 1;
  default:
    return 0;
  }
}

static int is_hihat_step(uint8_t step_index) {
  if (step_index >= 56u) {
    /* Keep hats busy in the fill section. */
    return 1;
  }

  step_index = (uint8_t)(step_index % 32u);
  switch (step_index) {
  case 0u:
  case 2u:
  case 4u:
  case 6u:
  case 7u:
  case 9u:
  case 10u:
  case 12u:
  case 14u:
  case 16u:
  case 18u:
  case 20u:
  case 22u:
  case 23u:
  case 25u:
  case 26u:
  case 28u:
  case 30u:
  case 31u:
    return 1;
  default:
    return 0;
  }
}

static int is_any_step(uint8_t step_index) {
  return step_index == 1u || step_index == 2u || step_index == 6u || step_index == 8u ||
         step_index == 10u || step_index == 14u;
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

static int contains_case_insensitive(const char *haystack, const char *needle) {
  size_t needle_len = strlen(needle);
  if (needle_len == 0u)
    return 1;

  for (size_t i = 0u; haystack[i] != '\0'; ++i) {
    size_t j = 0u;
    while (j < needle_len && haystack[i + j] != '\0' &&
           toupper((unsigned char)haystack[i + j]) == toupper((unsigned char)needle[j])) {
      ++j;
    }
    if (j == needle_len)
      return 1;
  }
  return 0;
}

static int name_looks_like_kick(const char *name) {
  if (!has_suffix(name, ".mtdrum"))
    return 0;
  if (name[0] == '.' || (name[0] == '_' && name[1] == '.'))
    return 0;
  if (contains_case_insensitive(name, " BD "))
    return 1;
  if (contains_case_insensitive(name, "KICK"))
    return 1;
  return 0;
}

static int name_looks_like_snare(const char *name) {
  if (!has_suffix(name, ".mtdrum"))
    return 0;
  if (name[0] == '.' || (name[0] == '_' && name[1] == '.'))
    return 0;
  if (contains_case_insensitive(name, " SD "))
    return 1;
  if (contains_case_insensitive(name, "SNARE"))
    return 1;
  return 0;
}

static int name_looks_like_hihat(const char *name) {
  if (!has_suffix(name, ".mtdrum"))
    return 0;
  if (name[0] == '.' || (name[0] == '_' && name[1] == '.'))
    return 0;
  if (contains_case_insensitive(name, " HH "))
    return 1;
  if (contains_case_insensitive(name, " CH "))
    return 1;
  if (contains_case_insensitive(name, " OH "))
    return 1;
  if (contains_case_insensitive(name, "HAT"))
    return 1;
  return 0;
}

static int name_looks_like_any(const char *name) {
  if (!has_suffix(name, ".mtdrum"))
    return 0;
  if (name[0] == '.' || (name[0] == '_' && name[1] == '.'))
    return 0;
  return 1;
}

typedef int (*patch_name_match_fn)(const char *name);

static int pick_random_patch(char *out_path, size_t out_path_capacity, patch_name_match_fn matcher,
                             const char *exclude_full_path) {
  DIR *dir = opendir(DEMO_PATCH_DIR);
  struct dirent *entry;
  size_t seen = 0u;
  char chosen[DEMO_PATH_MAX];

  if (dir == NULL)
    return 0;

  chosen[0] = '\0';
  while ((entry = readdir(dir)) != NULL) {
    int pick_this = 0;
    char candidate[DEMO_PATH_MAX];
    if (!matcher(entry->d_name))
      continue;
    (void)snprintf(candidate, sizeof(candidate), "%s/%s", DEMO_PATCH_DIR, entry->d_name);
    if (exclude_full_path != NULL && strcmp(candidate, exclude_full_path) == 0)
      continue;
    seen++;
    pick_this = (rand() % (int)seen) == 0;
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

static int pick_random_kick_patch(char *out_path, size_t out_path_capacity) {
  return pick_random_patch(out_path, out_path_capacity, name_looks_like_kick, NULL);
}

static int pick_random_snare_patch(char *out_path, size_t out_path_capacity,
                                   const char *exclude_full_path) {
  return pick_random_patch(out_path, out_path_capacity, name_looks_like_snare, exclude_full_path);
}

static int pick_random_hihat_patch(char *out_path, size_t out_path_capacity,
                                   const char *exclude_full_path) {
  return pick_random_patch(out_path, out_path_capacity, name_looks_like_hihat, exclude_full_path);
}

static int pick_random_any_patch(char *out_path, size_t out_path_capacity,
                                 const char *exclude_full_path) {
  return pick_random_patch(out_path, out_path_capacity, name_looks_like_any, exclude_full_path);
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

static float hh_morph_for_step(uint8_t step_index) {
  uint8_t phase = (uint8_t)(step_index % 8u);
  if (phase <= 4u)
    return (float)phase / 4.0f;
  return (float)(8u - phase) / 4.0f;
}

static float any_morph_for_step(uint8_t step_index) {
  uint8_t phase = (uint8_t)(step_index % 8u);
  return (float)phase / 7.0f;
}

static float lerp01(float a, float b, float t) {
  return a + (b - a) * t;
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
  const char *kick_patch_arg = NULL;
  const char *snare_patch_arg = NULL;
  const char *hh_patch_a_arg = NULL;
  const char *hh_patch_b_arg = NULL;
  const char *any_patch_a_arg = NULL;
  const char *any_patch_b_arg = NULL;

  char random_kick_patch_path[DEMO_PATH_MAX];
  char random_snare_patch_path[DEMO_PATH_MAX];
  char random_hh_patch_a_path[DEMO_PATH_MAX];
  char random_hh_patch_b_path[DEMO_PATH_MAX];
  char random_any_patch_a_path[DEMO_PATH_MAX];
  char random_any_patch_b_path[DEMO_PATH_MAX];

  int using_random_kick_patch = 0;
  int using_random_snare_patch = 0;
  int using_random_hh_patch_a = 0;
  int using_random_hh_patch_b = 0;
  int using_random_any_patch_a = 0;
  int using_random_any_patch_b = 0;

  po32_synth_t synth;
  po32_patch_params_t kick;
  po32_patch_params_t snare;
  po32_patch_params_t hh_a;
  po32_patch_params_t hh_b;
  po32_patch_params_t any_a;
  po32_patch_params_t any_b;
  po32_patch_params_t hh_step_patch;
  po32_patch_params_t any_step_patch;
  po32_status_t status;

  float step_seconds = (60.0f / DEMO_BPM) / 4.0f;
  size_t step_samples = (size_t)(step_seconds * (float)DEMO_SAMPLE_RATE + 0.5f);
  size_t total_samples = step_samples * DEMO_TOTAL_STEPS;
  size_t hit_capacity;
  size_t hh_capacity;
  size_t any_capacity;
  size_t kick_len = 0u;
  size_t snare_len = 0u;
  size_t hh_len = 0u;
  size_t any_len = 0u;
  size_t longest_hit_len;
  size_t output_len;

  float *kick_hit = NULL;
  float *snare_hit = NULL;
  float *hh_hit = NULL;
  float *any_hit = NULL;
  float *output = NULL;
  float peak = 0.0f;

  srand((unsigned int)time(NULL) ^ (unsigned int)getpid());

  if (argc > 8) {
    fprintf(stderr,
            "usage: %s [output.wav] [kick_patch.mtdrum] [snare_patch.mtdrum] [hh_a.mtdrum] "
            "[hh_b.mtdrum] [any_a.mtdrum] [any_b.mtdrum]\n",
            argv[0]);
    return 1;
  }
  if (argc >= 2)
    wav_path = argv[1];
  if (argc >= 3)
    kick_patch_arg = argv[2];
  if (argc >= 4)
    snare_patch_arg = argv[3];
  if (argc >= 5)
    hh_patch_a_arg = argv[4];
  if (argc >= 6)
    hh_patch_b_arg = argv[5];
  if (argc >= 7)
    any_patch_a_arg = argv[6];
  if (argc >= 8)
    any_patch_b_arg = argv[7];

  po32_synth_init(&synth, DEMO_SAMPLE_RATE);

  if (kick_patch_arg != NULL) {
    if (!load_patch_from_mtdrum(kick_patch_arg, &kick)) {
      fprintf(stderr, "failed to parse kick patch file: %s\n", kick_patch_arg);
      return 1;
    }
  } else if (pick_random_kick_patch(random_kick_patch_path, sizeof(random_kick_patch_path)) &&
             load_patch_from_mtdrum(random_kick_patch_path, &kick)) {
    kick_patch_arg = random_kick_patch_path;
    using_random_kick_patch = 1;
  } else {
    make_kick_patch(&kick);
  }

  if (snare_patch_arg != NULL) {
    if (!load_patch_from_mtdrum(snare_patch_arg, &snare)) {
      fprintf(stderr, "failed to parse snare patch file: %s\n", snare_patch_arg);
      return 1;
    }
  } else if (pick_random_snare_patch(random_snare_patch_path, sizeof(random_snare_patch_path),
                                     kick_patch_arg) &&
             load_patch_from_mtdrum(random_snare_patch_path, &snare)) {
    snare_patch_arg = random_snare_patch_path;
    using_random_snare_patch = 1;
  } else {
    make_snare_patch(&snare);
  }

  if (hh_patch_a_arg != NULL) {
    if (!load_patch_from_mtdrum(hh_patch_a_arg, &hh_a)) {
      fprintf(stderr, "failed to parse hihat A patch file: %s\n", hh_patch_a_arg);
      return 1;
    }
  } else if (pick_random_hihat_patch(random_hh_patch_a_path, sizeof(random_hh_patch_a_path),
                                     NULL) &&
             load_patch_from_mtdrum(random_hh_patch_a_path, &hh_a)) {
    hh_patch_a_arg = random_hh_patch_a_path;
    using_random_hh_patch_a = 1;
  } else {
    make_hh_patch_a(&hh_a);
  }

  if (hh_patch_b_arg != NULL) {
    if (!load_patch_from_mtdrum(hh_patch_b_arg, &hh_b)) {
      fprintf(stderr, "failed to parse hihat B patch file: %s\n", hh_patch_b_arg);
      return 1;
    }
  } else if (pick_random_hihat_patch(random_hh_patch_b_path, sizeof(random_hh_patch_b_path),
                                     hh_patch_a_arg) &&
             load_patch_from_mtdrum(random_hh_patch_b_path, &hh_b)) {
    hh_patch_b_arg = random_hh_patch_b_path;
    using_random_hh_patch_b = 1;
  } else {
    make_hh_patch_b(&hh_b);
  }

  if (any_patch_a_arg != NULL) {
    if (!load_patch_from_mtdrum(any_patch_a_arg, &any_a)) {
      fprintf(stderr, "failed to parse any A patch file: %s\n", any_patch_a_arg);
      return 1;
    }
  } else if (pick_random_any_patch(random_any_patch_a_path, sizeof(random_any_patch_a_path),
                                   NULL) &&
             load_patch_from_mtdrum(random_any_patch_a_path, &any_a)) {
    any_patch_a_arg = random_any_patch_a_path;
    using_random_any_patch_a = 1;
  } else {
    make_any_patch_a(&any_a);
  }

  if (any_patch_b_arg != NULL) {
    if (!load_patch_from_mtdrum(any_patch_b_arg, &any_b)) {
      fprintf(stderr, "failed to parse any B patch file: %s\n", any_patch_b_arg);
      return 1;
    }
  } else if (pick_random_any_patch(random_any_patch_b_path, sizeof(random_any_patch_b_path),
                                   any_patch_a_arg) &&
             load_patch_from_mtdrum(random_any_patch_b_path, &any_b)) {
    any_patch_b_arg = random_any_patch_b_path;
    using_random_any_patch_b = 1;
  } else {
    make_any_patch_b(&any_b);
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
    return 1;
  }

  status = po32_synth_render(&synth, &kick, DEMO_KICK_VELOCITY, DEMO_HIT_SECONDS, kick_hit,
                             hit_capacity, &kick_len);
  if (status != PO32_OK) {
    fprintf(stderr, "failed to render kick hit: %d\n", status);
    free(kick_hit);
    free(snare_hit);
    free(hh_hit);
    free(any_hit);
    return 1;
  }

  status = po32_synth_render(&synth, &snare, DEMO_SNARE_VELOCITY, DEMO_HIT_SECONDS, snare_hit,
                             hit_capacity, &snare_len);
  if (status != PO32_OK) {
    fprintf(stderr, "failed to render snare hit: %d\n", status);
    free(kick_hit);
    free(snare_hit);
    free(hh_hit);
    free(any_hit);
    return 1;
  }

  longest_hit_len = kick_len > snare_len ? kick_len : snare_len;
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
    return 1;
  }

  for (size_t step = 0u; step < DEMO_TOTAL_STEPS; ++step) {
    uint8_t step64 = (uint8_t)(step % DEMO_BASE_STEPS);
    size_t start = step * step_samples;

    if (is_kick_step(step64)) {
      for (size_t i = 0u; i < kick_len && start + i < output_len; ++i)
        output[start + i] += kick_hit[i];
    }

    if (is_snare_step(step64)) {
      for (size_t i = 0u; i < snare_len && start + i < output_len; ++i)
        output[start + i] += snare_hit[i];
    }

    if (is_hihat_step(step64)) {
      float morph = hh_morph_for_step((uint8_t)(step64 % 32u));
      interpolate_patch_params(&hh_a, &hh_b, morph, &hh_step_patch);
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
        return 1;
      }
      for (size_t i = 0u; i < hh_len && start + i < output_len; ++i)
        output[start + i] += hh_hit[i];
    }

    if (is_any_step((uint8_t)(step64 % 16u))) {
      float morph = any_morph_for_step((uint8_t)(step64 % 32u));
      interpolate_patch_params(&any_a, &any_b, morph, &any_step_patch);
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
        return 1;
      }
      for (size_t i = 0u; i < any_len && start + i < output_len; ++i)
        output[start + i] += any_hit[i];
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

  if (!write_wav(wav_path, output, output_len, DEMO_SAMPLE_RATE)) {
    fprintf(stderr, "failed to write wav file: %s\n", wav_path);
    free(output);
    free(kick_hit);
    free(snare_hit);
    free(hh_hit);
    free(any_hit);
    return 1;
  }

  printf("wrote %s\n", wav_path);
  printf("pattern: 64-step jungle pattern with 8-step end fill, repeated 2x (128 total steps) at "
         "%.0f BPM\n",
         DEMO_BPM);
  printf("lanes: kick/snare/hihat jungle grid; steps 57-64 are dense kick+snare fill\n");
  printf("hihat morph: HH A -> HH B over 4 steps, then back over 4 steps (oscillating)\n");
  printf("any morph: ANY A -> ANY B over 8 steps (repeats every 8 steps)\n");

  if (using_random_kick_patch) {
    printf("kick patch: random from %s\n", kick_patch_arg);
  } else if (kick_patch_arg != NULL) {
    printf("kick patch: %s\n", kick_patch_arg);
  } else {
    printf("kick patch: built-in fallback\n");
  }

  if (using_random_snare_patch) {
    printf("snare patch: random from %s\n", snare_patch_arg);
  } else if (snare_patch_arg != NULL) {
    printf("snare patch: %s\n", snare_patch_arg);
  } else {
    printf("snare patch: built-in fallback\n");
  }

  if (using_random_hh_patch_a) {
    printf("hihat A patch: random from %s\n", hh_patch_a_arg);
  } else if (hh_patch_a_arg != NULL) {
    printf("hihat A patch: %s\n", hh_patch_a_arg);
  } else {
    printf("hihat A patch: built-in fallback\n");
  }

  if (using_random_hh_patch_b) {
    printf("hihat B patch: random from %s\n", hh_patch_b_arg);
  } else if (hh_patch_b_arg != NULL) {
    printf("hihat B patch: %s\n", hh_patch_b_arg);
  } else {
    printf("hihat B patch: built-in fallback\n");
  }

  if (using_random_any_patch_a) {
    printf("any A patch: random from %s\n", any_patch_a_arg);
  } else if (any_patch_a_arg != NULL) {
    printf("any A patch: %s\n", any_patch_a_arg);
  } else {
    printf("any A patch: built-in fallback\n");
  }

  if (using_random_any_patch_b) {
    printf("any B patch: random from %s\n", any_patch_b_arg);
  } else if (any_patch_b_arg != NULL) {
    printf("any B patch: %s\n", any_patch_b_arg);
  } else {
    printf("any B patch: built-in fallback\n");
  }

  printf("duration: %.3f s (%zu samples @ %u Hz)\n", (double)output_len / (double)DEMO_SAMPLE_RATE,
         output_len, DEMO_SAMPLE_RATE);

  free(output);
  free(kick_hit);
  free(snare_hit);
  free(hh_hit);
  free(any_hit);
  return 0;
}
