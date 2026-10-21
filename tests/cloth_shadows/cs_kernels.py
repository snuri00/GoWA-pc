INCLUDES = {
    "cloth": ["cloth_kernels.h"],
    "shadows": [],
}

KERNELS = {
    "cloth": {
        0x09FB8: "kern::fill16(R, ls, ctl);",
        0x05C40: "kern::dist4(R, ls, ctl);",
    },
    "shadows": {},
}
