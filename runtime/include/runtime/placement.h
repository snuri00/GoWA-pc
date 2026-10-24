#pragma once

#include <string>
#include <vector>

namespace placement {

void apply(const std::string& role);
std::vector<int> role_lps(const std::string& role);
int logical_cpus();

}
