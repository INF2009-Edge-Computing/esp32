#pragma once

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool tflm_load_model(const unsigned char *model_data, size_t model_size);
int tflm_predict(const float *input, size_t len);
int tflm_predict_with_confidence(const float *input, size_t len, float *out_confidence);
void tflm_reset(void);
const char *tflm_last_error(void);

#ifdef __cplusplus
}
#endif
