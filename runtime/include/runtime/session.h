#pragma once

#include <cstdio>
#include <string>

namespace session {

bool begin();
bool active();
const std::string& dir();
FILE* open(const char* name);

}
