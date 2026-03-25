#include "tflm_inference.h"

#include <string.h>
#include <new>
#include <math.h>
#include <limits.h>

extern "C" {
#include "esp_timer.h"
}

#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

namespace {

// The tensor arena is the largest fixed RAM allocation in this firmware.
// Lowering it reduces heap pressure at the cost of limiting model complexity.
// 48 KB has proven sufficient for the current dense model while freeing more
// heap for HTTP model download buffers on ESP32-C3.
constexpr int kTensorArenaSize = 48 * 1024;
alignas(16) static uint8_t g_tensor_arena[kTensorArenaSize];

static const tflite::Model *g_model = nullptr;
static tflite::MicroInterpreter *g_interpreter = nullptr;
static TfLiteTensor *g_input = nullptr;
static TfLiteTensor *g_output = nullptr;
static const unsigned char *g_model_data = nullptr;
static size_t g_model_size = 0;
static char g_last_error[160] = "not initialized";
static uint32_t g_last_invoke_us = 0;
static uint32_t g_min_invoke_us = UINT32_MAX;
static uint32_t g_max_invoke_us = 0;
static uint64_t g_total_invoke_us = 0;
static uint32_t g_invoke_sample_count = 0;

static void update_invoke_profile(uint32_t invoke_us) {
    g_last_invoke_us = invoke_us;
    if (invoke_us < g_min_invoke_us) {
        g_min_invoke_us = invoke_us;
    }
    if (invoke_us > g_max_invoke_us) {
        g_max_invoke_us = invoke_us;
    }
    g_total_invoke_us += invoke_us;
    g_invoke_sample_count++;
}

static void reset_invoke_profile_internal(void) {
    g_last_invoke_us = 0;
    g_min_invoke_us = UINT32_MAX;
    g_max_invoke_us = 0;
    g_total_invoke_us = 0;
    g_invoke_sample_count = 0;
}

static void set_error(const char *msg) {
    if (!msg) {
        g_last_error[0] = '\0';
        return;
    }
    strncpy(g_last_error, msg, sizeof(g_last_error) - 1);
    g_last_error[sizeof(g_last_error) - 1] = '\0';
}

static int argmax_float(const float *vals, int n) {
    if (!vals || n <= 0) {
        return -1;
    }
    int best = 0;
    float best_val = vals[0];
    for (int i = 1; i < n; i++) {
        if (vals[i] > best_val) {
            best_val = vals[i];
            best = i;
        }
    }
    return best;
}

static void softmax_to_probs(const float *vals, int n, float *out_probs) {
    if (!vals || !out_probs || n <= 0) {
        return;
    }

    float maxv = vals[0];
    for (int i = 1; i < n; i++) {
        if (vals[i] > maxv) {
            maxv = vals[i];
        }
    }

    float sum = 0.0f;
    for (int i = 0; i < n; i++) {
        float e = expf(vals[i] - maxv);
        out_probs[i] = e;
        sum += e;
    }

    if (sum <= 0.0f) {
        float uniform = 1.0f / static_cast<float>(n);
        for (int i = 0; i < n; i++) {
            out_probs[i] = uniform;
        }
        return;
    }

    for (int i = 0; i < n; i++) {
        out_probs[i] /= sum;
    }
}

static void renormalize_probs(float *vals, int n) {
    if (!vals || n <= 0) {
        return;
    }

    float sum = 0.0f;
    for (int i = 0; i < n; i++) {
        if (vals[i] < 0.0f) {
            vals[i] = 0.0f;
        }
        sum += vals[i];
    }

    if (sum <= 0.0f) {
        float uniform = 1.0f / static_cast<float>(n);
        for (int i = 0; i < n; i++) {
            vals[i] = uniform;
        }
        return;
    }

    for (int i = 0; i < n; i++) {
        vals[i] /= sum;
    }
}

static bool looks_like_probs(const float *vals, int n) {
    if (!vals || n <= 0) {
        return false;
    }

    float sum = 0.0f;
    for (int i = 0; i < n; i++) {
        if (vals[i] < -0.01f || vals[i] > 1.01f) {
            return false;
        }
        sum += vals[i];
    }

    return fabsf(sum - 1.0f) <= 0.12f;
}

static int read_output_tensor(float *out_vals, int max_vals) {
    if (!g_output || !out_vals || max_vals <= 0) {
        return -1;
    }

    int output_count = 0;
    if (g_output->type == kTfLiteFloat32) {
        output_count = g_output->bytes / static_cast<int>(sizeof(float));
        if (output_count <= 0 || output_count > max_vals) {
            return -1;
        }
        for (int i = 0; i < output_count; i++) {
            out_vals[i] = g_output->data.f[i];
        }
        return output_count;
    }

    if (g_output->type == kTfLiteInt8) {
        output_count = g_output->bytes;
        if (output_count <= 0 || output_count > max_vals) {
            return -1;
        }
        float scale = g_output->params.scale;
        int zero_point = g_output->params.zero_point;
        for (int i = 0; i < output_count; i++) {
            out_vals[i] = (g_output->data.int8[i] - zero_point) * scale;
        }
        return output_count;
    }

    if (g_output->type == kTfLiteUInt8) {
        output_count = g_output->bytes;
        if (output_count <= 0 || output_count > max_vals) {
            return -1;
        }
        float scale = g_output->params.scale;
        int zero_point = g_output->params.zero_point;
        for (int i = 0; i < output_count; i++) {
            out_vals[i] = (g_output->data.uint8[i] - zero_point) * scale;
        }
        return output_count;
    }

    return -1;
}

} // namespace

bool tflm_load_model(const unsigned char *model_data, size_t model_size) {
    if (!model_data || model_size == 0) {
        set_error("empty model data");
        return false;
    }

    g_model_data = model_data;
    g_model_size = model_size;

    g_model = tflite::GetModel(g_model_data);
    if (!g_model) {
        set_error("GetModel returned null");
        return false;
    }

    if (g_model->version() != TFLITE_SCHEMA_VERSION) {
        set_error("schema version mismatch");
        return false;
    }

    static tflite::MicroMutableOpResolver<13> resolver;
    static bool resolver_initialized = false;
    if (!resolver_initialized) {
        if (resolver.AddFullyConnected() != kTfLiteOk ||
            resolver.AddRelu() != kTfLiteOk ||
            resolver.AddSoftmax() != kTfLiteOk ||
            resolver.AddReshape() != kTfLiteOk ||
            resolver.AddQuantize() != kTfLiteOk ||
            resolver.AddDequantize() != kTfLiteOk ||
            resolver.AddMul() != kTfLiteOk ||
            resolver.AddAdd() != kTfLiteOk ||
            resolver.AddConv2D() != kTfLiteOk ||
            resolver.AddMaxPool2D() != kTfLiteOk ||
            resolver.AddMean() != kTfLiteOk ||
            resolver.AddExpandDims() != kTfLiteOk ||
            resolver.AddSqueeze() != kTfLiteOk) {
            set_error("resolver op registration failed");
            return false;
        }
        resolver_initialized = true;
    }

    if (g_interpreter) {
        delete g_interpreter;
        g_interpreter = nullptr;
    }

    g_interpreter = new (std::nothrow)
        tflite::MicroInterpreter(g_model, resolver, g_tensor_arena, kTensorArenaSize);
    if (!g_interpreter) {
        set_error("failed to allocate MicroInterpreter");
        return false;
    }

    if (g_interpreter->AllocateTensors() != kTfLiteOk) {
        set_error("AllocateTensors failed");
        return false;
    }

    g_input = g_interpreter->input(0);
    g_output = g_interpreter->output(0);
    if (!g_input || !g_output) {
        set_error("input/output tensor missing");
        return false;
    }

    reset_invoke_profile_internal();

    set_error("ok");
    return true;
}

int tflm_predict(const float *input, size_t len) {
    return tflm_predict_with_confidence(input, len, nullptr);
}

int tflm_predict_with_probs(const float *input,
                           size_t len,
                           float *out_probs,
                           size_t probs_len,
                           float *out_confidence) {
    if (!g_interpreter || !g_input || !g_output) {
        set_error("interpreter not initialized");
        return -1;
    }
    if (!input || len == 0) {
        set_error("empty input");
        return -1;
    }

    // Populate input tensor
    int input_count = 0;
    if (g_input->type == kTfLiteFloat32) {
        input_count = g_input->bytes / static_cast<int>(sizeof(float));
        if (input_count <= 0) {
            set_error("invalid float input tensor size");
            return -1;
        }

        int copy_n = static_cast<int>(len < static_cast<size_t>(input_count) ? len : input_count);
        for (int i = 0; i < copy_n; i++) {
            g_input->data.f[i] = input[i];
        }
        for (int i = copy_n; i < input_count; i++) {
            g_input->data.f[i] = 0.0f;
        }
    } else if (g_input->type == kTfLiteInt8) {
        input_count = g_input->bytes;
        if (input_count <= 0) {
            set_error("invalid int8 input tensor size");
            return -1;
        }

        float scale = g_input->params.scale;
        int zero_point = g_input->params.zero_point;
        if (scale == 0.0f) {
            set_error("invalid int8 input scale");
            return -1;
        }

        int copy_n = static_cast<int>(len < static_cast<size_t>(input_count) ? len : input_count);
        for (int i = 0; i < copy_n; i++) {
            int q = static_cast<int>(input[i] / scale) + zero_point;
            if (q > 127) q = 127;
            if (q < -128) q = -128;
            g_input->data.int8[i] = static_cast<int8_t>(q);
        }
        for (int i = copy_n; i < input_count; i++) {
            g_input->data.int8[i] = static_cast<int8_t>(zero_point);
        }
    } else {
        set_error("unsupported input tensor type");
        return -1;
    }

    int64_t invoke_start_us = esp_timer_get_time();
    TfLiteStatus invoke_status = g_interpreter->Invoke();
    int64_t invoke_end_us = esp_timer_get_time();
    uint32_t invoke_us = 0;
    if (invoke_end_us > invoke_start_us) {
        int64_t delta = invoke_end_us - invoke_start_us;
        invoke_us = static_cast<uint32_t>(delta > static_cast<int64_t>(UINT32_MAX)
                                              ? UINT32_MAX
                                              : delta);
    }
    update_invoke_profile(invoke_us);

    if (invoke_status != kTfLiteOk) {
        set_error("Invoke failed");
        return -1;
    }

    float logits_or_probs[32] = {0.0f};
    int output_count = read_output_tensor(logits_or_probs,
                                          static_cast<int>(sizeof(logits_or_probs) / sizeof(logits_or_probs[0])));
    if (output_count <= 0) {
        set_error("unsupported or invalid output tensor");
        return -1;
    }

    float probs[32] = {0.0f};
    if (looks_like_probs(logits_or_probs, output_count)) {
        for (int i = 0; i < output_count; i++) {
            probs[i] = logits_or_probs[i];
        }
        renormalize_probs(probs, output_count);
    } else {
        softmax_to_probs(logits_or_probs, output_count, probs);
    }

    int best = argmax_float(probs, output_count);
    if (best < 0) {
        set_error("failed to compute prediction");
        return -1;
    }

    float confidence = probs[best];
    if (out_confidence) {
        *out_confidence = confidence;
    }

    if (out_probs && probs_len > 0) {
        size_t copy_n = probs_len < static_cast<size_t>(output_count)
                            ? probs_len
                            : static_cast<size_t>(output_count);
        for (size_t i = 0; i < copy_n; i++) {
            out_probs[i] = probs[i];
        }
        for (size_t i = copy_n; i < probs_len; i++) {
            out_probs[i] = 0.0f;
        }
    }

    set_error("ok");
    return best;
}

int tflm_predict_with_confidence(const float *input, size_t len, float *out_confidence) {
    return tflm_predict_with_probs(input, len, nullptr, 0, out_confidence);
}

size_t tflm_input_element_count(void) {
    if (!g_input) {
        return 0;
    }

    if (g_input->type == kTfLiteFloat32) {
        if (g_input->bytes <= 0) {
            return 0;
        }
        return static_cast<size_t>(g_input->bytes / static_cast<int>(sizeof(float)));
    }

    if (g_input->type == kTfLiteInt8 || g_input->type == kTfLiteUInt8) {
        if (g_input->bytes <= 0) {
            return 0;
        }
        return static_cast<size_t>(g_input->bytes);
    }

    return 0;
}

bool tflm_get_inference_profile(tflm_inference_profile_t *out_profile) {
    if (!out_profile) {
        return false;
    }

    out_profile->last_us = g_last_invoke_us;
    out_profile->sample_count = g_invoke_sample_count;

    if (g_invoke_sample_count == 0) {
        out_profile->min_us = 0;
        out_profile->max_us = 0;
        out_profile->avg_us = 0;
        return false;
    }

    out_profile->min_us = (g_min_invoke_us == UINT32_MAX) ? 0 : g_min_invoke_us;
    out_profile->max_us = g_max_invoke_us;
    out_profile->avg_us = static_cast<uint32_t>(g_total_invoke_us / g_invoke_sample_count);
    return true;
}

void tflm_reset_inference_profile(void) {
    reset_invoke_profile_internal();
}

void tflm_reset(void) {
    if (g_interpreter) {
        delete g_interpreter;
        g_interpreter = nullptr;
    }
    g_model = nullptr;
    g_input = nullptr;
    g_output = nullptr;
    g_model_data = nullptr;
    g_model_size = 0;
    reset_invoke_profile_internal();
    set_error("reset");
}

const char *tflm_last_error(void) {
    return g_last_error;
}
