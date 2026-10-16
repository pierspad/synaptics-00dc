#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <assert.h>
#include "../src/syna_proto.h"
#include "../src/syna_mode2.h"
#include "../src/syna_matcher.h"
#include "../src/syna_align_frames.h"

#define H SYNA_OUTPUT_ROWS  /* 56 */
#define W SYNA_OUTPUT_COLS  /* 144 */
#define N_PIXELS (H * W)

static float calc_rms(const float *frame, const float *baseline, size_t n)
{
    float sum_sq = 0.0f;
    for (size_t i = 0; i < n; i++) {
        float diff = frame[i] - baseline[i];
        sum_sq += diff * diff;
    }
    return sqrtf(sum_sq / (float)n);
}

static float *align_and_average(float frames[][N_PIXELS], int num_frames)
{
    if (num_frames <= 0) return NULL;
    return syna_align_frames(&frames[0][0], (size_t)num_frames, NULL);
}

static int extract_contact_burst(const char *path, int start_frame, float *out_filt, float *baseline)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    uint8_t raw[SYNA_FRAME_BYTES];

    /* If baseline is empty, extract from frames 10..30 */
    float base_norm = 0.0f;
    for (int p = 0; p < N_PIXELS; p++) base_norm += baseline[p] * baseline[p];
    if (base_norm < 1.0f) {
        fseek(f, (long)10 * SYNA_FRAME_BYTES, SEEK_SET);
        int bcnt = 0;
        for (int i = 0; i < 20; i++) {
            if (fread(raw, 1, SYNA_FRAME_BYTES, f) != SYNA_FRAME_BYTES) break;
            if (syna_validate_frame(raw, NULL) == 0) {
                float dec[N_PIXELS];
                syna_decode_frame_unclipped(raw, dec);
                for (int p = 0; p < N_PIXELS; p++) baseline[p] += dec[p];
                bcnt++;
            }
        }
        if (bcnt > 0) {
            for (int p = 0; p < N_PIXELS; p++) baseline[p] /= (float)bcnt;
        }
    }

    fseek(f, (long)start_frame * SYNA_FRAME_BYTES, SEEK_SET);
    float frames[12][N_PIXELS];
    int collected = 0;

    for (int i = 0; i < 30 && collected < 6; i++) {
        if (fread(raw, 1, SYNA_FRAME_BYTES, f) != SYNA_FRAME_BYTES) break;
        if (syna_validate_frame(raw, NULL) == 0) {
            float dec[N_PIXELS];
            syna_decode_frame_unclipped(raw, dec);
            float rms = calc_rms(dec, baseline, N_PIXELS);
            if (rms > 40.0f) {
                for (int p = 0; p < N_PIXELS; p++) {
                    frames[collected][p] = dec[p] - baseline[p];
                }
                collected++;
            }
        }
    }
    fclose(f);

    if (collected < 5) return -2;

    float *avg = align_and_average(frames, collected);
    int rc = syna_bandpass_filter(avg, out_filt);
    free(avg);
    return rc;
}

int main(void)
{
    printf("\n=== Verification of Real Captures Separation: Genuines vs Impostors ===\n");

    const char *p7 = getenv("SYNA_TEST_CAPTURE7");
    const char *p8 = getenv("SYNA_TEST_CAPTURE8");
    if ((p7 == NULL) != (p8 == NULL)) {
        fprintf(stderr, "Set both SYNA_TEST_CAPTURE7 and SYNA_TEST_CAPTURE8, or neither.\n");
        return 1;
    }
    if (!p7) {
        printf("SYNTHETIC regression only: no measured biometric FAR/FRR.\n");
        float f1_touch1[N_PIXELS], f1_touch2[N_PIXELS], f2_impostor[N_PIXELS];
        for (int r = 0; r < H; r++) {
            for (int c = 0; c < W; c++) {
                float x = (float)c - 72.0f;
                float y = (float)r - 28.0f;
                f1_touch1[r * W + c] = sinf(0.85f * x + 0.22f * y + 0.015f * x * y) +
                                       0.6f * cosf(0.32f * x - 0.78f * y);
                /* Shifted by (+8 px, -11 px) and rotated by +7 degrees */
                float rad = 7.0f * ((float)M_PI / 180.0f);
                float xr = (x - 8.0f) * cosf(rad) - (y + 11.0f) * sinf(rad);
                float yr = (x - 8.0f) * sinf(rad) + (y + 11.0f) * cosf(rad);
                f1_touch2[r * W + c] = sinf(0.85f * xr + 0.22f * yr + 0.015f * xr * yr) +
                                       0.6f * cosf(0.32f * xr - 0.78f * yr);
                /* Distinct impostor ridge pattern */
                f2_impostor[r * W + c] = sinf(0.75f * y - 0.45f * x + 0.02f * x * x) +
                                         0.7f * cosf(0.92f * x + 0.55f * y);
            }
        }
        float filt_1a[N_PIXELS], filt_1b[N_PIXELS], filt_2[N_PIXELS];
        assert(syna_bandpass_filter(f1_touch1, filt_1a) == 0);
        assert(syna_bandpass_filter(f1_touch2, filt_1b) == 0);
        assert(syna_bandpass_filter(f2_impostor, filt_2) == 0);
        SynaMatchResult r_gen, r_imp;
        assert(syna_match_images(filt_1a, filt_1b, 0.45f, &r_gen) == 0);
        assert(syna_match_images(filt_1a, filt_2, 0.45f, &r_imp) == 0);
        printf("  Synthetic Genuine score:  %.4f (angle: %.1f°, ov: %.1f%%)\n",
               r_gen.score, r_gen.angle, r_gen.overlap * 100.0f);
        printf("  Synthetic Impostor score: %.4f (angle: %.1f°, ov: %.1f%%)\n",
               r_imp.score, r_imp.angle, r_imp.overlap * 100.0f);
        assert(r_gen.score >= 0.70f);
        assert(r_imp.score <= 0.35f);
        return 0;
    }
    printf("PRIVATE fixture regression: two fingers only, not population FAR/FRR.\n");

    float baseline_7[N_PIXELS] = { 0 };
    float baseline_8[N_PIXELS] = { 0 };

    float t7_c1[N_PIXELS], t7_c2[N_PIXELS];
    float t8_c1[N_PIXELS], t8_c2[N_PIXELS];

    /* Trial 7 (Right Index - Genuine): contact 1 at frame 940, contact 2 at frame 1745 */
    assert(extract_contact_burst(p7, 940, t7_c1, baseline_7) == 0);
    assert(extract_contact_burst(p7, 1745, t7_c2, baseline_7) == 0);

    /* Trial 8 (Left Index - Impostor): contact 1 at frame 535, contact 2 at frame 1190 */
    assert(extract_contact_burst(p8, 535, t8_c1, baseline_8) == 0);
    assert(extract_contact_burst(p8, 1190, t8_c2, baseline_8) == 0);

    SynaMatchResult res;

    /* 1. Genuine Matches */
    printf("\n--- Genuine Attempts (Same Finger, Independent Touch Placements) ---\n");
    assert(syna_match_images(t7_c1, t7_c2, 0.45f, &res) == 0);
    printf("  [GENUINE] t7_c1 vs t7_c2 (Right Index): score = %.4f (angle: %5.1f°, ov: %.1f%%)\n",
           res.score, res.angle, res.overlap * 100.0f);
    assert(res.score >= 0.70f);

    assert(syna_match_images(t8_c1, t8_c2, 0.45f, &res) == 0);
    printf("  [GENUINE] t8_c1 vs t8_c2 (Left Index):  score = %.4f (angle: %5.1f°, ov: %.1f%%)\n",
           res.score, res.angle, res.overlap * 100.0f);
    assert(res.score >= 0.70f);

    /* 2. Impostor Matches */
    printf("\n--- Impostor Attempts (Cross-Finger: Left Index vs Right Index) ---\n");
    assert(syna_match_images(t7_c1, t8_c1, 0.45f, &res) == 0);
    printf("  [IMPOSTOR] t7_c1 (Right) vs t8_c1 (Left): score = %.4f (angle: %5.1f°, ov: %.1f%%)\n",
           res.score, res.angle, res.overlap * 100.0f);
    assert(res.score <= 0.35f);

    assert(syna_match_images(t7_c1, t8_c2, 0.45f, &res) == 0);
    printf("  [IMPOSTOR] t7_c1 (Right) vs t8_c2 (Left): score = %.4f (angle: %5.1f°, ov: %.1f%%)\n",
           res.score, res.angle, res.overlap * 100.0f);
    assert(res.score <= 0.35f);

    assert(syna_match_images(t7_c2, t8_c1, 0.45f, &res) == 0);
    printf("  [IMPOSTOR] t7_c2 (Right) vs t8_c1 (Left): score = %.4f (angle: %5.1f°, ov: %.1f%%)\n",
           res.score, res.angle, res.overlap * 100.0f);
    assert(res.score <= 0.35f);

    assert(syna_match_images(t7_c2, t8_c2, 0.45f, &res) == 0);
    printf("  [IMPOSTOR] t7_c2 (Right) vs t8_c2 (Left): score = %.4f (angle: %5.1f°, ov: %.1f%%)\n",
           res.score, res.angle, res.overlap * 100.0f);
    assert(res.score <= 0.35f);

    printf("\nFixture assertions passed; these few comparisons do not estimate population FAR/FRR.\n");
    return 0;
}
