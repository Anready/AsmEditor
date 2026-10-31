// Built-in x86 / x86-64 disassembler (NASM syntax): the general-purpose instruction set, the common SSE/SSE2
// instructions and the usual prefixes. Anything else comes out as "db 0x.." (one byte), like ndisasm does.
#include "asmtools.h"

#include <cstdio>

namespace Disasm {
namespace {

const char* kReg64[] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
                        "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"};
const char* kReg32[] = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi",
                        "r8d", "r9d", "r10d", "r11d", "r12d", "r13d", "r14d", "r15d"};
const char* kReg16[] = {"ax", "cx", "dx", "bx", "sp", "bp", "si", "di",
                        "r8w", "r9w", "r10w", "r11w", "r12w", "r13w", "r14w", "r15w"};
const char* kReg8Rex[] = {"al", "cl", "dl", "bl", "spl", "bpl", "sil", "dil",
                          "r8b", "r9b", "r10b", "r11b", "r12b", "r13b", "r14b", "r15b"};
const char* kReg8[] = {"al", "cl", "dl", "bl", "ah", "ch", "dh", "bh"};
const char* kCond[] = {"o", "no", "b", "ae", "e", "ne", "be", "a", "s", "ns", "p", "np", "l", "ge", "le", "g"};
const char* kGroup1[] = {"add", "or", "adc", "sbb", "and", "sub", "xor", "cmp"};
const char* kGroup2[] = {"rol", "ror", "rcl", "rcr", "shl", "shr", "sal", "sar"};

std::string Hex(uint64_t v) {
    char buf[24];
    sprintf_s(buf, "0x%llx", static_cast<unsigned long long>(v));
    return buf;
}

std::string SignedHex(int64_t v) {
    return v < 0 ? "-" + Hex(0 - static_cast<uint64_t>(v)) : Hex(static_cast<uint64_t>(v));
}

const char* SizeName(int size) {
    switch (size) {
    case 1: return "byte";
    case 2: return "word";
    case 4: return "dword";
    case 8: return "qword";
    case 10: return "tword";
    case 16: return "oword";
    }
    return "";
}

std::string Join(const std::string& m, const std::string& a = "", const std::string& b = "", const std::string& c = "");

struct Decoder {
    const uint8_t* p;
    size_t n;
    uint64_t address;
    bool x64;
    size_t i = 0;
    bool ok = true;
    // prefixes
    bool opsize = false, adsize = false, lock = false;
    int rep = 0;  // 0, 0xF2 or 0xF3 (the last one)
    int mandatory = 0;  // the last of 66 / F2 / F3, for SSE
    uint8_t rex = 0;
    // ModRM
    bool modrm = false;
    int mod = 0, reg = 0, rm = 0;
    std::string mem;      // "[...]" (with "{rip}" for a RIP-relative address)
    int64_t ripDisp = 0;
    bool ripRelative = false;

    uint8_t Byte() {
        if (i >= n) {
            ok = false;
            return 0;
        }
        return p[i++];
    }
    int64_t Imm(int size) {
        int64_t v = 0;
        switch (size) {
        case 1: v = static_cast<int8_t>(Byte()); break;
        case 2: v = static_cast<int16_t>(Byte() | (Byte() << 8)); break;
        case 4: {
            uint32_t x = 0;
            for (int k = 0; k < 4; k++) x |= static_cast<uint32_t>(Byte()) << (8 * k);
            v = static_cast<int32_t>(x);
            break;
        }
        case 8: {
            uint64_t x = 0;
            for (int k = 0; k < 8; k++) x |= static_cast<uint64_t>(Byte()) << (8 * k);
            v = static_cast<int64_t>(x);
            break;
        }
        }
        return v;
    }
    bool W() const { return (rex & 8) != 0; }
    int OperandSize() const { return W() ? 8 : opsize ? 2 : 4; }
    int StackSize() const { return x64 ? (opsize ? 2 : 8) : (opsize ? 2 : 4); }  // push, pop, call r/m
    std::string Reg(int size, int num) const {
        switch (size) {
        case 1: return rex || num >= 8 ? kReg8Rex[num] : num < 8 ? kReg8[num] : "?";
        case 2: return kReg16[num];
        case 4: return kReg32[num];
        default: return kReg64[num];
        }
    }
    std::string Xmm(int num) const { return "xmm" + std::to_string(num); }

    void ReadModRM() {
        uint8_t m = Byte();
        modrm = true;
        mod = m >> 6;
        reg = ((m >> 3) & 7) | ((rex & 4) ? 8 : 0);
        rm = m & 7;
        if (mod == 3) {
            rm |= (rex & 1) ? 8 : 0;
            return;
        }
        const char** regs = x64 && !adsize ? kReg64 : kReg32;
        if (!x64 && adsize) {  // 16-bit addressing: not supported
            ok = false;
            return;
        }
        std::string base, index;
        int scale = 1;
        int64_t disp = 0;
        bool hasDisp = false;
        if (rm == 4) {  // SIB
            uint8_t sib = Byte();
            scale = 1 << (sib >> 6);
            int idx = ((sib >> 3) & 7) | ((rex & 2) ? 8 : 0);
            int b = (sib & 7) | ((rex & 1) ? 8 : 0);
            if (idx != 4) index = regs[idx];
            if ((sib & 7) == 5 && mod == 0) {
                disp = Imm(4);
                hasDisp = true;
            } else {
                base = regs[b];
            }
        } else if (rm == 5 && mod == 0) {
            if (x64) {  // RIP-relative: the address is known once the whole instruction is read
                ripDisp = Imm(4);
                ripRelative = true;
                mem = "[rel {rip}]";
                return;
            }
            disp = Imm(4);
            hasDisp = true;
        } else {
            base = regs[rm | ((rex & 1) ? 8 : 0)];
        }
        if (mod == 1) {
            disp = Imm(1);
            hasDisp = true;
        } else if (mod == 2) {
            disp = Imm(4);
            hasDisp = true;
        }
        std::string s = base;
        if (!index.empty()) s += (s.empty() ? "" : "+") + index + (scale > 1 ? "*" + std::to_string(scale) : "");
        if (hasDisp && (disp != 0 || s.empty())) {
            if (s.empty()) s = Hex(x64 ? static_cast<uint64_t>(static_cast<uint32_t>(disp)) : static_cast<uint32_t>(disp));
            else s += disp < 0 ? "-" + Hex(0 - static_cast<uint64_t>(disp)) : "+" + Hex(static_cast<uint64_t>(disp));
        }
        mem = "[" + s + "]";
    }
    std::string E(int size) const {  // register or memory with its size
        if (mod == 3) return Reg(size, rm);
        return std::string(SizeName(size)) + " " + mem;
    }
    std::string EX(int memSize) const {  // xmm or memory (memSize 0: no size keyword)
        if (mod == 3) return Xmm(rm);
        return memSize ? std::string(SizeName(memSize)) + " " + mem : mem;
    }
    std::string G(int size) const { return Reg(size, reg); }
    std::string GX() const { return Xmm(reg); }
    std::string Target(int64_t rel) const {
        uint64_t t = address + i + rel;
        if (!x64) t &= 0xFFFFFFFF;
        return Hex(t);
    }
    std::string Finish(std::string text) const {
        size_t at = text.find("{rip}");
        if (at != std::string::npos) text.replace(at, 5, Hex(address + i + ripDisp));
        return text;
    }

    std::string Decode();
    std::string Decode0F();
    std::string DecodeX87(uint8_t op);
    std::string DecodeVex(uint8_t first);
};

// AVX / AVX2 / FMA with a VEX prefix (C4 / C5): the common moves, arithmetic, logic and fused multiply-add.
std::string Decoder::DecodeVex(uint8_t first) {
    uint8_t b1 = Byte();
    int map = 1, pp, vvvv, L;
    bool w = false;
    if (first == 0xC5) {
        rex = static_cast<uint8_t>((b1 & 0x80) ? 0 : 4);
        vvvv = (~b1 >> 3) & 15;
        L = (b1 >> 2) & 1;
        pp = b1 & 3;
    } else {
        uint8_t b2 = Byte();
        rex = static_cast<uint8_t>(((b1 & 0x80) ? 0 : 4) | ((b1 & 0x40) ? 0 : 2) | ((b1 & 0x20) ? 0 : 1));
        map = b1 & 31;
        w = (b2 & 0x80) != 0;
        vvvv = (~b2 >> 3) & 15;
        L = (b2 >> 2) & 1;
        pp = b2 & 3;
    }
    if (!x64) vvvv &= 7;
    uint8_t op = Byte();
    std::string kind = L ? "ymm" : "xmm";
    auto V = [&](int n) { return kind + std::to_string(n); };
    auto RM = [&]() { return mod == 3 ? V(rm) : mem; };
    if (map == 1 && op == 0x77) return L ? "vzeroall" : "vzeroupper";
    ReadModRM();
    if (!ok) return "";
    std::string sfx = pp == 0 ? "ps" : pp == 1 ? "pd" : pp == 2 ? "ss" : "sd";
    bool scalar = pp >= 2;
    if (map == 1) {
        if (op == 0x10 || op == 0x11) {
            std::string m = std::string("vmov") + (scalar ? sfx : (pp == 1 ? "upd" : "ups"));
            if (scalar && mod == 3) {
                return op == 0x10 ? Join(m, Xmm(reg), Xmm(vvvv), Xmm(rm)) : Join(m, Xmm(rm), Xmm(vvvv), Xmm(reg));
            }
            std::string r = scalar ? Xmm(reg) : V(reg);
            return op == 0x10 ? Join(m, r, scalar ? mem : RM()) : Join(m, scalar ? mem : RM(), r);
        }
        if ((op == 0x28 || op == 0x29) && pp < 2) {
            std::string m = pp == 1 ? "vmovapd" : "vmovaps";
            return op == 0x28 ? Join(m, V(reg), RM()) : Join(m, RM(), V(reg));
        }
        if ((op == 0x6F || op == 0x7F) && (pp == 1 || pp == 2)) {
            std::string m = pp == 1 ? "vmovdqa" : "vmovdqu";
            return op == 0x6F ? Join(m, V(reg), RM()) : Join(m, RM(), V(reg));
        }
        static const std::map<int, std::string> arith = {{0x58, "vadd"}, {0x59, "vmul"}, {0x5C, "vsub"}, {0x5D, "vmin"},
                                                         {0x5E, "vdiv"}, {0x5F, "vmax"}};
        static const std::map<int, std::string> logic = {{0x54, "vand"}, {0x55, "vandn"}, {0x56, "vor"}, {0x57, "vxor"}};
        auto a = arith.find(op);
        if (a != arith.end()) {
            if (scalar) return Join(a->second + sfx, Xmm(reg), Xmm(vvvv), mod == 3 ? Xmm(rm) : mem);
            return Join(a->second + sfx, V(reg), V(vvvv), RM());
        }
        auto l = logic.find(op);
        if (l != logic.end() && pp < 2) return Join(l->second + sfx, V(reg), V(vvvv), RM());
        if (op == 0x51) {
            if (scalar) return Join("vsqrt" + sfx, Xmm(reg), Xmm(vvvv), mod == 3 ? Xmm(rm) : mem);
            return Join("vsqrt" + sfx, V(reg), RM());
        }
        if (pp == 1) {
            static const std::map<int, std::string> packed = {
                {0xEF, "vpxor"}, {0xDB, "vpand"}, {0xDF, "vpandn"}, {0xEB, "vpor"}, {0xFC, "vpaddb"}, {0xFD, "vpaddw"},
                {0xFE, "vpaddd"}, {0xD4, "vpaddq"}, {0xF8, "vpsubb"}, {0xF9, "vpsubw"}, {0xFA, "vpsubd"}, {0xFB, "vpsubq"},
                {0x74, "vpcmpeqb"}, {0x75, "vpcmpeqw"}, {0x76, "vpcmpeqd"}, {0xD5, "vpmullw"}};
            auto p2 = packed.find(op);
            if (p2 != packed.end()) return Join(p2->second, V(reg), V(vvvv), RM());
        }
    }
    if (map == 2 && pp == 1) {
        if (op == 0x18) return Join("vbroadcastss", V(reg), mod == 3 ? Xmm(rm) : mem);
        if (op == 0x58) return Join("vpbroadcastd", V(reg), mod == 3 ? Xmm(rm) : mem);
        if (op == 0x59) return Join("vpbroadcastq", V(reg), mod == 3 ? Xmm(rm) : mem);
        if (op == 0x40) return Join("vpmulld", V(reg), V(vvvv), RM());
        int col = op & 15, row = op >> 4;  // FMA: 9x = 132, Ax = 213, Bx = 231
        if ((row == 9 || row == 0xA || row == 0xB) && col >= 8 && col <= 0xD) {
            static const char* kinds[] = {"vfmadd", "vfmadd", "vfmsub", "vfmsub", "vfnmadd", "vfnmadd"};
            std::string order = row == 9 ? "132" : row == 0xA ? "213" : "231";
            bool isScalar = col & 1;
            std::string t = isScalar ? (w ? "sd" : "ss") : (w ? "pd" : "ps");
            std::string m = std::string(kinds[col - 8]) + order + t;
            if (isScalar) return Join(m, Xmm(reg), Xmm(vvvv), mod == 3 ? Xmm(rm) : mem);
            return Join(m, V(reg), V(vvvv), RM());
        }
    }
    ok = false;
    return "";
}

// x87 (D8–DF): memory forms by the reg field, register forms by the whole ModRM byte.
std::string Decoder::DecodeX87(uint8_t op) {
    ReadModRM();
    if (!ok) return "";
    auto st = [](int i) { return "st" + std::to_string(i); };
    int r = reg & 7, i = rm & 7;
    static const char* arith[] = {"fadd", "fmul", "fcom", "fcomp", "fsub", "fsubr", "fdiv", "fdivr"};
    static const char* intArith[] = {"fiadd", "fimul", "ficom", "ficomp", "fisub", "fisubr", "fidiv", "fidivr"};
    if (mod != 3) {
        switch (op) {
        case 0xD8: return std::string(arith[r]) + " dword " + mem;
        case 0xDC: return std::string(arith[r]) + " qword " + mem;
        case 0xDA: return std::string(intArith[r]) + " dword " + mem;
        case 0xDE: return std::string(intArith[r]) + " word " + mem;
        case 0xD9: {
            static const char* n[] = {"fld dword", "", "fst dword", "fstp dword", "fldenv", "fldcw word", "fnstenv", "fnstcw word"};
            if (!*n[r]) break;
            return std::string(n[r]) + " " + mem;
        }
        case 0xDB: {
            static const char* n[] = {"fild dword", "fisttp dword", "fist dword", "fistp dword", "", "fld tword", "", "fstp tword"};
            if (!*n[r]) break;
            return std::string(n[r]) + " " + mem;
        }
        case 0xDD: {
            static const char* n[] = {"fld qword", "fisttp qword", "fst qword", "fstp qword", "frstor", "", "fnsave", "fnstsw word"};
            if (!*n[r]) break;
            return std::string(n[r]) + " " + mem;
        }
        case 0xDF: {
            static const char* n[] = {"fild word", "fisttp word", "fist word", "fistp word", "fbld tword", "fild qword", "fbstp tword", "fistp qword"};
            return std::string(n[r]) + " " + mem;
        }
        }
        ok = false;
        return "";
    }
    uint8_t m = static_cast<uint8_t>(0xC0 | (r << 3) | i);
    switch (op) {
    case 0xD8:
        if (r == 2 || r == 3) return std::string(arith[r]) + " " + st(i);
        return std::string(arith[r]) + " st0, " + st(i);
    case 0xD9: {
        if (r == 0) return "fld " + st(i);
        if (r == 1) return "fxch " + st(i);
        static const std::map<int, const char*> n = {
            {0xD0, "fnop"}, {0xE0, "fchs"}, {0xE1, "fabs"}, {0xE4, "ftst"}, {0xE5, "fxam"}, {0xE8, "fld1"},
            {0xE9, "fldl2t"}, {0xEA, "fldl2e"}, {0xEB, "fldpi"}, {0xEC, "fldlg2"}, {0xED, "fldln2"}, {0xEE, "fldz"},
            {0xF0, "f2xm1"}, {0xF1, "fyl2x"}, {0xF2, "fptan"}, {0xF3, "fpatan"}, {0xF4, "fxtract"}, {0xF5, "fprem1"},
            {0xF6, "fdecstp"}, {0xF7, "fincstp"}, {0xF8, "fprem"}, {0xF9, "fyl2xp1"}, {0xFA, "fsqrt"}, {0xFB, "fsincos"},
            {0xFC, "frndint"}, {0xFD, "fscale"}, {0xFE, "fsin"}, {0xFF, "fcos"}};
        auto it = n.find(m);
        if (it != n.end()) return it->second;
        break;
    }
    case 0xDA: {
        static const char* n[] = {"fcmovb", "fcmove", "fcmovbe", "fcmovu"};
        if (r < 4) return std::string(n[r]) + " st0, " + st(i);
        if (m == 0xE9) return "fucompp";
        break;
    }
    case 0xDB: {
        static const char* n[] = {"fcmovnb", "fcmovne", "fcmovnbe", "fcmovnu"};
        if (r < 4) return std::string(n[r]) + " st0, " + st(i);
        if (m == 0xE2) return "fnclex";
        if (m == 0xE3) return "fninit";
        if (r == 5) return "fucomi st0, " + st(i);
        if (r == 6) return "fcomi st0, " + st(i);
        break;
    }
    case 0xDC: {
        static const char* n[] = {"fadd", "fmul", "", "", "fsubr", "fsub", "fdivr", "fdiv"};
        if (*n[r]) return std::string(n[r]) + " " + st(i) + ", st0";
        break;
    }
    case 0xDD:
        if (r == 0) return "ffree " + st(i);
        if (r == 2) return "fst " + st(i);
        if (r == 3) return "fstp " + st(i);
        if (r == 4) return "fucom " + st(i);
        if (r == 5) return "fucomp " + st(i);
        break;
    case 0xDE: {
        static const char* n[] = {"faddp", "fmulp", "", "", "fsubrp", "fsubp", "fdivrp", "fdivp"};
        if (m == 0xD9) return "fcompp";
        if (*n[r]) return std::string(n[r]) + " " + st(i) + ", st0";
        break;
    }
    case 0xDF:
        if (m == 0xE0) return "fnstsw ax";
        if (r == 5) return "fucomip st0, " + st(i);
        if (r == 6) return "fcomip st0, " + st(i);
        break;
    }
    ok = false;
    return "";
}

std::string Join(const std::string& m, const std::string& a, const std::string& b, const std::string& c) {
    std::string s = m;
    if (!a.empty()) s += " " + a;
    if (!b.empty()) s += ", " + b;
    if (!c.empty()) s += ", " + c;
    return s;
}

std::string Decoder::Decode() {
    // prefixes
    for (;;) {
        if (i >= n) return "";
        uint8_t b = p[i];
        if (b == 0x66) opsize = true, mandatory = 0x66;
        else if (b == 0x67) adsize = true;
        else if (b == 0xF0) lock = true;
        else if (b == 0xF2 || b == 0xF3) rep = b, mandatory = b;
        else if (b == 0x2E || b == 0x36 || b == 0x3E || b == 0x26 || b == 0x64 || b == 0x65) {
        } else break;
        i++;
    }
    if (x64 && i < n && (p[i] & 0xF0) == 0x40) rex = p[i++];
    uint8_t op = Byte();
    if (!ok) return "";
    int osz = OperandSize();
    std::string pre = lock ? "lock " : "";

    if (op == 0x0F) return pre + Decode0F();
    if (op >= 0xD8 && op <= 0xDF) return DecodeX87(op);
    // VEX: always in 64-bit mode; in 32-bit mode only when the next byte could not be a ModRM of les/lds memory
    if ((op == 0xC4 || op == 0xC5) && (x64 || (i < n && (p[i] & 0xC0) == 0xC0))) return DecodeVex(op);
    if (op == 0x9B) return "wait";
    if (op < 0x40 && (op & 7) < 6) {  // add / or / adc / sbb / and / sub / xor / cmp
        const char* m = kGroup1[op >> 3];
        switch (op & 7) {
        case 0: ReadModRM(); return pre + Join(m, E(1), G(1));
        case 1: ReadModRM(); return pre + Join(m, E(osz), G(osz));
        case 2: ReadModRM(); return Join(m, G(1), E(1));
        case 3: ReadModRM(); return Join(m, G(osz), E(osz));
        case 4: return Join(m, "al", Hex(static_cast<uint8_t>(Imm(1))));
        case 5: {
            int64_t v = Imm(osz == 2 ? 2 : 4);
            return Join(m, Reg(osz, 0), osz == 8 ? SignedHex(v) : Hex(static_cast<uint64_t>(v) & (osz == 2 ? 0xFFFF : 0xFFFFFFFF)));
        }
        }
    }
    if (!x64 && op >= 0x40 && op <= 0x4F) return Join(op < 0x48 ? "inc" : "dec", Reg(osz, op & 7));
    if (op >= 0x50 && op <= 0x57) return Join("push", Reg(StackSize(), (op & 7) | ((rex & 1) ? 8 : 0)));
    if (op >= 0x58 && op <= 0x5F) return Join("pop", Reg(StackSize(), (op & 7) | ((rex & 1) ? 8 : 0)));
    if (!x64 && op == 0x60) return opsize ? "pushaw" : "pushad";
    if (!x64 && op == 0x61) return opsize ? "popaw" : "popad";
    if (x64 && op == 0x63) {
        ReadModRM();
        return Join("movsxd", G(osz), E(4));
    }
    if (op == 0x68) return Join("push", SignedHex(Imm(opsize ? 2 : 4)));
    if (op == 0x6A) return Join("push", SignedHex(Imm(1)));
    if (op == 0x69 || op == 0x6B) {
        ReadModRM();
        std::string g = G(osz), e = E(osz);
        int64_t v = Imm(op == 0x6B ? 1 : osz == 2 ? 2 : 4);
        return Join("imul", g, e, osz == 8 ? SignedHex(v) : Hex(static_cast<uint64_t>(v) & (osz == 2 ? 0xFFFF : 0xFFFFFFFF)));
    }
    if (op >= 0x70 && op <= 0x7F) {
        int64_t rel = Imm(1);
        return Join(std::string("j") + kCond[op & 15], Target(rel));
    }
    if (op >= 0x80 && op <= 0x83 && op != 0x82) {
        ReadModRM();
        int size = op == 0x80 ? 1 : osz;
        std::string e = E(size);
        int64_t v = Imm(op == 0x81 ? (osz == 2 ? 2 : 4) : 1);
        std::string imm = size == 8 ? SignedHex(v)
                        : Hex(static_cast<uint64_t>(v) & (size == 1 ? 0xFF : size == 2 ? 0xFFFF : 0xFFFFFFFF));
        return pre + Join(kGroup1[reg & 7], e, imm);
    }
    if (op == 0x84 || op == 0x85) {
        ReadModRM();
        int size = op == 0x84 ? 1 : osz;
        return Join("test", E(size), G(size));
    }
    if (op == 0x86 || op == 0x87) {
        ReadModRM();
        int size = op == 0x86 ? 1 : osz;
        return pre + Join("xchg", E(size), G(size));
    }
    if (op >= 0x88 && op <= 0x8B) {
        ReadModRM();
        int size = (op & 1) ? osz : 1;
        return op < 0x8A ? Join("mov", E(size), G(size)) : Join("mov", G(size), E(size));
    }
    if (op == 0x8D) {
        ReadModRM();
        if (mod == 3) ok = false;
        return Join("lea", G(osz), mem);
    }
    if (op == 0x8F) {
        ReadModRM();
        if ((reg & 7) != 0) ok = false;
        return Join("pop", E(StackSize()));
    }
    if (op == 0x90 && !(rex & 1)) return rep == 0xF3 ? "pause" : "nop";
    if (op >= 0x90 && op <= 0x97) return Join("xchg", Reg(osz, (op & 7) | ((rex & 1) ? 8 : 0)), Reg(osz, 0));
    if (op == 0x98) return osz == 8 ? "cdqe" : osz == 2 ? "cbw" : "cwde";
    if (op == 0x99) return osz == 8 ? "cqo" : osz == 2 ? "cwd" : "cdq";
    if (op == 0x9C) return x64 ? (opsize ? "pushfw" : "pushfq") : (opsize ? "pushfw" : "pushfd");
    if (op == 0x9D) return x64 ? (opsize ? "popfw" : "popfq") : (opsize ? "popfw" : "popfd");
    if (op == 0x9E) return "sahf";
    if (op == 0x9F) return "lahf";
    if (op >= 0xA0 && op <= 0xA3 && !x64) {  // mov al/eax <-> [absolute address]
        int size = (op & 1) ? osz : 1;
        std::string m = std::string(SizeName(size)) + " [" + Hex(static_cast<uint32_t>(Imm(4))) + "]";
        return op < 0xA2 ? Join("mov", Reg(size, 0), m) : Join("mov", m, Reg(size, 0));
    }
    if (op == 0xA8) return Join("test", "al", Hex(static_cast<uint8_t>(Imm(1))));
    if (op == 0xA9) {
        int64_t v = Imm(osz == 2 ? 2 : 4);
        return Join("test", Reg(osz, 0), osz == 8 ? SignedHex(v) : Hex(static_cast<uint64_t>(v) & (osz == 2 ? 0xFFFF : 0xFFFFFFFF)));
    }
    if (op >= 0xA4 && op <= 0xAF && op != 0xA8 && op != 0xA9) {  // string instructions
        static const char* names[] = {"movs", "movs", "cmps", "cmps", "", "", "stos", "stos", "lods", "lods", "scas", "scas"};
        std::string name = names[op - 0xA4];
        int size = (op & 1) ? osz : 1;
        name += size == 1 ? "b" : size == 2 ? "w" : size == 4 ? "d" : "q";
        bool compares = op == 0xA6 || op == 0xA7 || op == 0xAE || op == 0xAF;
        std::string prefix = rep == 0xF3 ? (compares ? "repe " : "rep ") : rep == 0xF2 ? "repne " : "";
        return prefix + name;
    }
    if (op >= 0xB0 && op <= 0xB7) return Join("mov", Reg(1, (op & 7) | ((rex & 1) ? 8 : 0)), Hex(static_cast<uint8_t>(Imm(1))));
    if (op >= 0xB8 && op <= 0xBF) {
        int64_t v = Imm(osz);
        uint64_t u = osz == 8 ? static_cast<uint64_t>(v) : static_cast<uint64_t>(v) & (osz == 2 ? 0xFFFF : 0xFFFFFFFF);
        return Join("mov", Reg(osz, (op & 7) | ((rex & 1) ? 8 : 0)), Hex(u));
    }
    if (op == 0xC0 || op == 0xC1 || (op >= 0xD0 && op <= 0xD3)) {
        ReadModRM();
        int size = (op & 1) ? osz : 1;
        std::string e = E(size);
        std::string count = op <= 0xC1 ? Hex(static_cast<uint8_t>(Imm(1))) : op <= 0xD1 ? "1" : "cl";
        return Join(kGroup2[reg & 7], e, count);
    }
    if (op == 0xC2) return Join("ret", Hex(static_cast<uint16_t>(Imm(2))));
    if (op == 0xC3) return rep == 0xF3 ? "rep ret" : "ret";
    if (op == 0xC6 || op == 0xC7) {
        ReadModRM();
        if ((reg & 7) != 0) ok = false;
        int size = op == 0xC6 ? 1 : osz;
        std::string e = E(size);
        int64_t v = Imm(size == 1 ? 1 : size == 2 ? 2 : 4);
        return Join("mov", e, size == 8 ? SignedHex(v) : Hex(static_cast<uint64_t>(v) & (size == 1 ? 0xFF : size == 2 ? 0xFFFF : 0xFFFFFFFF)));
    }
    if (op == 0xC8) {
        int64_t size = Imm(2) & 0xFFFF;
        return Join("enter", Hex(static_cast<uint64_t>(size)), Hex(Byte()));
    }
    if (op == 0xC9) return "leave";
    if (op == 0xCC) return "int3";
    if (op == 0xCD) return Join("int", Hex(Byte()));
    if (op >= 0xE0 && op <= 0xE3) {
        static const char* names[] = {"loopne", "loope", "loop", ""};
        int64_t rel = Imm(1);
        std::string name = op == 0xE3 ? (x64 ? (adsize ? "jecxz" : "jrcxz") : (adsize ? "jcxz" : "jecxz")) : names[op - 0xE0];
        return Join(name, Target(rel));
    }
    if (op == 0xE8 || op == 0xE9) {
        int64_t rel = Imm(4);
        return Join(op == 0xE8 ? "call" : "jmp", Target(rel));
    }
    if (op == 0xEB) {
        int64_t rel = Imm(1);
        return Join("jmp short", Target(rel));
    }
    if (op == 0xF4) return "hlt";
    if (op == 0xF5) return "cmc";
    if (op == 0xF6 || op == 0xF7) {
        ReadModRM();
        static const char* names[] = {"test", "test", "not", "neg", "mul", "imul", "div", "idiv"};
        int size = op == 0xF6 ? 1 : osz;
        std::string e = E(size);
        if ((reg & 7) < 2) {
            int64_t v = Imm(size == 1 ? 1 : size == 2 ? 2 : 4);
            return Join("test", e, size == 8 ? SignedHex(v) : Hex(static_cast<uint64_t>(v) & (size == 1 ? 0xFF : size == 2 ? 0xFFFF : 0xFFFFFFFF)));
        }
        return pre + Join(names[reg & 7], e);
    }
    if (op == 0xF8) return "clc";
    if (op == 0xF9) return "stc";
    if (op == 0xFA) return "cli";
    if (op == 0xFB) return "sti";
    if (op == 0xFC) return "cld";
    if (op == 0xFD) return "std";
    if (op == 0xFE) {
        ReadModRM();
        if ((reg & 7) > 1) ok = false;
        return pre + Join((reg & 7) == 0 ? "inc" : "dec", E(1));
    }
    if (op == 0xFF) {
        ReadModRM();
        switch (reg & 7) {
        case 0: return pre + Join("inc", E(osz));
        case 1: return pre + Join("dec", E(osz));
        case 2: return Join("call", E(StackSize()));
        case 4: return Join("jmp", E(StackSize()));
        case 6: return Join("push", E(StackSize()));
        }
        ok = false;
        return "";
    }
    ok = false;
    return "";
}

std::string Decoder::Decode0F() {
    uint8_t op = Byte();
    int osz = OperandSize();
    bool sse66 = mandatory == 0x66, sseF3 = mandatory == 0xF3, sseF2 = mandatory == 0xF2;
    if (op == 0x05) return "syscall";
    if (op == 0x0B) return "ud2";
    if (op == 0x1F) {
        ReadModRM();
        return Join("nop", E(osz));
    }
    if (op == 0x31) return "rdtsc";
    if (op == 0xA2) return "cpuid";
    if (op >= 0x40 && op <= 0x4F) {
        ReadModRM();
        return Join(std::string("cmov") + kCond[op & 15], G(osz), E(osz));
    }
    if (op >= 0x80 && op <= 0x8F) {
        int64_t rel = Imm(4);
        return Join(std::string("j") + kCond[op & 15], Target(rel));
    }
    if (op >= 0x90 && op <= 0x9F) {
        ReadModRM();
        return Join(std::string("set") + kCond[op & 15], E(1));
    }
    if (op == 0xA3 || op == 0xAB || op == 0xB3 || op == 0xBB) {
        ReadModRM();
        const char* m = op == 0xA3 ? "bt" : op == 0xAB ? "bts" : op == 0xB3 ? "btr" : "btc";
        return Join(m, E(osz), G(osz));
    }
    if (op == 0xBA) {
        ReadModRM();
        static const char* names[] = {"", "", "", "", "bt", "bts", "btr", "btc"};
        if ((reg & 7) < 4) ok = false;
        std::string e = E(osz);
        return Join(names[reg & 7], e, Hex(Byte()));
    }
    if (op == 0xA4 || op == 0xA5 || op == 0xAC || op == 0xAD) {
        ReadModRM();
        std::string e = E(osz), g = G(osz);
        std::string count = (op & 1) ? "cl" : Hex(Byte());
        return Join(op < 0xA8 ? "shld" : "shrd", e, g, count);
    }
    if (op == 0xAF) {
        ReadModRM();
        return Join("imul", G(osz), E(osz));
    }
    if (op == 0xB0 || op == 0xB1) {
        ReadModRM();
        int size = op == 0xB0 ? 1 : osz;
        return Join("cmpxchg", E(size), G(size));
    }
    if (op == 0xC0 || op == 0xC1) {
        ReadModRM();
        int size = op == 0xC0 ? 1 : osz;
        return Join("xadd", E(size), G(size));
    }
    if (op == 0xB6 || op == 0xB7 || op == 0xBE || op == 0xBF) {
        ReadModRM();
        return Join(op < 0xB8 ? "movzx" : "movsx", G(osz), E((op & 1) ? 2 : 1));
    }
    if (op == 0xB8 && sseF3) {
        ReadModRM();
        return Join("popcnt", G(osz), E(osz));
    }
    if (op == 0xBC || op == 0xBD) {
        ReadModRM();
        const char* m = sseF3 ? (op == 0xBC ? "tzcnt" : "lzcnt") : (op == 0xBC ? "bsf" : "bsr");
        return Join(m, G(osz), E(osz));
    }
    if (op >= 0xC8 && op <= 0xCF) return Join("bswap", Reg(W() ? 8 : 4, (op & 7) | ((rex & 1) ? 8 : 0)));

    // SSE / SSE2: the mandatory prefix selects ps / pd / ss / sd
    std::string sfx = sseF3 ? "ss" : sseF2 ? "sd" : sse66 ? "pd" : "ps";
    int scalar = sseF3 ? 4 : sseF2 ? 8 : 0;
    if (op == 0x10 || op == 0x11) {
        ReadModRM();
        std::string m = sseF3 ? "movss" : sseF2 ? "movsd" : sse66 ? "movupd" : "movups";
        return op == 0x10 ? Join(m, GX(), EX(scalar)) : Join(m, EX(scalar), GX());
    }
    if (op == 0x28 || op == 0x29) {
        ReadModRM();
        std::string m = sse66 ? "movapd" : "movaps";
        return op == 0x28 ? Join(m, GX(), EX(0)) : Join(m, EX(0), GX());
    }
    if (op == 0x2A && (sseF3 || sseF2)) {
        ReadModRM();
        return Join(sseF3 ? "cvtsi2ss" : "cvtsi2sd", GX(), E(W() ? 8 : 4));
    }
    if ((op == 0x2C || op == 0x2D) && (sseF3 || sseF2)) {
        ReadModRM();
        std::string m = std::string(op == 0x2C ? "cvtt" : "cvt") + (sseF3 ? "ss2si" : "sd2si");
        return Join(m, G(W() ? 8 : 4), EX(scalar));
    }
    if ((op == 0x2E || op == 0x2F) && !sseF3 && !sseF2) {
        ReadModRM();
        std::string m = std::string(op == 0x2E ? "ucomis" : "comis") + (sse66 ? "d" : "s");
        return Join(m, GX(), EX(sse66 ? 8 : 4));
    }
    if (op == 0x51 || (op >= 0x58 && op <= 0x5F && op != 0x5A && op != 0x5B) ||
        ((op >= 0x54 && op <= 0x57) && !sseF3 && !sseF2)) {
        ReadModRM();
        static const std::map<int, std::string> names = {{0x51, "sqrt"}, {0x54, "and"}, {0x55, "andn"}, {0x56, "or"},
                                                         {0x57, "xor"}, {0x58, "add"}, {0x59, "mul"}, {0x5C, "sub"},
                                                         {0x5D, "min"}, {0x5E, "div"}, {0x5F, "max"}};
        return Join(names.at(op) + sfx, GX(), EX(scalar));
    }
    if (op == 0x5A) {
        ReadModRM();
        std::string m = sseF3 ? "cvtss2sd" : sseF2 ? "cvtsd2ss" : sse66 ? "cvtpd2ps" : "cvtps2pd";
        return Join(m, GX(), EX(sseF3 ? 4 : sseF2 ? 8 : 0));
    }
    if (op == 0x6E && sse66) {
        ReadModRM();
        return Join(W() ? "movq" : "movd", GX(), E(W() ? 8 : 4));
    }
    if (op == 0x7E && sse66) {
        ReadModRM();
        return Join(W() ? "movq" : "movd", E(W() ? 8 : 4), GX());
    }
    if (op == 0x7E && sseF3) {
        ReadModRM();
        return Join("movq", GX(), EX(8));
    }
    if (op == 0xD6 && sse66) {
        ReadModRM();
        return Join("movq", EX(8), GX());
    }
    if ((op == 0x6F || op == 0x7F) && (sse66 || sseF3)) {
        ReadModRM();
        std::string m = sse66 ? "movdqa" : "movdqu";
        return op == 0x6F ? Join(m, GX(), EX(0)) : Join(m, EX(0), GX());
    }
    if (sse66) {
        static const std::map<int, std::string> packed = {
            {0xEF, "pxor"}, {0xDB, "pand"}, {0xDF, "pandn"}, {0xEB, "por"}, {0xFC, "paddb"}, {0xFD, "paddw"},
            {0xFE, "paddd"}, {0xD4, "paddq"}, {0xF8, "psubb"}, {0xF9, "psubw"}, {0xFA, "psubd"}, {0xFB, "psubq"},
            {0x74, "pcmpeqb"}, {0x75, "pcmpeqw"}, {0x76, "pcmpeqd"}, {0x64, "pcmpgtb"}, {0x65, "pcmpgtw"},
            {0x66, "pcmpgtd"}, {0xD7, "pmovmskb"}, {0xD5, "pmullw"}, {0x60, "punpcklbw"}, {0x61, "punpcklwd"},
            {0x62, "punpckldq"}, {0x6C, "punpcklqdq"}, {0x68, "punpckhbw"}, {0x69, "punpckhwd"}, {0x6A, "punpckhdq"},
            {0x6D, "punpckhqdq"}};
        auto it = packed.find(op);
        if (it != packed.end()) {
            ReadModRM();
            if (op == 0xD7) return Join(it->second, G(4), EX(0));
            return Join(it->second, GX(), EX(0));
        }
        if (op == 0x70) {
            ReadModRM();
            std::string g = GX(), e = EX(0);
            return Join("pshufd", g, e, Hex(Byte()));
        }
    }
    ok = false;
    return "";
}

std::string HexBytes(const uint8_t* p, size_t n) {
    std::string s;
    char b[4];
    for (size_t k = 0; k < n; k++) {
        sprintf_s(b, "%02X", p[k]);
        s += b;
    }
    return s;
}
}

std::vector<Line> Decode(const uint8_t* data, size_t size, uint64_t address, int bits) {
    std::vector<Line> out;
    size_t at = 0;
    while (at < size) {
        Decoder d{data + at, std::min<size_t>(size - at, 15), address + at, bits == 64};
        std::string text = d.Decode();
        Line line;
        line.address = address + at;
        if (!d.ok || text.empty() || d.i == 0) {  // unknown or cut off: one byte of data
            line.bytes = HexBytes(data + at, 1);
            line.text = "db " + Hex(data[at]);
            at++;
        } else {
            line.bytes = HexBytes(data + at, d.i);
            line.text = d.Finish(text);
            at += d.i;
        }
        out.push_back(line);
    }
    return out;
}

}
