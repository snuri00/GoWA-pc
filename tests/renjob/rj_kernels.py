KERNELS = {
    0x06cf0: "{ uint32_t d = pref(r4) - pref(r3); uint32_t n = d >> 4; if (!(d & 15) && n >= 2 && n <= 0x1000 && ctl.budget > n && ctl.jn + n < Ctl::kCap) { uint32_t a = pref(r81) + pref(r3); "
             "for (uint32_t k = 0; k + 1 < n; k++) st(ls, a + 16 * k, r7, ctl); r3 = ai(r3, int32_t(16 * (n - 1))); ctl.budget -= n - 1; } }",
    0x06d90: "{ uint32_t n = pref(r12) - pref(r9); uint32_t a8 = pref(r8), tb = pref(r81), base = pref(r11); "
             "if (n >= 2 && n <= 0x10000 && !(a8 & 3) && !(tb & 3) && pref(r10) == 0xffffu && ctl.budget > n && ctl.jn + n < Ctl::kCap) { "
             "for (uint32_t k = 0; k + 1 < n; k++) { uint32_t a = a8 + 4 * k; st32(ls, tb + 4 * ld16(ls, a), base + ld16(ls, a + 2), ctl); } "
             "r8 = ai(r8, int32_t(4 * (n - 1))); r9 = ai(r9, int32_t(n - 1)); ctl.budget -= n - 1; } }",
}
