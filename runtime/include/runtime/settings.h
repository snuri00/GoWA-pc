#pragma once

#include <string>
#include <vector>

namespace settings {

enum class Kind { Path, Bool, Int, Float, Choice, Text };

struct Option {
    const char* section;
    const char* key;
    const char* env;
    Kind kind;
    const char* def;
    const char* label;
    const char* help;
    const char* choices;
    bool windows_only = false;
};

const std::vector<Option>& options();
std::string user_dir();
std::string path();
bool load();
bool save();
std::string get(const Option& o);
void set(const Option& o, const std::string& value);
void apply_env();

}
