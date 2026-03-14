#include "tflm_inference.h"

#include <string.h>
#include <new>

#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

namespace {

constexpr int kTensorArenaSize = 90 * 1024;
alignas(16) static uint8_t g_tensor_arena[kTensorArenaSize];

static const tflite::Model *g_model = nullptr;
static tflite::MicroInterpreter *g_interpreter = nullptr;
static TfLiteTensor *g_input = nullptr;
static TfLiteTensor *g_output = nullptr;
static const unsigned char *g_model_data = nullptr;
static size_t g_model_size = 0;
static char g_last_error[160] = "not initialized";

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

    static tflite::MicroMutableOpResolver<8> resolver;
    static bool resolver_initialized = false;
    if (!resolver_initialized) {
        if (resolver.AddFullyConnected() != kTfLiteOk ||
            resolver.AddRelu() != kTfLiteOk ||
            resolver.AddSoftmax() != kTfLiteOk ||
            resolver.AddReshape() != kTfLiteOk ||
            resolver.AddQuantize() != kTfLiteOk ||
            resolver.AddDequantize() != kTfLiteOk ||
            resolver.AddMul() != kTfLiteOk ||
            resolver.AddAdd() != kTfLiteOk) {
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

    set_error("ok");
    return true;
}

static float softmax_max(const float *vals, int n) {
    if (!vals || n <= 0) {
        return 0.0f;
    }
    float maxv = vals[0];
    for (int i = 1; i < n; i++) {
        if (vals[i] > maxv) {
            maxv = vals[i];
        }
    }
    float sum = 0.0f;
    for (int i = 0; i < n; i++) {
        sum += expf(vals[i] - maxv);
    }
    if (sum <= 0.0f) {
        return 0.0f;
    }
    float best = 0.0f;
    for (int i = 0; i < n; i++) {
        float p = expf(vals[i] - maxv) / sum;
        if (p > best) {
            best = p;
        }
    }
    return best;
}

int tflm_predict_with_confidence(const float *input, size_t len, float *out_confidence) {
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

    if (g_interpreter->Invoke() != kTfLiteOk) {
        set_error("Invoke failed");
        return -1;
    }

    int output_count = 0;
    float confidence = 0.0f;
    int best = -1;

    if (g_output->type == kTfLiteFloat32) {
        output_count = g_output->bytes / static_cast<int>(sizeof(float));
        if (output_count <= 0) {
            set_error("invalid float output size");
            return -1;
        }
        best = argmax_float(g_output->data.f, output_count);
        confidence = softmax_max(g_output->data.f, output_count);
    } else if (g_output->type == kTfLiteInt8) {
        output_count = g_output->bytes;
        if (output_count <= 0) {
            set_error("invalid int8 output size");
            return -1;
        }
        float scale = g_output->params.scale;
        int zero_point = g_output->params.zero_point;
        float tmp[32];
        if (output_count > (int)(sizeof(tmp)/sizeof(tmp[0]))) {
            set_error("output too large");
            return -1;
        }
        for (int i = 0; i < output_count; i++) {
            tmp[i] = (g_output->data.int8[i] - zero_point) * scale;
        }
        best = argmax_float(tmp, output_count);
        confidence = softmax_max(tmp, output_count);
    } else if (g_output->type == kTfLiteUInt8) {
        output_count = g_output->bytes;
        if (output_count <= 0) {
            set_error("invalid uint8 output size");
            return -1;
        }
        float scale = g_output->params.scale;
        int zero_point = g_output->params.zero_point;
        float tmp[32];
        if (output_count > (int)(sizeof(tmp)/sizeof(tmp[0]))) {
            set_error("output too large");
            return -1;
        }
        for (int i = 0; i < output_count; i++) {
            tmp[i] = (g_output->data.uint8[i] - zero_point) * scale;
        }
        best = argmax_float(tmp, output_count);
        confidence = softmax_max(tmp, output_count);
    } else {
        set_error("unsupported output tensor type");
        return -1;
    }

    if (out_confidence) {
        *out_confidence = confidence;
    }

    set_error("ok");
    return best;
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
    set_error("reset");
}

const char *tflm_last_error(void) {
    return g_last_error;
}
