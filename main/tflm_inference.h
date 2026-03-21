#pragma once

#include <stddef.h>
#include <stdbool.h>

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
const char *tflm_last_error(void);

#ifdef __cplusplus
}
#endif
