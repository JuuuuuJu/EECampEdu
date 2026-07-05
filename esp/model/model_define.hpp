#pragma once

#include "dl_layer_model.hpp"
#include "dl_layer_conv2d.hpp"
#include "dl_layer_max_pool2d.hpp"
#include "model_offsets.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <string.h>
#include <stdint.h>
#include <limits.h>

using namespace dl;
using namespace layer;

extern const uint8_t *g_model_weights;

#ifndef DISABLE_WEIGHT_RAM_COPY
#define DISABLE_WEIGHT_RAM_COPY 0
#endif

#ifndef ENABLE_LAYER_PROFILING
#define ENABLE_LAYER_PROFILING 1
#endif

// Offset conversion: Ensure pointer arithmetic executes at the 16-bit element level
#define GET_PTR(offset) (((int16_t*)g_model_weights) + (offset))

// Internal-RAM allocation for hot weight matrices to prevent Flash SPI cache
// thrashing. PSRAM is intentionally not used by default: previous benchmarks
// showed correct early layers but wrong deeper Conv output after PSRAM copy.
inline int16_t* copy_weight_to_ram(int offset, int num_elements, const char* label) {
    size_t bytes = (size_t)num_elements * sizeof(int16_t);
    const uint8_t* src = g_model_weights + (size_t)offset * sizeof(int16_t);

#if DISABLE_WEIGHT_RAM_COPY
    ESP_LOGW("MODEL", "Using flash mmap for %-15s (RAM copy disabled)", label);
    return GET_PTR(offset);
#endif

    int16_t* ram_ptr = (int16_t*)heap_caps_aligned_alloc(16, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (ram_ptr != NULL) {
        memcpy(ram_ptr, src, bytes);
        ESP_LOGI("MODEL", "Allocated %-15s: %6u KB (internal RAM)", label, (unsigned)(bytes / 1024));
        return ram_ptr;
    }

    ESP_LOGW("MODEL", "Internal RAM unavailable for %-15s (%u KB). Using flash mmap.",
             label, (unsigned)(bytes / 1024));
    return GET_PTR(offset);
}

#define COPY_TO_RAM(offset, size) copy_weight_to_ram(offset, size, #offset)

#ifndef FLATTEN_CHW_ORDER
#define FLATTEN_CHW_ORDER 0
#endif

#ifndef DEBUG_DUAL_FLATTEN
#define DEBUG_DUAL_FLATTEN 0
#endif

#if FLATTEN_CHW_ORDER
#define FLATTEN_OP_NAME "Flatten_CHW"
#else
#define FLATTEN_OP_NAME "Flatten_HWC"
#endif

inline void dump_tensor_stats(const char* label, Tensor<int16_t> &tensor) {
#if ENABLE_LAYER_PROFILING
    int total = tensor.get_size();
    int16_t* data = tensor.get_element_ptr();

    if (data == NULL || total <= 0) {
        printf("LAYER_DUMP,%s,empty\n", label);
        return;
    }

    int16_t min_value = data[0];
    int16_t max_value = data[0];
    int64_t sum = 0;
    int clipped = 0;

    for (int i = 0; i < total; i++) {
        int16_t value = data[i];
        if (value < min_value) min_value = value;
        if (value > max_value) max_value = value;
        if (value == INT16_MIN || value == INT16_MAX) clipped++;
        sum += value;
    }

    printf("LAYER_DUMP,%s,shape=", label);
    for (size_t i = 0; i < tensor.shape.size(); i++) {
        printf("%s%d", i == 0 ? "" : "x", tensor.shape[i]);
    }
    printf(",exp=%d,min=%d,max=%d,clip=%d,sum=%lld,first=%d\n",
           tensor.exponent, min_value, max_value, clipped, sum, data[0]);
#else
    (void)label;
    (void)tensor;
#endif
}

inline void dump_layer_time(const char* label, const char* op, int64_t elapsed_us) {
#if ENABLE_LAYER_PROFILING
    printf("LAYER_TIME,%s,%s,%lld\n", label, op, elapsed_us);
#else
    (void)label;
    (void)op;
    (void)elapsed_us;
#endif
}

// Tensor transformation before FC-as-Conv2D.
template <typename feature_t>
class Flatten : public Layer
{
private:
    Tensor<feature_t> output;
    bool chw_order;
public:
    Flatten(bool chw_order = false) : chw_order(chw_order) {}

    void build(Tensor<feature_t> &input)
    {
        int total_elements = input.shape[0] * input.shape[1] * input.shape[2];
        this->output.set_element((feature_t *)dl::tool::malloc_aligned(total_elements * sizeof(feature_t), 16))
                    .set_exponent(input.exponent)
                    .set_shape({1, 1, total_elements})
                    .set_auto_free(true);
    }
    void call(Tensor<feature_t> &input)
    {
        feature_t *in_ptr = input.get_element_ptr();
        feature_t *out_ptr = this->output.get_element_ptr();

        if (this->chw_order) {
        int h = input.shape[0];
        int w = input.shape[1];
        int c = input.shape[2];

        for (int _c = 0; _c < c; _c++) {
            for (int _h = 0; _h < h; _h++) {
                for (int _w = 0; _w < w; _w++) {
                    out_ptr[_c * h * w + _h * w + _w] = in_ptr[_h * w * c + _w * c + _c];
                }
            }
        }
        } else {
        int total_elements = input.shape[0] * input.shape[1] * input.shape[2];
        memcpy(out_ptr, in_ptr, total_elements * sizeof(feature_t));
        }
    }
    Tensor<feature_t> &get_output() { return this->output; }
};

class HANDRECOGNITION : public Model<int16_t>
{
private:
    Filter<int16_t> f0, f3, f6, f10, f12;
    Bias<int16_t>   b0, b3, b6, b10, b12;
    Activation<int16_t> act_relu;

    Conv2D<int16_t> l1;
    MaxPool2D<int16_t> l2;
    Conv2D<int16_t> l3;
    MaxPool2D<int16_t> l4;
    Conv2D<int16_t> l5;
    MaxPool2D<int16_t> l6;
    Flatten<int16_t> flatten_hwc_layer;
    Flatten<int16_t> flatten_chw_layer;
    Conv2D<int16_t> l7;

    void copy_head_scores(int16_t *dest, int *pred) {
        int16_t *scores = this->l8.get_output().get_element_ptr();
        int16_t max_score = scores[0];
        int max_index = 0;

        for (int i = 0; i < 10; i++) {
            dest[i] = scores[i];
            if (i > 0 && scores[i] > max_score) {
                max_score = scores[i];
                max_index = i;
            }
        }

        *pred = max_index;
    }

    void dump_head_result(const char *order, int pred, const int16_t *scores) {
        printf("HEAD_RESULT,%s,%d", order, pred);
        for (int i = 0; i < 10; i++) {
            printf(",%d", scores[i]);
        }
        printf("\n");
    }

public:
    Conv2D<int16_t> l8;
    int16_t debug_hwc_scores[10];
    int16_t debug_chw_scores[10];
    int debug_hwc_pred;
    int debug_chw_pred;

    HANDRECOGNITION() :
        f0(COPY_TO_RAM(CONV_0_FILTER_ELEMENT_OFFSET, 800), -16, {5, 5, 1, 32}, {1, 1}),
        f3(COPY_TO_RAM(CONV_3_FILTER_ELEMENT_OFFSET, 18432), -17, {3, 3, 32, 64}, {1, 1}),
        f6(COPY_TO_RAM(CONV_6_FILTER_ELEMENT_OFFSET, 36864), -17, {3, 3, 64, 64}, {1, 1}),
        f10(GET_PTR(GEMM_10_FILTER_ELEMENT_OFFSET), -17, {1, 1, 6400, 128}, {1, 1}),
        f12(GET_PTR(GEMM_12_FILTER_ELEMENT_OFFSET), -17, {1, 1, 128, 10}, {1, 1}),
        
        b0(GET_PTR(CONV_0_BIAS_ELEMENT_OFFSET), -12, {32}),
        b3(GET_PTR(CONV_3_BIAS_ELEMENT_OFFSET), -11, {64}),
        b6(GET_PTR(CONV_6_BIAS_ELEMENT_OFFSET), -8, {64}),
        b10(GET_PTR(GEMM_10_BIAS_ELEMENT_OFFSET), -8, {128}),
        b12(GET_PTR(GEMM_12_BIAS_ELEMENT_OFFSET), -8, {10}),

        act_relu(ReLU),

        l1(Conv2D<int16_t>(-12, &f0, &b0, &act_relu, PADDING_VALID, {}, 1, 1, "l1")),
        l2(MaxPool2D<int16_t>({2, 2}, PADDING_VALID, {}, 2, 2, "l2")),
        l3(Conv2D<int16_t>(-11, &f3, &b3, &act_relu, PADDING_VALID, {}, 1, 1, "l3")),
        l4(MaxPool2D<int16_t>({2, 2}, PADDING_VALID, {}, 2, 2, "l4")),
        l5(Conv2D<int16_t>(-8,  &f6, &b6, &act_relu, PADDING_VALID, {}, 1, 1, "l5")),
        l6(MaxPool2D<int16_t>({2, 2}, PADDING_VALID, {}, 2, 2, "l6")),
        flatten_hwc_layer(false),
        flatten_chw_layer(true),
        l7(Conv2D<int16_t>(-8,  &f10, &b10, &act_relu, PADDING_VALID, {}, 1, 1, "l7")),
        l8(Conv2D<int16_t>(-8,  &f12, &b12, NULL, PADDING_VALID, {}, 1, 1, "l8")),
        debug_hwc_pred(0),
        debug_chw_pred(0)
    {}

    void build(Tensor<int16_t> &input) {
        this->l1.build(input);
        this->l2.build(this->l1.get_output());
        this->l3.build(this->l2.get_output());
        this->l4.build(this->l3.get_output());
        this->l5.build(this->l4.get_output());
        this->l6.build(this->l5.get_output());
        this->flatten_hwc_layer.build(this->l6.get_output());
        this->flatten_chw_layer.build(this->l6.get_output());
        this->l7.build(this->flatten_hwc_layer.get_output());
        this->l8.build(this->l7.get_output());
    }

    void call(Tensor<int16_t> &input) {
        int64_t layer_start = 0;
        int64_t layer_end = 0;

        layer_start = esp_timer_get_time();
        this->l1.call(input);
        layer_end = esp_timer_get_time();
        dump_layer_time("l1", "Conv2D", layer_end - layer_start);
        dump_tensor_stats("l1", this->l1.get_output());

        layer_start = esp_timer_get_time();
        this->l2.call(this->l1.get_output());
        layer_end = esp_timer_get_time();
        dump_layer_time("l2", "MaxPool2D", layer_end - layer_start);
        dump_tensor_stats("l2", this->l2.get_output());

        layer_start = esp_timer_get_time();
        this->l3.call(this->l2.get_output());
        layer_end = esp_timer_get_time();
        dump_layer_time("l3", "Conv2D", layer_end - layer_start);
        dump_tensor_stats("l3", this->l3.get_output());

        layer_start = esp_timer_get_time();
        this->l4.call(this->l3.get_output());
        layer_end = esp_timer_get_time();
        dump_layer_time("l4", "MaxPool2D", layer_end - layer_start);
        dump_tensor_stats("l4", this->l4.get_output());

        layer_start = esp_timer_get_time();
        this->l5.call(this->l4.get_output());
        layer_end = esp_timer_get_time();
        dump_layer_time("l5", "Conv2D", layer_end - layer_start);
        dump_tensor_stats("l5", this->l5.get_output());

        layer_start = esp_timer_get_time();
        this->l6.call(this->l5.get_output());
        layer_end = esp_timer_get_time();
        dump_layer_time("l6", "MaxPool2D", layer_end - layer_start);
        dump_tensor_stats("l6", this->l6.get_output());

#if DEBUG_DUAL_FLATTEN
        layer_start = esp_timer_get_time();
        this->flatten_hwc_layer.call(this->l6.get_output());
        layer_end = esp_timer_get_time();
        dump_layer_time("flatten_hwc", "Flatten_HWC", layer_end - layer_start);
        dump_tensor_stats("flatten_hwc", this->flatten_hwc_layer.get_output());

        layer_start = esp_timer_get_time();
        this->l7.call(this->flatten_hwc_layer.get_output());
        layer_end = esp_timer_get_time();
        dump_layer_time("l7_hwc", "FC_as_Conv2D", layer_end - layer_start);
        dump_tensor_stats("l7_hwc", this->l7.get_output());

        layer_start = esp_timer_get_time();
        this->l8.call(this->l7.get_output());
        layer_end = esp_timer_get_time();
        dump_layer_time("l8_hwc", "FC_as_Conv2D", layer_end - layer_start);
        dump_tensor_stats("l8_hwc", this->l8.get_output());
        copy_head_scores(this->debug_hwc_scores, &this->debug_hwc_pred);
        dump_head_result("HWC", this->debug_hwc_pred, this->debug_hwc_scores);

        layer_start = esp_timer_get_time();
        this->flatten_chw_layer.call(this->l6.get_output());
        layer_end = esp_timer_get_time();
        dump_layer_time("flatten_chw", "Flatten_CHW", layer_end - layer_start);
        dump_tensor_stats("flatten_chw", this->flatten_chw_layer.get_output());

        layer_start = esp_timer_get_time();
        this->l7.call(this->flatten_chw_layer.get_output());
        layer_end = esp_timer_get_time();
        dump_layer_time("l7_chw", "FC_as_Conv2D", layer_end - layer_start);
        dump_tensor_stats("l7_chw", this->l7.get_output());

        layer_start = esp_timer_get_time();
        this->l8.call(this->l7.get_output());
        layer_end = esp_timer_get_time();
        dump_layer_time("l8_chw", "FC_as_Conv2D", layer_end - layer_start);
        dump_tensor_stats("l8_chw", this->l8.get_output());
        copy_head_scores(this->debug_chw_scores, &this->debug_chw_pred);
        dump_head_result("CHW", this->debug_chw_pred, this->debug_chw_scores);

#if FLATTEN_CHW_ORDER
        memcpy(this->l8.get_output().get_element_ptr(), this->debug_chw_scores, sizeof(this->debug_chw_scores));
#else
        memcpy(this->l8.get_output().get_element_ptr(), this->debug_hwc_scores, sizeof(this->debug_hwc_scores));
#endif
#else
        layer_start = esp_timer_get_time();
        #if FLATTEN_CHW_ORDER
        this->flatten_chw_layer.call(this->l6.get_output());
        Tensor<int16_t> &flatten_output = this->flatten_chw_layer.get_output();
        #else
        this->flatten_hwc_layer.call(this->l6.get_output());
        Tensor<int16_t> &flatten_output = this->flatten_hwc_layer.get_output();
        #endif
        layer_end = esp_timer_get_time();
        dump_layer_time("flatten", FLATTEN_OP_NAME, layer_end - layer_start);
        dump_tensor_stats("flatten", flatten_output);

        layer_start = esp_timer_get_time();
        this->l7.call(flatten_output);
        layer_end = esp_timer_get_time();
        dump_layer_time("l7", "FC_as_Conv2D", layer_end - layer_start);
        dump_tensor_stats("l7", this->l7.get_output());

        layer_start = esp_timer_get_time();
        this->l8.call(this->l7.get_output());
        layer_end = esp_timer_get_time();
        dump_layer_time("l8", "FC_as_Conv2D", layer_end - layer_start);
        dump_tensor_stats("l8", this->l8.get_output());
#endif
    }
};
