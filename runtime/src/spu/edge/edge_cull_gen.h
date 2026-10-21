#pragma once

#include "edge_cull_ops.h"

namespace edge::xo {

struct Ctl {
    bool fail = false;
};

void f_073e0(V* R, uint8_t* ls, Ctl& ctl);
void f_0d618(V* R, uint8_t* ls, Ctl& ctl);

}
