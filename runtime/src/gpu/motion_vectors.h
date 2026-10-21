#pragma once

#include "vk_context.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace gpu::mv {

constexpr uint64_t kCameraVelocityFp = 0xf410397daf071f23ull;

bool enabled();
void set_camera(const float* fc, size_t vec4_count, uint32_t velocity_offset, uint32_t depth_offset);
bool camera(uint32_t& velocity_offset, uint32_t& depth_offset);
bool want_dump(double elapsed);
int run(VkCommandBuffer cmd, VkImageView velocity, VkImageView depth, uint32_t w, uint32_t h, double elapsed);
std::string dump_base(double elapsed);
double elapsed();
void complete();

}
