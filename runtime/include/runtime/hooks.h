#pragma once

#include <cstdint>
#include <string>

namespace hooks {

void init();
const std::string& name_of(uint32_t addr);

}
