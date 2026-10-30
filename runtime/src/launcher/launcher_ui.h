#pragma once

#include "runtime/launcher.h"

#include <imgui.h>

namespace launcher::ui {

struct Image {
    ImTextureID id = 0;
    int w = 0;
    int h = 0;
};

using Upload = Image (*)(const unsigned char* rgba, int w, int h);

void setup(Upload upload);
bool frame(Result& result);
void shutdown();

}
