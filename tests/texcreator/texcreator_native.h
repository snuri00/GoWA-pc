#pragma once

#include <cstdint>

namespace texcreator {

constexpr uint32_t kParamSize = 0x140;
constexpr uint32_t kArgsSize = 0x50;

struct Job {
    uint32_t width = 64;
    uint32_t height = 32;
    uint32_t depth = 16;
    uint32_t rows = 2;
    uint32_t row_start = 0;
    uint32_t out_ea = 0;
    uint32_t param_ea = 0;
};

Job parse_args(const uint8_t* args_be);

uint32_t output_size(const Job& job);

void run_job(uint8_t* param_be, const Job& job, uint8_t* out_be);

float perlin_noise(float x, float y, float z);

void run_volume(uint8_t* param_be, uint32_t width, uint32_t height, uint32_t depth, uint32_t rows_per_job, uint8_t* volume_be);

}
