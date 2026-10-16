#include "syna_align_frames.h"
#include "syna_proto.h"

#include <math.h>
#include <stdlib.h>

float *
syna_align_frames (const float *frames, size_t count, float *out_quality)
{
    const int height = SYNA_OUTPUT_ROWS;
    const int width = SYNA_OUTPUT_COLS;
    const size_t pixels = (size_t)height * (size_t)width;
    if (out_quality)
        *out_quality = 0.0f;
    if (!frames || count == 0 || count > SYNA_MAX_ALIGNMENT_FRAMES)
        return NULL;

    float means[SYNA_MAX_ALIGNMENT_FRAMES];
    int anchor_index = 0;
    float max_energy = -1.0f;

    for (size_t i = 0; i < count; i++) {
        const float *frame = frames + i * pixels;
        float sum = 0.0f;
        for (size_t p = 0; p < pixels; p++)
            sum += frame[p];
        means[i] = sum / (float)pixels;

        float energy = 0.0f;
        for (size_t p = 0; p < pixels; p++) {
            float delta = frame[p] - means[i];
            energy += delta * delta;
        }
        if (energy > max_energy) {
            max_energy = energy;
            anchor_index = (int)i;
        }
    }

    if (out_quality)
        *out_quality = max_energy / (float)pixels;

    float anchor_zero[SYNA_OUTPUT_ROWS * SYNA_OUTPUT_COLS];
    const float *anchor = frames + (size_t)anchor_index * pixels;
    for (size_t p = 0; p < pixels; p++)
        anchor_zero[p] = anchor[p] - means[anchor_index];

    int shift_y[SYNA_MAX_ALIGNMENT_FRAMES] = { 0 };
    int shift_x[SYNA_MAX_ALIGNMENT_FRAMES] = { 0 };
    int accepted[SYNA_MAX_ALIGNMENT_FRAMES] = { 0 };
    for (size_t i = 0; i < count; i++) {
        if ((int)i == anchor_index) {
            accepted[i] = 1;
            continue;
        }

        float best_corr = -1.0f;
        int best_dy = 0;
        int best_dx = 0;
        const float *frame = frames + i * pixels;
        for (int dy = -2; dy <= 2; dy++) {
            for (int dx = -2; dx <= 2; dx++) {
                float dot = 0.0f;
                float norm_anchor = 0.0f;
                float norm_frame = 0.0f;
                for (int r = 0; r < height; r++) {
                    int source_row = r + dy;
                    if (source_row < 0 || source_row >= height)
                        continue;
                    for (int c = 0; c < width; c++) {
                        int source_col = c + dx;
                        if (source_col < 0 || source_col >= width)
                            continue;
                        size_t anchor_pixel = (size_t)r * width + c;
                        size_t frame_pixel = (size_t)source_row * width + source_col;
                        float a = anchor_zero[anchor_pixel];
                        float b = frame[frame_pixel] - means[i];
                        dot += a * b;
                        norm_anchor += a * a;
                        norm_frame += b * b;
                    }
                }
                float corr = dot / sqrtf(fmaxf(norm_anchor * norm_frame, 1e-9f));
                if (corr > best_corr) {
                    best_corr = corr;
                    best_dy = dy;
                    best_dx = dx;
                }
            }
        }

        if (best_corr >= 0.55f) {
            shift_y[i] = best_dy;
            shift_x[i] = best_dx;
            accepted[i] = 1;
        }
    }

    float *accum = calloc(pixels, sizeof(*accum));
    int *counts = calloc(pixels, sizeof(*counts));
    float *average = malloc(pixels * sizeof(*average));
    if (!accum || !counts || !average) {
        free(accum);
        free(counts);
        free(average);
        if (out_quality)
            *out_quality = 0.0f;
        return NULL;
    }

    for (size_t i = 0; i < count; i++) {
        if (!accepted[i])
            continue;
        int dy = shift_y[i];
        int dx = shift_x[i];
        const float *frame = frames + i * pixels;
        for (int r = 0; r < height; r++) {
            int source_row = r + dy;
            if (source_row < 0 || source_row >= height)
                continue;
            for (int c = 0; c < width; c++) {
                int source_col = c + dx;
                if (source_col < 0 || source_col >= width)
                    continue;
                size_t pixel = (size_t)r * width + c;
                size_t source_pixel = (size_t)source_row * width + source_col;
                accum[pixel] += frame[source_pixel];
                counts[pixel]++;
            }
        }
    }

    for (size_t p = 0; p < pixels; p++)
        average[p] = counts[p] > 0 ? accum[p] / (float)counts[p] : anchor[p];
    free(accum);
    free(counts);
    return average;
}
