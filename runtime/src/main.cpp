#include "runtime/image.h"

#include <cstdio>

int main(int argc, char** argv) {
    std::printf("GOWA-pc: %u functions, %u imports, text 0x%08x-0x%08x, toc 0x%08x, entry opd 0x%08x\n",
                image::function_count, image::import_count, image::text_lo, image::text_hi, image::toc, image::entry_opd);
    return 0;
}
