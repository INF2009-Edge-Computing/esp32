#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool tflm_load_model(const unsigned char *model_data, size_t model_size);
int tflm_predict(const float *input, size_t len);
int tflm_predict_with_confidence(const float *input, size_t len, float *out_confidence);
int tflm_predict_with_probs(const float *input,
                            size_t len,
                            float *out_probs,
                            size_t probs_len,
                            float *out_confidence);
size_t tflm_input_element_count(void);
void tflm_reset(void);
typedef struct {
    uint32_t last_us;
    uint32_t avg_us;
    uint32_t min_us;
    uint32_t max_us;
    uint32_t sample_count;
} tflm_inference_profile_t;

bool tflm_get_inference_profile(tflm_inference_profile_t *profile);

const char *tflm_last_error(void);

#ifdef __cplusplus
}
#endif
