#pragma once

namespace launcher {

enum class Result { Play, Quit };

bool wanted(int argc, char** argv);
Result run();

}
