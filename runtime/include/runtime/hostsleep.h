#pragma once

#include <cstdint>

namespace host {

void sleep_us(uint64_t us);
void sleep_until_ns(int64_t steady_ns);

}
