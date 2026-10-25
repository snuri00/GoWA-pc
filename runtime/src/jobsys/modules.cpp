#include "runtime/jobsys.h"

namespace jobsys {

namespace {

const Registrar r_texcreator({0x730800, "TexCreator", nullptr, nullptr, 0, 0, 0, 0});
const Registrar r_texstream({0x6f7200, "TexStreaming", nullptr, nullptr, 0, 0, 0, kTexTable});
const Registrar r_colorcorr({0x743a00, "ColorCorrection", nullptr, nullptr, 0, 0, 0, 0});
const Registrar r_readpk({0x766280, "ReadPackets", nullptr, nullptr, 0, 0, 0, 0});
const Registrar r_writepk({0x77a580, "WritePackets", nullptr, nullptr, 0, 0, 0, 0});
const Registrar r_sortprim({0x6f4a00, "SortPrim", nullptr, nullptr, 0, 0, 0, 0});
const Registrar r_meshprep({0x74b100, "MeshPrep", nullptr, nullptr, 0, 0, 0, kOptIn});
const Registrar r_waterproc({0x71c500, "WaterProc", nullptr, nullptr, 0, 0, 0, kOptIn});
const Registrar r_cloth({0x728400, "Cloth", nullptr, nullptr, 0, 0, 0, kBuffers | kOptIn});
const Registrar r_stereo({0x7a5c80, "StereoDepth", nullptr, nullptr, 0, 0, 0, kOptIn});

}

}
