#include "ops_impl.h"

namespace spu {

namespace {


struct Table {
    Handler h[2048];

    void set(uint32_t opcode, int bits, Handler fn) {
        int shift = 11 - bits;
        for (uint32_t k = 0; k < (1u << shift); k++) h[(opcode << shift) | k] = fn;
    }

    Table() {
        for (auto& e : h) e = op_illegal;
        set(0x8, 4, op_selb); set(0xB, 4, op_shufb); set(0xC, 4, op_mpya); set(0xD, 4, op_fnms); set(0xE, 4, op_fma); set(0xF, 4, op_fms);
        set(0x21, 7, op_ila); set(0x08, 7, op_nop); set(0x09, 7, op_nop);
        set(0x04, 8, op_ori); set(0x05, 8, op_orhi); set(0x06, 8, op_orbi); set(0x0C, 8, op_sfi); set(0x0D, 8, op_sfhi);
        set(0x14, 8, op_andi); set(0x15, 8, op_andhi); set(0x16, 8, op_andbi); set(0x1C, 8, op_ai); set(0x1D, 8, op_ahi);
        set(0x24, 8, op_stqd); set(0x34, 8, op_lqd); set(0x44, 8, op_xori); set(0x45, 8, op_xorhi); set(0x46, 8, op_xorbi);
        set(0x4C, 8, op_cgti); set(0x4D, 8, op_cgthi); set(0x4E, 8, op_cgtbi); set(0x4F, 8, op_hgti);
        set(0x5C, 8, op_clgti); set(0x5D, 8, op_clgthi); set(0x5E, 8, op_clgtbi); set(0x5F, 8, op_hlgti);
        set(0x74, 8, op_mpyi); set(0x75, 8, op_mpyui); set(0x7C, 8, op_ceqi); set(0x7D, 8, op_ceqhi); set(0x7E, 8, op_ceqbi); set(0x7F, 8, op_heqi);
        set(0x040, 9, op_brz); set(0x041, 9, op_stqa); set(0x042, 9, op_brnz); set(0x044, 9, op_brhz); set(0x046, 9, op_brhnz); set(0x047, 9, op_stqr);
        set(0x060, 9, op_bra); set(0x061, 9, op_lqa); set(0x062, 9, op_brasl); set(0x064, 9, op_br); set(0x065, 9, op_fsmbi); set(0x066, 9, op_brsl); set(0x067, 9, op_lqr);
        set(0x081, 9, op_il); set(0x082, 9, op_ilhu); set(0x083, 9, op_ilh); set(0x0C1, 9, op_iohl);
        set(0x1D8, 10, op_cflts); set(0x1D9, 10, op_cfltu); set(0x1DA, 10, op_csflt); set(0x1DB, 10, op_cuflt);
        set(0x000, 11, op_stop); set(0x001, 11, op_nop); set(0x002, 11, op_nop); set(0x003, 11, op_nop); set(0x00C, 11, op_mfspr); set(0x00D, 11, op_rdch); set(0x00F, 11, op_rchcnt);
        set(0x040, 11, op_sf); set(0x041, 11, op_or); set(0x042, 11, op_bg); set(0x048, 11, op_sfh); set(0x049, 11, op_nor); set(0x053, 11, op_absdb);
        set(0x058, 11, op_rot); set(0x059, 11, op_rotm); set(0x05A, 11, op_rotma); set(0x05B, 11, op_shl); set(0x05C, 11, op_roth); set(0x05D, 11, op_rothm); set(0x05E, 11, op_rotmah); set(0x05F, 11, op_shlh);
        set(0x078, 11, op_roti); set(0x079, 11, op_rotmi); set(0x07A, 11, op_rotmai); set(0x07B, 11, op_shli); set(0x07C, 11, op_rothi); set(0x07D, 11, op_rothmi); set(0x07E, 11, op_rotmahi); set(0x07F, 11, op_shlhi);
        set(0x0C0, 11, op_a); set(0x0C1, 11, op_and); set(0x0C2, 11, op_cg); set(0x0C8, 11, op_ah); set(0x0C9, 11, op_nand); set(0x0D3, 11, op_avgb);
        set(0x10C, 11, op_nop); set(0x10D, 11, op_wrch);
        set(0x128, 11, op_biz); set(0x129, 11, op_binz); set(0x12A, 11, op_bihz); set(0x12B, 11, op_bihnz);
        set(0x140, 11, op_stop); set(0x144, 11, op_stqx);
        set(0x1A8, 11, op_bi); set(0x1A9, 11, op_bisl); set(0x1AA, 11, op_iret); set(0x1AB, 11, op_bisled); set(0x1AC, 11, op_nop);
        set(0x1B0, 11, op_gb); set(0x1B1, 11, op_gbh); set(0x1B2, 11, op_gbb); set(0x1B4, 11, op_fsm); set(0x1B5, 11, op_fsmh); set(0x1B6, 11, op_fsmb);
        set(0x1B8, 11, op_frest); set(0x1B9, 11, op_frsqest); set(0x1C4, 11, op_lqx);
        set(0x1CC, 11, op_rotqbybi); set(0x1CD, 11, op_rotqmbybi); set(0x1CF, 11, op_shlqbybi);
        set(0x1D4, 11, op_cbx); set(0x1D5, 11, op_chx); set(0x1D6, 11, op_cwx); set(0x1D7, 11, op_cdx);
        set(0x1D8, 11, op_rotqbi); set(0x1D9, 11, op_rotqmbi); set(0x1DB, 11, op_shlqbi); set(0x1DC, 11, op_rotqby); set(0x1DD, 11, op_rotqmby); set(0x1DF, 11, op_shlqby);
        set(0x1F0, 11, op_orx); set(0x1F4, 11, op_cbd); set(0x1F5, 11, op_chd); set(0x1F6, 11, op_cwd); set(0x1F7, 11, op_cdd);
        set(0x1F8, 11, op_rotqbii); set(0x1F9, 11, op_rotqmbii); set(0x1FB, 11, op_shlqbii); set(0x1FC, 11, op_rotqbyi); set(0x1FD, 11, op_rotqmbyi); set(0x1FF, 11, op_shlqbyi);
        set(0x201, 11, op_nop);
        set(0x240, 11, op_cgt); set(0x241, 11, op_xor); set(0x248, 11, op_cgth); set(0x249, 11, op_eqv); set(0x250, 11, op_cgtb); set(0x253, 11, op_sumb); set(0x258, 11, op_hgt);
        set(0x2A5, 11, op_clz); set(0x2A6, 11, op_xswd); set(0x2AE, 11, op_xshw); set(0x2B4, 11, op_cntb); set(0x2B6, 11, op_xsbh);
        set(0x2C0, 11, op_clgt); set(0x2C1, 11, op_andc); set(0x2C2, 11, op_fcgt); set(0x2C3, 11, op_dfcgt); set(0x2C4, 11, op_fa); set(0x2C5, 11, op_fs); set(0x2C6, 11, op_fm);
        set(0x2C8, 11, op_clgth); set(0x2C9, 11, op_orc); set(0x2CA, 11, op_fcmgt); set(0x2CB, 11, op_dfcmgt); set(0x2CC, 11, op_dfa); set(0x2CD, 11, op_dfs); set(0x2CE, 11, op_dfm);
        set(0x2D0, 11, op_clgtb); set(0x2D8, 11, op_hlgt);
        set(0x340, 11, op_addx); set(0x341, 11, op_sfx); set(0x342, 11, op_cgx); set(0x343, 11, op_bgx); set(0x346, 11, op_mpyhha); set(0x34E, 11, op_mpyhhau);
        set(0x35C, 11, op_dfma); set(0x35D, 11, op_dfms); set(0x35E, 11, op_dfnms); set(0x35F, 11, op_dfnma);
        set(0x398, 11, op_fscrrd); set(0x3B8, 11, op_fesd); set(0x3B9, 11, op_frds); set(0x3BA, 11, op_fscrwr); set(0x3BF, 11, op_dftsv);
        set(0x3C0, 11, op_ceq); set(0x3C2, 11, op_fceq); set(0x3C3, 11, op_dfceq); set(0x3C4, 11, op_mpy); set(0x3C5, 11, op_mpyh); set(0x3C6, 11, op_mpyhh); set(0x3C7, 11, op_mpys);
        set(0x3C8, 11, op_ceqh); set(0x3CA, 11, op_fcmeq); set(0x3CB, 11, op_dfcmeq); set(0x3CC, 11, op_mpyu); set(0x3CE, 11, op_mpyhhu); set(0x3D0, 11, op_ceqb); set(0x3D4, 11, op_fi); set(0x3D8, 11, op_heq);
    }
};

const Table g_table;

}

bool step(Context& c, Host& host) {
    uint32_t pc = c.pc;
    uint32_t w;
    std::memcpy(&w, c.ls + pc, 4);
    w = __builtin_bswap32(w);
    c.pc = (pc + 4) & PC_MASK;
    return g_table.h[w >> 21](c, host, w, pc);
}

void interpret(Context& c, Host& host) {
    while (step(c, host)) {
    }
}

}
