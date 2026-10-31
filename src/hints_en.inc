// Included by hints.cpp: the English reference. It overwrites every entry of the Russian table (same names);
// the test ReferenceInEnglish checks that no card is left in Russian.
namespace Hints {
namespace {

void AddEnglish(std::unordered_map<std::string, Entry>& m) {
    const char* none = "unchanged";
    const char* arith = "OF SF ZF AF PF CF";
    const char* logic = "SF ZF PF from the result; OF = CF = 0; AF undefined";
    const char* fpu = "C0–C3 FPU";
    auto set = [&](const char* names, Entry e) {
        std::istringstream in(names);
        std::string n;
        while (in >> n) m[n] = e;
    };

    // ---- data movement
    set("mov", {"mov dest, src",
                "Copies src to dest. Operands: register, memory, number; memory to memory is not allowed. Writing a "
                "32-bit register (eax) clears the upper 32 bits of rax; writing ax/al leaves them.", none});
    set("movzx", {"movzx r16/32/64, r/m8 | movzx r32/64, r/m16",
                  "Copies a smaller operand into a larger one, filling the upper bits with zeros (unsigned extension). "
                  "There is no movzx for 32 → 64 bits: mov eax, ... already clears the upper bits.", none});
    set("movsx", {"movsx r16/32/64, r/m8 | movsx r32/64, r/m16",
                  "Copies a smaller operand into a larger one with sign extension (for signed numbers).", none});
    set("movsxd", {"movsxd r64, r/m32", "Sign-extends a 32-bit number to 64 bits.", none});
    set("lea", {"lea register, [address]",
                "Computes the address of the operand and puts it in the register; memory is not read. Handy for "
                "arithmetic too: lea rax, [rbx + rcx*4 + 8]. With default rel: lea rcx, [msg] gives the address of msg.",
                none});
    set("xchg", {"xchg a, b", "Swaps the operands. With a memory operand it is atomic (an implicit lock).", none});
    set("push", {"push r64 | push [m64] | push imm32",
                 "Decreases rsp by 8 and writes the value to [rsp]. In 64-bit mode 8 bytes are pushed.", none});
    set("pop", {"pop r64 | pop [m64]", "Reads 8 bytes from [rsp] into the operand and increases rsp by 8.", none});
    set("bswap", {"bswap r32/r64", "Reverses the byte order (little-endian ↔ big-endian).", none});
    set("cbw", {"cbw", "Sign extension: al → ax.", none});
    set("cwde", {"cwde", "Sign extension: ax → eax.", none});
    set("cdqe", {"cdqe", "Sign extension: eax → rax.", none});
    set("cwd", {"cwd", "Sign-extends ax into dx (dx:ax), before a 16-bit idiv.", none});
    set("cdq", {"cdq", "Sign-extends eax into edx (edx:eax), before a 32-bit idiv.", none});
    set("cqo", {"cqo", "Sign-extends rax into rdx (rdx:rax), before a 64-bit idiv.", none});

    // ---- arithmetic
    set("add", {"add dest, src", "dest = dest + src.", arith});
    set("adc", {"adc dest, src", "dest = dest + src + CF. For adding numbers longer than a register.", arith});
    set("sub", {"sub dest, src", "dest = dest − src.", arith});
    set("sbb", {"sbb dest, src", "dest = dest − (src + CF). For long subtraction.", arith});
    set("inc", {"inc operand", "Adds 1.", "OF SF ZF AF PF (CF unchanged)"});
    set("dec", {"dec operand", "Subtracts 1.", "OF SF ZF AF PF (CF unchanged)"});
    set("neg", {"neg operand", "Changes the sign: operand = −operand. CF = 1 if the operand was not 0.", arith});
    set("cmp", {"cmp a, b",
                "Computes a − b only for the flags (the result is not stored). Then jcc, setcc or cmovcc: jl/jg for "
                "signed numbers, jb/ja for unsigned.", arith});
    set("mul", {"mul r/m",
                "Unsigned multiplication by rax: rdx:rax = rax × operand. 32 bits: edx:eax = eax × r/m32; 16: dx:ax; "
                "8: ax = al × r/m8.", "OF = CF = 1 if the upper half of the result is not 0; SF ZF AF PF undefined"});
    set("imul", {"imul r/m | imul r, r/m | imul r, r/m, imm",
                 "Signed multiplication. One operand: like mul (rdx:rax = rax × r/m); two: r = r × r/m; three: r = "
                 "r/m × imm. The 2- and 3-operand forms lose the upper half.",
                 "OF = CF = 1 on overflow; the others undefined"});
    set("div", {"div r/m",
                "Unsigned division of rdx:rax by the operand: rax = quotient, rdx = remainder (32 bits: edx:eax → eax, "
                "edx). Clear rdx before dividing (xor edx, edx). Division by 0 or a too large quotient raises #DE.",
                "undefined"});
    set("idiv", {"idiv r/m",
                 "Signed division of rdx:rax by the operand: rax = quotient, rdx = remainder (with the dividend's sign). "
                 "Sign-extend before dividing: cqo (64 bits) or cdq (32 bits).", "undefined"});

    // ---- logic, shifts, bits
    set("and", {"and dest, src", "Bitwise AND. Often used to clear bits with a mask.", logic});
    set("or", {"or dest, src", "Bitwise OR. Often used to set bits.", logic});
    set("xor", {"xor dest, src",
                "Bitwise exclusive OR. xor eax, eax is the fastest way to clear a register (all of rax).", logic});
    set("not", {"not operand", "Inverts all bits.", none});
    set("test", {"test a, b", "Computes a AND b only for the flags. test rax, rax checks for zero (jz) and sign (js).", logic});
    set("shl sal", {"shl operand, 1 | imm8 | cl",
                    "Shift left (multiplication by 2^n). The bit shifted out goes to CF. The count is taken modulo 64 (32).",
                    "CF: the last bit shifted out; OF defined for a shift by 1; SF ZF PF from the result"});
    set("shr", {"shr operand, 1 | imm8 | cl", "Logical shift right, zeros come in from the left (unsigned division by 2^n).",
                "CF: the last bit shifted out; SF ZF PF from the result"});
    set("sar", {"sar operand, 1 | imm8 | cl",
                "Arithmetic shift right: copies the sign bit (signed division by 2^n rounding down).",
                "CF: the last bit shifted out; SF ZF PF from the result"});
    set("rol ror", {"rol/ror operand, 1 | imm8 | cl", "Rotation: the bits shifted out come back on the other side.",
                    "CF: the last bit rotated; OF for a rotation by 1"});
    set("rcl rcr", {"rcl/rcr operand, 1 | imm8 | cl", "Rotation through the CF flag.", "CF; OF for a rotation by 1"});
    set("shld shrd", {"shld/shrd dest, src, imm8 | cl", "Shift filled with bits of the second operand (a 128-bit pair shift).",
                      "CF SF ZF PF; OF for 1"});
    set("bt", {"bt operand, bit", "Copies the given bit to CF.", "CF; the others undefined"});
    set("bts", {"bts operand, bit", "Copies the bit to CF and sets it to 1.", "CF"});
    set("btr", {"btr operand, bit", "Copies the bit to CF and clears it.", "CF"});
    set("btc", {"btc operand, bit", "Copies the bit to CF and inverts it.", "CF"});
    set("bsf", {"bsf dest, src", "Index of the lowest 1 bit of src. If src is 0, ZF = 1 and dest is undefined.", "ZF"});
    set("bsr", {"bsr dest, src", "Index of the highest 1 bit of src. If src is 0, ZF = 1.", "ZF"});
    set("popcnt", {"popcnt dest, src", "Number of 1 bits.", "ZF from the result; the others = 0"});
    set("lzcnt", {"lzcnt dest, src", "Number of leading zero bits (the operand size for 0).",
                  "CF = 1 if src is 0; ZF from the result"});
    set("tzcnt", {"tzcnt dest, src", "Number of trailing zero bits (the operand size for 0).",
                  "CF = 1 if src is 0; ZF from the result"});

    // ---- control flow
    set("jmp", {"jmp label | jmp r/m64", "Unconditional jump.", none});
    set("call", {"call label | call r/m64",
                 "Pushes the return address (rsp −= 8) and jumps. In Win64 before call: rsp is a multiple of 16 and 32 "
                 "bytes of shadow space are reserved (sub rsp, 40 at the start of the function). Arguments: rcx, rdx, r8, r9.",
                 none});
    set("ret retn", {"ret | ret imm16",
                     "Pops the return address and jumps to it; ret imm16 also releases imm16 bytes.", none});
    set("loop", {"loop label", "rcx = rcx − 1; jumps if rcx ≠ 0. Slower than dec + jnz.", none});
    set("loope loopz", {"loope label", "rcx −= 1; jumps if rcx ≠ 0 and ZF = 1.", none});
    set("loopne loopnz", {"loopne label", "rcx −= 1; jumps if rcx ≠ 0 and ZF = 0.", none});
    set("jrcxz", {"jrcxz label", "Jumps if rcx = 0.", none});
    set("jecxz", {"jecxz label", "Jumps if ecx = 0.", none});
    set("leave", {"leave", "Leaves a stack frame: mov rsp, rbp; pop rbp.", none});
    set("enter", {"enter size, 0", "Creates a stack frame: push rbp; mov rbp, rsp; sub rsp, size.", none});
    set("syscall", {"syscall", "A kernel system call (number in rax). Undocumented on Windows: use the WinAPI.",
                    "saved in r11"});
    set("int", {"int imm8", "Software interrupt. Hardly used on 64-bit Windows.", "IF TF cleared"});
    set("int3", {"int3", "Breakpoint: under the debugger the program stops on this line.", none});
    set("nop", {"nop", "Does nothing (1 byte; with an operand, a long nop for alignment).", none});
    set("hlt", {"hlt", "Halts the processor; in an ordinary program it raises an exception.", none});
    set("ud2", {"ud2", "A guaranteed invalid instruction (#UD exception).", none});

    // ---- flags, strings, misc
    set("clc", {"clc", "CF = 0.", "CF"});
    set("stc", {"stc", "CF = 1.", "CF"});
    set("cmc", {"cmc", "CF = NOT CF.", "CF"});
    set("cld", {"cld", "DF = 0: string instructions go forward (rsi/rdi grow). Win64 expects DF = 0.", "DF"});
    set("std", {"std", "DF = 1: string instructions go backward. Restore cld before calling functions.", "DF"});
    set("lahf", {"lahf", "ah = SF ZF AF PF CF (the low byte of the flags).", none});
    set("sahf", {"sahf", "SF ZF AF PF CF = ah.", "SF ZF AF PF CF"});
    set("pushf pushfq", {"pushfq", "Pushes the flags register.", none});
    set("popf popfq", {"popfq", "Pops the flags register.", "all (except the system ones)"});
    set("movsb movsw movsq", {"movsb/w/d/q",
                              "Copies an element [rsi] → [rdi] and advances rsi, rdi. With rep: repeats rcx times (block copy).",
                              none});
    set("stosb stosw stosd stosq", {"stosb/w/d/q", "[rdi] = al/ax/eax/rax and advances rdi. rep stosb fills rcx bytes.", none});
    set("lodsb lodsw lodsd lodsq", {"lodsb/w/d/q", "al/ax/eax/rax = [rsi] and advances rsi.", none});
    set("scasb scasw scasd scasq", {"scasb/w/d/q",
                                    "Compares al/ax/eax/rax with [rdi] and advances rdi. repne scasb searches for a byte "
                                    "(for example the zero at the end of a string).", arith});
    set("cmpsb cmpsw cmpsq", {"cmpsb/w/d/q", "Compares [rsi] with [rdi] and advances both. repe cmpsb compares blocks.", arith});
    set("rep", {"rep string_instruction", "Repeats movs/stos/lods rcx times (rcx counts down to 0).", none});
    set("repe repz", {"repe cmps/scas", "Repeats while rcx ≠ 0 and the elements are equal (ZF = 1).", none});
    set("repne repnz", {"repne cmps/scas", "Repeats while rcx ≠ 0 and the elements differ (ZF = 0).", none});
    set("lock", {"lock instruction", "Makes a memory instruction atomic for other threads (add, inc, xadd, cmpxchg…).", none});
    set("xadd", {"xadd dest, src", "Exchange and add: temp = dest + src; src = old dest; dest = the sum. lock xadd: an "
                                   "atomic counter.", arith});
    set("cmpxchg", {"cmpxchg dest, src",
                    "If rax = dest: dest = src, ZF = 1; otherwise rax = dest, ZF = 0. lock cmpxchg: an atomic "
                    "compare-and-swap.", arith});
    set("cpuid", {"cpuid", "Processor information: function in eax (and ecx), result in eax, ebx, ecx, edx.", none});
    set("rdtsc", {"rdtsc", "The processor's cycle counter: edx:eax.", none});

    // ---- SSE (scalar float/double)
    set("movss", {"movss xmm, xmm/m32 | movss m32, xmm", "Copies one float (32 bits).", none});
    set("movsd", {"movsd xmm, xmm/m64 | movsd (no operands)",
                  "With xmm operands: copies one double (64 bits). Without operands: a string instruction that copies "
                  "4 bytes [rsi] → [rdi].", none});
    set("movaps movapd movdqa", {"movaps xmm, xmm/m128",
                                 "Copies 16 bytes. A memory address must be aligned to 16, or an exception follows.", none});
    set("movups movupd movdqu", {"movups xmm, xmm/m128", "Copies 16 bytes without an alignment requirement.", none});
    set("movd", {"movd xmm, r/m32 | movd r/m32, xmm", "Copies 32 bits between a general-purpose register and xmm.", none});
    set("movq", {"movq xmm, r/m64 | movq r/m64, xmm", "Copies 64 bits between a general-purpose register and xmm.", none});
    for (const char* op : {"add", "sub", "mul", "div", "min", "max", "sqrt"}) {
        std::string o = op;
        std::string what = o == "add" ? "addition" : o == "sub" ? "subtraction" : o == "mul" ? "multiplication"
                         : o == "div" ? "division" : o == "min" ? "minimum" : o == "max" ? "maximum" : "square root";
        m[o + "ss"] = {o + "ss xmm, xmm/m32", "Scalar " + what + " of floats (the low 32 bits of xmm).", none};
        m[o + "sd"] = {o + "sd xmm, xmm/m64", "Scalar " + what + " of doubles (the low 64 bits of xmm).", none};
        m[o + "ps"] = {o + "ps xmm, xmm/m128", "Packed " + what + ": 4 floats at once.", none};
        m[o + "pd"] = {o + "pd xmm, xmm/m128", "Packed " + what + ": 2 doubles at once.", none};
    }
    set("cvtsi2sd", {"cvtsi2sd xmm, r/m32/64", "Signed integer → double.", none});
    set("cvtsi2ss", {"cvtsi2ss xmm, r/m32/64", "Signed integer → float.", none});
    set("cvttsd2si", {"cvttsd2si r32/64, xmm/m64", "double → integer, truncating the fraction.", none});
    set("cvttss2si", {"cvttss2si r32/64, xmm/m32", "float → integer, truncating the fraction.", none});
    set("cvtsd2si", {"cvtsd2si r32/64, xmm/m64", "double → integer, rounding by the current mode (to nearest).", none});
    set("cvtsd2ss", {"cvtsd2ss xmm, xmm/m64", "double → float.", none});
    set("cvtss2sd", {"cvtss2sd xmm, xmm/m32", "float → double.", none});
    set("comisd ucomisd", {"comisd xmm, xmm/m64",
                           "Compares two doubles and sets the flags like an unsigned comparison: then ja/jb/je. If either "
                           "is NaN, ZF = PF = CF = 1.", "ZF PF CF; OF = SF = AF = 0"});
    set("comiss ucomiss", {"comiss xmm, xmm/m32", "The same for floats: flags like an unsigned comparison (ja/jb/je).",
                           "ZF PF CF; OF = SF = AF = 0"});
    set("xorps xorpd pxor", {"xorps xmm, xmm/m128", "Bitwise XOR of 128 bits. xorps xmm0, xmm0 clears the register.", none});
    set("andps andpd pand", {"andps xmm, xmm/m128", "Bitwise AND of 128 bits (for example the absolute value by a mask).", none});

    // ---- x87
    set("fld", {"fld m32/m64/m80 | fld st(i)", "Loads a number onto the FPU stack (st0).", "C1 FPU"});
    set("fild", {"fild m16/m32/m64", "Loads an integer onto the FPU stack, converting it.", "C1 FPU"});
    set("fst fstp", {"fstp m32/m64/m80 | fstp st(i)", "Stores st0 (fstp then pops it off the FPU stack).", "C1 FPU"});
    set("fistp", {"fistp m16/m32/m64", "Stores st0 as an integer (rounded) and pops it.", "C1 FPU"});
    set("fadd faddp fsub fsubp fmul fmulp fdiv fdivp",
        {"fadd m | fadd st0, st(i) | faddp", "x87 arithmetic: the result in st0 (or st(i) for the p form, which pops).",
         "C1 FPU"});
    set("finit fninit", {"finit", "Resets the FPU to its initial state.", "all FPU flags"});

    // ---- NASM directives
    set("db", {"name db values", "Bytes (8 bits): numbers, characters, strings. msg db \"Hi\", 10, 0 is a string with a "
                                 "line break and a zero at the end.", ""});
    set("dw", {"name dw values", "Words of 2 bytes.", ""});
    set("dd", {"name dd values", "Double words of 4 bytes (int32, float).", ""});
    set("dq", {"name dq values", "Quad words of 8 bytes (int64, double, addresses).", ""});
    set("dt", {"name dt value", "10 bytes (an 80-bit x87 number).", ""});
    set("resb resw resd resq", {"name resb count",
                                "Reserves space without an initial value (usually in section .bss): resb bytes, resw by 2, "
                                "resd by 4, resq by 8.", ""});
    set("equ", {"NAME equ expression", "A build-time constant (takes no memory). len equ $ - msg is a string's length.", ""});
    set("times", {"times count instruction/data", "Repeats data or an instruction: times 64 db 0.", ""});
    set("section segment", {"section .text | .data | .bss | .rdata",
                            ".text is code, .data writable data, .rdata constants, .bss uninitialized data.", ""});
    set("global", {"global name", "Makes a label visible to the linker and other files (for example global main).", ""});
    set("extern", {"extern name", "Declares a symbol from another file or library (extern printf).", ""});
    set("default", {"default rel | default abs",
                    "default rel: [label] is addressed relative to rip, as 64-bit Windows code needs.", ""});
    set("bits", {"bits 16 | 32 | 64", "The bitness of the generated code (64 by default for -f win64).", ""});
    set("align alignb", {"align n", "Aligns the next address to n bytes (fills with nop or zeros).", ""});
    set("%define", {"%define NAME value", "A preprocessor text substitution (like #define in C).", ""});
    set("%assign", {"%assign NAME expression", "A numeric preprocessor variable (can be redefined).", ""});
    set("%macro", {"%macro name parameter_count … %endmacro",
                   "A multi-line macro; parameters inside are %1, %2…, local labels %%label.", ""});
    set("%include", {"%include \"file\"", "Inserts the text of the file (Ctrl+click on the name opens it).", ""});
    set("%if %ifdef %ifndef %else %elif %endif", {"%ifdef NAME … %else … %endif", "Conditional assembly.", ""});
    set("%rep", {"%rep count … %endrep", "Repeats a block of lines.", ""});

    // ---- generated: jcc, setcc, cmovcc
    struct Condition {
        const char* suffixes;
        const char* meaning;
        const char* test;
    };
    const Condition conditions[] = {
        {"e z", "equal / zero", "ZF = 1"},
        {"ne nz", "not equal / not zero", "ZF = 0"},
        {"g nle", "greater (signed numbers)", "ZF = 0 and SF = OF"},
        {"ge nl", "greater or equal (signed)", "SF = OF"},
        {"l nge", "less (signed)", "SF ≠ OF"},
        {"le ng", "less or equal (signed)", "ZF = 1 or SF ≠ OF"},
        {"a nbe", "above (unsigned, >)", "CF = 0 and ZF = 0"},
        {"ae nb nc", "above or equal (unsigned, ≥) / no carry", "CF = 0"},
        {"b nae c", "below (unsigned, <) / carry", "CF = 1"},
        {"be na", "below or equal (unsigned, ≤)", "CF = 1 or ZF = 1"},
        {"s", "the result is negative", "SF = 1"},
        {"ns", "the result is not negative", "SF = 0"},
        {"o", "overflow (signed)", "OF = 1"},
        {"no", "no overflow", "OF = 0"},
        {"p pe", "an even number of 1 bits in the low byte", "PF = 1"},
        {"np po", "an odd number of 1 bits in the low byte", "PF = 0"},
    };
    for (const Condition& c : conditions) {
        std::istringstream in(c.suffixes);
        std::string sfx;
        while (in >> sfx) {
            m["j" + sfx] = {"j" + sfx + " label",
                            std::string("Jumps if ") + c.meaning + " (" + c.test + "). Usually after cmp or test.", none};
            m["set" + sfx] = {"set" + sfx + " r/m8", std::string("Byte = 1 if ") + c.meaning + " (" + c.test + "), else 0.", none};
            m["cmov" + sfx] = {"cmov" + sfx + " register, r/m",
                               std::string("Copies the source into the register only if ") + c.meaning + " (" + c.test +
                                   "). No jump, so no branch mispredictions.", none};
        }
    }

    // ---- registers
    struct Reg {
        const char* names;
        const char* text;
    };
    const Reg regs[] = {
        {"rax eax ax al ah", "rax (eax, ax, al/ah). The function's return value; the implicit operand of mul/div. "
                             "Volatile: a called function may destroy it."},
        {"rbx ebx bx bl bh", "rbx (ebx, bx, bl/bh). Non-volatile: a function that changes it must restore it."},
        {"rcx ecx cx cl ch", "rcx (ecx, cx, cl/ch). The 1st integer argument in Win64; the counter of loop and rep; cl is the "
                             "shift count. Volatile."},
        {"rdx edx dx dl dh", "rdx (edx, dx, dl/dh). The 2nd argument in Win64; the upper half for mul/div. Volatile."},
        {"rsi esi si sil", "rsi (esi, si, sil). The source of string instructions. Non-volatile in Win64."},
        {"rdi edi di dil", "rdi (edi, di, dil). The destination of string instructions. Non-volatile in Win64."},
        {"rbp ebp bp bpl", "rbp (ebp, bp, bpl). The stack frame pointer (if used). Non-volatile."},
        {"rsp esp sp spl", "rsp is the stack pointer. Before call in Win64 it must be a multiple of 16; the caller reserves "
                           "32 bytes of shadow space for the arguments."},
        {"r8 r8d r8w r8b r8l", "r8 (r8d, r8w, r8b). The 3rd argument in Win64. Volatile."},
        {"r9 r9d r9w r9b r9l", "r9 (r9d, r9w, r9b). The 4th argument in Win64; further arguments go on the stack. Volatile."},
        {"r10 r10d r10w r10b r10l r11 r11d r11w r11b r11l", "r10, r11 are scratch (volatile) registers. syscall destroys r11."},
        {"r12 r12d r12w r12b r12l r13 r13d r13w r13b r13l r14 r14d r14w r14b r14l r15 r15d r15w r15b r15l",
         "r12–r15 are non-volatile: a function must restore them before it returns."},
        {"rip eip", "rip is the address of the next instruction. [rel label] addresses relative to rip."},
    };
    for (const Reg& r : regs) {
        std::istringstream in(r.names);
        std::string n;
        while (in >> n) {
            std::string size = RegisterSize(n);
            size.replace(size.find(" бит"), std::string(" бит").size(), "-bit");
            m[n] = {n + ": " + size + " register", r.text, ""};
        }
    }
    for (int i = 0; i < 16; i++) {
        std::string x = "xmm" + std::to_string(i);
        m[x] = {x + ": 128-bit SSE register",
                i < 4 ? "float/double arguments 1–4 in Win64 (xmm0 is also the return value). Volatile."
                      : i < 6 ? "Volatile (may be destroyed by a function call)."
                              : "Non-volatile in Win64: a function must restore it.",
                ""};
    }
    for (int i = 0; i < 16; i++) {
        std::string y = "ymm" + std::to_string(i);
        m[y] = {y + ": 256-bit AVX register", "Its low 128 bits are xmm" + std::to_string(i) +
                                                 ". Writing xmm with a v-prefixed instruction clears the upper half of ymm.", ""};
    }

    // ---- the extended set (hints_ext.inc)
    set("cmpxchg8b cmpxchg16b", {"cmpxchg16b m128",
                                 "Compares rdx:rax with 16 bytes of memory; if equal, writes rcx:rbx and ZF = 1, otherwise "
                                 "loads the memory into rdx:rax and ZF = 0. With lock it is atomic. The address is aligned to 16.",
                                 "ZF"});
    set("retf", {"retf | retf imm16", "Far return: pops the address and the code segment.", none});
    set("jcxz", {"jcxz label", "Jumps if cx = 0 (16-bit mode).", none});
    set("int1", {"int1", "Debug interrupt #DB (like single-stepping).", none});
    set("into", {"into", "Interrupt #OF if OF = 1 (not in 64-bit mode).", none});
    set("iret iretd iretq", {"iretq", "Returns from an interrupt handler: pops rip, cs, rflags, rsp, ss.", "all flags restored"});
    set("sysret", {"sysret", "Returns from a system call to user mode (kernel only).", "from r11"});
    set("sysenter sysexit", {"sysenter / sysexit", "Fast kernel entry and exit (32-bit systems).", "IF VM cleared"});
    set("cli", {"cli", "IF = 0: disable external interrupts. Privileged: raises an exception in a program.", "IF"});
    set("sti", {"sti", "IF = 1: enable interrupts. Privileged.", "IF"});
    set("pushfd popfd", {"pushfd / popfd", "32-bit versions of pushf/popf.", "popfd: all"});
    set("pusha popa pushad popad", {"pushad / popad", "Save / restore all general-purpose registers (16/32-bit code only).", none});
    set("movs", {"movs dest, src", "The general form of movsb/w/d/q with explicit operands.", none});
    set("lods", {"lods src", "The general form of lodsb/w/d/q.", none});
    set("stos", {"stos dest", "The general form of stosb/w/d/q.", none});
    set("scas", {"scas dest", "The general form of scasb/w/d/q.", arith});
    set("cmps", {"cmps a, b", "The general form of cmpsb/w/d/q.", arith});
    set("cmpsd", {"cmpsd | cmpsd xmm, xmm/m64, predicate",
                  "Without operands: a string instruction comparing dword [rsi] and [rdi] and advancing both. With xmm "
                  "operands: compares two doubles; the low 64 bits become all ones if the condition holds (cmpltsd, cmpeqsd…).",
                  arith});
    set("ins insb insw insd", {"insb", "Reads from port dx into [rdi] (privileged on Windows).", none});
    set("outs outsb outsw outsd", {"outsb", "Writes [rsi] to port dx (privileged on Windows).", none});
    set("in", {"in al/ax/eax, imm8 | dx", "Reads an I/O port. In a Windows user program: an exception.", none});
    set("out", {"out imm8 | dx, al/ax/eax", "Writes an I/O port (privileged).", none});
    set("xlat xlatb", {"xlatb", "al = [rbx + al]: translates a byte through a table.", none});
    set("rdtscp", {"rdtscp", "The cycle counter edx:eax and the processor number in ecx; waits for the previous instructions.", none});
    set("rdrand", {"rdrand r16/32/64", "A random number from the hardware generator; CF = 1 if one was returned.", "CF; the others = 0"});
    set("rdseed", {"rdseed r16/32/64", "A random seed from the hardware entropy source; CF = 1 if one was returned.",
                   "CF; the others = 0"});
    set("rdpmc", {"rdpmc", "Performance counter number ecx → edx:eax.", none});
    set("rdmsr", {"rdmsr", "Reads model-specific register ecx → edx:eax (kernel only).", none});
    set("wrmsr", {"wrmsr", "Writes edx:eax to model-specific register ecx (kernel only).", none});
    set("pause", {"pause", "A hint to the processor inside a spin-wait loop: saves power.", none});
    set("lfence", {"lfence", "Barrier: all earlier loads complete before later ones.", none});
    set("sfence", {"sfence", "Barrier for stores (especially non-temporal movnt*).", none});
    set("mfence", {"mfence", "A full memory barrier for loads and stores.", none});
    set("prefetch prefetchw prefetcht0 prefetcht1 prefetcht2 prefetchnta",
        {"prefetcht0 m8", "A hint to load a cache line early (t0 into all levels, nta bypassing the cache). Changes nothing.", none});
    set("clflush clflushopt", {"clflush m8", "Flushes the cache line with this address to memory.", none});
    set("ud0 ud1", {"ud1 r, r/m", "Invalid instructions (#UD exception).", none});
    set("andn", {"andn r, r1, r/m (BMI1)", "r = (NOT r1) AND r/m.", "SF ZF; OF = CF = 0"});
    set("bextr", {"bextr r, r/m, r2 (BMI1)", "Extracts bits: start in the low byte of r2, length in the next byte.", "ZF; CF = OF = 0"});
    set("blsi", {"blsi r, r/m (BMI1)", "Keeps the lowest 1 bit: x & -x.", "ZF SF; CF = (src ≠ 0)"});
    set("blsmsk", {"blsmsk r, r/m (BMI1)", "A mask up to and including the lowest 1 bit: x ^ (x - 1).", "SF; CF"});
    set("blsr", {"blsr r, r/m (BMI1)", "Clears the lowest 1 bit: x & (x - 1).", "ZF SF; CF"});
    set("bzhi", {"bzhi r, r/m, r2 (BMI2)", "Clears the high bits starting at the index in r2.", "ZF SF; CF"});
    set("pdep", {"pdep r, r1, r/m (BMI2)", "Deposits the low bits of r1 at the positions of the 1 bits of the mask r/m.", none});
    set("pext", {"pext r, r1, r/m (BMI2)", "Gathers the bits of r1 at the 1 bits of the mask r/m into the low bits.", none});
    set("rorx", {"rorx r, r/m, imm8 (BMI2)", "Rotates right without changing the flags.", none});
    set("sarx shlx shrx", {"shlx r, r/m, r2 (BMI2)", "Shift by r2 without changing the flags, three operands.", none});
    set("mulx", {"mulx r_hi, r_lo, r/m (BMI2)", "Unsigned rdx × r/m → r_hi:r_lo without changing the flags.", none});
    set("adcx", {"adcx r, r/m", "Add with carry changing only CF (for long arithmetic in two chains).", "CF"});
    set("adox", {"adox r, r/m", "Add with carry through OF, changing only OF.", "OF"});
    set("crc32", {"crc32 r32/64, r/m8/16/32/64 (SSE4.2)", "Accumulates a CRC-32C (Castagnoli polynomial) over the data.", none});
    set("movbe", {"movbe r, m | m, r", "Copies with reversed byte order (big-endian data).", none});
    set("endbr64 endbr32", {"endbr64", "Marks a valid target of an indirect jump (CET protection). Works as a nop.", none});
    set("xgetbv", {"xgetbv", "XCR register number ecx → edx:eax (checks that the OS supports AVX).", none});
    set("xsave xrstor", {"xsave m", "Saves / restores the extended processor state (x87, SSE, AVX) by the mask edx:eax.", none});
    set("lgdt lidt", {"lgdt m", "Loads a descriptor table (GDT / IDT). Kernel only.", none});
    set("sgdt sidt", {"sgdt m", "Stores the address and size of the GDT / IDT to memory.", none});
    set("invlpg", {"invlpg m", "Flushes the TLB entry of a page (kernel only).", none});
    set("wbinvd", {"wbinvd", "Writes back and invalidates all caches (kernel only).", none});
    set("emms", {"emms", "Frees the MMX registers for x87 (after mm0–mm7 instructions).", "FPU tags"});
    set("ldmxcsr", {"ldmxcsr m32", "Loads MXCSR: the rounding mode and the SSE exception masks.", none});
    set("stmxcsr", {"stmxcsr m32", "Stores MXCSR.", none});
    set("movnti", {"movnti m, r", "A store that bypasses the cache (non-temporal).", none});
    set("movntq maskmovq", {"movntq m64, mm", "A non-temporal store of an MMX register.", none});

    set("fist", {"fist m16/m32", "Stores st0 as an integer (rounded) without popping.", fpu});
    set("fisttp", {"fisttp m16/m32/m64", "Stores st0 as an integer, truncating, and pops (SSE3).", fpu});
    set("fsubr fsubrp fdivr fdivrp", {"fsubr st0, st(i)", "Reverse subtraction / division: st0 = st(i) − st0 (÷).", fpu});
    set("fiadd fisub fisubr fimul fidiv fidivr", {"fiadd m16/m32", "Arithmetic of st0 with an integer from memory.", fpu});
    set("fchs", {"fchs", "st0 = −st0.", fpu});
    set("fabs", {"fabs", "st0 = |st0|.", fpu});
    set("fsqrt", {"fsqrt", "st0 = √st0.", fpu});
    set("fsin fcos", {"fsin", "st0 = sin(st0) (radians).", fpu});
    set("fsincos", {"fsincos", "st0 = cos(x), st1 = sin(x).", fpu});
    set("fptan", {"fptan", "st0 = tan(st0), then pushes 1.0.", fpu});
    set("fpatan", {"fpatan", "st1 = arctan(st1 / st0), pops st0.", fpu});
    set("fprem fprem1", {"fprem", "The remainder of st0 divided by st1.", fpu});
    set("frndint", {"frndint", "Rounds st0 to an integer by the current mode.", fpu});
    set("fscale", {"fscale", "st0 = st0 · 2^trunc(st1).", fpu});
    set("fxtract", {"fxtract", "Splits st0 into exponent and mantissa.", fpu});
    set("fcom fcomp fcompp fucom fucomp fucompp ficom ficomp ftst",
        {"fcomp st(i) | m32/m64", "Compares st0 with the operand; the result goes to C0, C2, C3 of the status word "
                                  "(fstsw ax / sahf).", fpu});
    set("fcomi fcomip fucomi fucomip", {"fcomip st0, st(i)",
                                        "Compares and sets ZF, PF, CF directly (like an unsigned comparison: ja/jb).", "ZF PF CF"});
    set("fxam", {"fxam", "Classifies st0 (zero, infinity, NaN…) into C0–C3.", fpu});
    set("fld1 fldz fldpi fldl2e fldl2t fldlg2 fldln2", {"fldpi", "Loads a constant: 1, 0, π, log2(e), log2(10), log10(2), ln 2.", fpu});
    set("fclex fnclex", {"fclex", "Clears the FPU exception flags.", fpu});
    set("fstsw fnstsw", {"fstsw ax", "FPU status word → ax (after fcom: sahf and the ordinary jumps).", none});
    set("fstcw fnstcw", {"fstcw m16", "Stores the FPU control word (rounding, precision).", none});
    set("fldcw", {"fldcw m16", "Loads the FPU control word.", none});
    set("fwait wait", {"fwait", "Waits for the FPU instruction to finish and handles exceptions.", none});
    set("ffree", {"ffree st(i)", "Marks an FPU stack register empty.", none});
    set("fincstp fdecstp", {"fincstp", "Moves the FPU stack top pointer.", fpu});
    set("fxch", {"fxch st(i)", "Swaps st0 and st(i).", fpu});
    set("fxsave fxrstor", {"fxsave m512", "Saves / restores the x87 and SSE state (512 bytes, address a multiple of 16).", none});
    set("f2xm1", {"f2xm1", "st0 = 2^st0 − 1 (for −1 ≤ st0 ≤ 1).", fpu});
    set("fyl2x fyl2xp1", {"fyl2x", "st1 = st1 · log2(st0), pops st0.", fpu});
    set("fnop", {"fnop", "Does nothing (FPU).", none});
    set("fsave fnsave frstor fstenv fnstenv fldenv", {"fsave m", "Saves / restores the FPU state or environment.",
                                                      "resets the FPU (fsave)"});

    set("movhps movlps movhpd movlpd", {"movhps xmm, m64 | m64, xmm", "Copies the high (h) or low (l) 64 bits of xmm.", none});
    set("movhlps movlhps", {"movhlps xmm1, xmm2", "Copies the high half of xmm2 to the low half of xmm1 (lh: the other way).", none});
    set("movmskps movmskpd", {"movmskps r32, xmm", "The sign bits of the elements → the low bits of a register (after cmpps).", none});
    set("movntps movntpd movntdq movntdqa", {"movntps m128, xmm", "Stores / loads 16 bytes bypassing the cache.", none});
    set("rcpps rcpss", {"rcpps xmm, xmm/m128", "Approximate 1/x (about 12 bits of precision).", none});
    set("rsqrtps rsqrtss", {"rsqrtps xmm, xmm/m128", "Approximate 1/√x (about 12 bits of precision).", none});
    set("andnps andnpd pandn", {"andnps xmm, xmm/m128", "Bitwise (NOT dest) AND src.", none});
    set("orps orpd por", {"orps xmm, xmm/m128", "Bitwise OR of 128 bits.", none});
    set("cmpps cmppd cmpss", {"cmpps xmm, xmm/m128, predicate",
                              "Compares elements: the result is a mask of ones (true) or zeros. Predicate: 0 = eq, 1 = lt, "
                              "2 = le, 4 = neq… (NASM: cmpltps).", none});
    set("shufps shufpd", {"shufps xmm1, xmm2/m128, imm8", "Shuffle: the two low elements from xmm1, the two high from xmm2, "
                                                          "chosen by the bits of imm8.", none});
    set("unpcklps unpckhps unpcklpd unpckhpd", {"unpcklps xmm1, xmm2/m128",
                                                "Interleaves the low (l) or high (h) elements of two registers.", none});
    set("cvtss2si", {"cvtss2si r32/64, xmm/m32", "float → integer, rounding by the current mode.", none});
    set("cvtps2pd", {"cvtps2pd xmm, xmm/m64", "Two low floats → two doubles.", none});
    set("cvtpd2ps", {"cvtpd2ps xmm, xmm/m128", "Two doubles → two floats.", none});
    set("cvtdq2ps", {"cvtdq2ps xmm, xmm/m128", "Four int32 → four floats.", none});
    set("cvtps2dq cvttps2dq", {"cvtps2dq xmm, xmm/m128", "Four floats → int32 (tt: truncating).", none});
    set("cvtdq2pd", {"cvtdq2pd xmm, xmm/m64", "Two int32 → two doubles.", none});
    set("cvtpd2dq cvttpd2dq", {"cvtpd2dq xmm, xmm/m128", "Two doubles → two int32 (tt: truncating).", none});
    set("blendps blendpd", {"blendps xmm1, xmm2/m128, imm8", "Takes the elements of xmm2 where the bit of imm8 is 1 (SSE4.1).", none});
    set("blendvps blendvpd pblendvb", {"blendvps xmm1, xmm2/m128, xmm0", "Blends by the sign bits of the mask in xmm0 (SSE4.1).", none});
    set("pblendw", {"pblendw xmm1, xmm2/m128, imm8", "Blends 16-bit elements by the bits of imm8.", none});
    set("dpps dppd", {"dpps xmm1, xmm2/m128, imm8", "Dot product of vectors (which elements: by imm8).", none});
    set("roundps roundpd roundss roundsd", {"roundsd xmm1, xmm2/m64, imm8",
                                            "Rounds: imm8 = 0 to nearest, 1 down, 2 up, 3 toward zero (SSE4.1).", none});
    set("haddps haddpd hsubps hsubpd", {"haddps xmm1, xmm2/m128", "Horizontal addition of neighbouring elements (SSE3).", none});
    set("addsubps addsubpd", {"addsubps xmm1, xmm2/m128", "Even elements are subtracted, odd ones added (SSE3).", none});
    set("movddup movshdup movsldup", {"movddup xmm1, xmm2/m64", "Duplicates elements (the low double / even or odd floats).", none});
    set("lddqu", {"lddqu xmm, m128", "An unaligned load of 16 bytes (SSE3).", none});
    set("insertps", {"insertps xmm1, xmm2/m32, imm8", "Inserts one float at the position given by imm8, may zero the others.", none});
    set("extractps", {"extractps r/m32, xmm, imm8", "Extracts one float (as 32 bits).", none});

    struct Size {
        const char* suffix;
        const char* what;
    };
    const Size sizes[] = {{"b", "16 bytes"}, {"w", "8 words (16 bits)"}, {"d", "4 double words (32 bits)"},
                          {"q", "2 quad words (64 bits)"}};
    for (const Size& s : sizes) {
        std::string sf = s.suffix;
        m["padd" + sf] = {"padd" + sf + " xmm, xmm/m128", std::string("Packed integer addition: ") + s.what +
                                                             " at once, wrapping around.", none};
        m["psub" + sf] = {"psub" + sf + " xmm, xmm/m128", std::string("Packed integer subtraction: ") + s.what + ".", none};
        m["pcmpeq" + sf] = {"pcmpeq" + sf + " xmm, xmm/m128",
                            std::string("Compare for equality (") + s.what + "): an element becomes all ones if equal.", none};
        m["pcmpgt" + sf] = {"pcmpgt" + sf + " xmm, xmm/m128",
                            std::string("Signed greater-than comparison (") + s.what + "): a mask of ones.", none};
    }
    for (const char* sf : {"sb", "sw"}) {
        m[std::string("padd") + sf] = {std::string("padd") + sf + " xmm, xmm/m128",
                                       "Signed addition with saturation (the result is clamped instead of overflowing).", none};
        m[std::string("psub") + sf] = {std::string("psub") + sf + " xmm, xmm/m128", "Signed subtraction with saturation.", none};
    }
    for (const char* sf : {"usb", "usw"}) {
        m[std::string("padd") + sf] = {std::string("padd") + sf + " xmm, xmm/m128", "Unsigned addition with saturation.", none};
        m[std::string("psub") + sf] = {std::string("psub") + sf + " xmm, xmm/m128",
                                       "Unsigned subtraction with saturation (not below 0).", none};
    }
    set("pmullw pmulld", {"pmullw xmm, xmm/m128", "Packed integer multiplication, the low halves of the results.", none});
    set("pmulhw pmulhuw", {"pmulhw xmm, xmm/m128", "Multiplies 16-bit elements, the high 16 bits of the results (uw: unsigned).", none});
    set("pmuludq pmuldq", {"pmuludq xmm, xmm/m128", "Multiplies the even 32-bit elements → 64-bit products.", none});
    set("pmaddwd", {"pmaddwd xmm, xmm/m128", "Multiplies 16-bit pairs and adds neighbouring products into 32 bits.", none});
    for (const char* op : {"psll", "psrl", "psra"}) {
        std::string o = op;
        std::string what = o == "psll" ? "left" : o == "psrl" ? "right (logical)" : "right keeping the sign";
        for (const char* sf : {"w", "d", "q"}) {
            if (o == "psra" && std::string(sf) == "q") continue;
            m[o + sf] = {o + sf + " xmm, xmm/imm8", "Shifts each element " + what + ".", none};
        }
    }
    set("pslldq psrldq", {"pslldq xmm, imm8", "Shifts the whole register by imm8 bytes (left / right).", none});
    set("pshufb", {"pshufb xmm1, xmm2/m128", "Shuffles the bytes of xmm1 by the indexes in xmm2 (bit 7 = zero) — SSSE3.", none});
    set("pshufd", {"pshufd xmm1, xmm2/m128, imm8", "Shuffles 4 double words by the 2-bit fields of imm8.", none});
    set("pshufhw pshuflw", {"pshufhw xmm1, xmm2/m128, imm8", "Shuffles the high (h) or low (l) 4 words.", none});
    for (const char* sf : {"bw", "wd", "dq", "qdq"}) {
        m[std::string("punpckl") + sf] = {std::string("punpckl") + sf + " xmm, xmm/m128",
                                          "Interleaves the low elements of two registers.", none};
        m[std::string("punpckh") + sf] = {std::string("punpckh") + sf + " xmm, xmm/m128",
                                          "Interleaves the high elements of two registers.", none};
    }
    set("packsswb packssdw", {"packsswb xmm, xmm/m128", "Packs elements into half the size with signed saturation.", none});
    set("packuswb packusdw", {"packuswb xmm, xmm/m128", "Packs with unsigned saturation.", none});
    set("pmovmskb", {"pmovmskb r32, xmm", "The top bits of 16 bytes → 16 bits of a register (byte search: pcmpeqb + "
                                          "pmovmskb + bsf).", none});
    set("pextrb pextrw pextrd pextrq", {"pextrd r/m32, xmm, imm8", "Extracts element number imm8 into a register or memory.", none});
    set("pinsrb pinsrw pinsrd pinsrq", {"pinsrd xmm, r/m32, imm8", "Inserts an element at position imm8.", none});
    set("pminsb pminsw pminsd pminub pminuw pminud", {"pminsd xmm, xmm/m128", "Element-wise minimum (s signed, u unsigned).", none});
    set("pmaxsb pmaxsw pmaxsd pmaxub pmaxuw pmaxud", {"pmaxsd xmm, xmm/m128", "Element-wise maximum (s signed, u unsigned).", none});
    set("pabsb pabsw pabsd", {"pabsd xmm, xmm/m128", "The absolute value of each element (SSSE3).", none});
    set("palignr", {"palignr xmm1, xmm2/m128, imm8", "Concatenates xmm1:xmm2 and shifts right by imm8 bytes.", none});
    set("ptest", {"ptest xmm1, xmm2/m128", "ZF = 1 if (xmm1 AND xmm2) = 0; CF = 1 if (NOT xmm1 AND xmm2) = 0.", "ZF CF"});
    set("pavgb pavgw", {"pavgb xmm, xmm/m128", "Unsigned average rounding up.", none});
    set("psadbw", {"psadbw xmm, xmm/m128", "The sum of absolute byte differences (for comparing blocks).", none});
    for (const char* sx : {"s", "z"}) {
        for (const char* sf : {"bw", "bd", "bq", "wd", "wq", "dq"}) {
            std::string n = std::string("pmov") + sx + "x" + sf;
            m[n] = {n + " xmm, xmm/m", std::string("Extends the low elements to a larger size ") +
                                           (*sx == 's' ? "with the sign" : "with zeros") + " (SSE4.1).", none};
        }
    }
    set("pcmpestri pcmpestrm pcmpistri pcmpistrm", {"pcmpistri xmm1, xmm2/m128, imm8",
                                                    "String comparison (SSE4.2): searches characters, substrings, ranges; "
                                                    "the mode is in imm8.", "CF ZF SF OF"});
    set("aesenc aesenclast aesdec aesdeclast", {"aesenc xmm1, xmm2/m128", "One round of AES encryption / decryption.", none});
    set("aesimc aeskeygenassist", {"aeskeygenassist xmm1, xmm2/m128, imm8", "AES key preparation.", none});
    set("pclmulqdq", {"pclmulqdq xmm1, xmm2/m128, imm8", "Carry-less multiplication 64×64 → 128 bits (CRC, GCM).", none});

    set("vzeroupper", {"vzeroupper", "Clears the upper halves of all ymm. Use it before ordinary SSE code and C functions.", none});
    set("vzeroall", {"vzeroall", "Clears all ymm registers.", none});
    set("vbroadcastss vbroadcastsd vbroadcastf128 vbroadcasti128 vpbroadcastb vpbroadcastw vpbroadcastd vpbroadcastq",
        {"vbroadcastss ymm, m32 | xmm", "Copies one value into all elements of a register.", none});
    set("vperm2f128 vperm2i128", {"vperm2f128 ymm1, ymm2, ymm3/m256, imm8", "Picks 128-bit halves from two registers.", none});
    set("vpermd vpermps", {"vpermd ymm1, ymm2, ymm3/m256", "Permutes 8 elements of 32 bits by the indexes in ymm2 (AVX2).", none});
    set("vpermq vpermpd", {"vpermq ymm1, ymm2/m256, imm8", "Permutes 4 elements of 64 bits by imm8 (AVX2).", none});
    set("vpermilps vpermilpd", {"vpermilps ymm1, ymm2, ymm3/imm8", "Permutes within each 128-bit half.", none});
    set("vinsertf128 vinserti128", {"vinsertf128 ymm1, ymm2, xmm3/m128, imm8", "Inserts 128 bits into a half of ymm (imm8 = 0/1).",
                                    none});
    set("vextractf128 vextracti128", {"vextractf128 xmm/m128, ymm, imm8", "Extracts a half of ymm.", none});
    set("vmaskmovps vmaskmovpd vpmaskmovd vpmaskmovq", {"vmaskmovps ymm1, ymm2, m256",
                                                        "Loads / stores only the elements whose mask sign is 1.", none});
    set("vgatherdps vgatherdpd vgatherqps vgatherqpd vpgatherdd vpgatherdq vpgatherqd vpgatherqq",
        {"vgatherdps ymm1, [base + ymm2*scale], ymm3", "Gathers elements from memory by a vector of indexes (AVX2), mask in ymm3.",
         none});
    set("vpsllvd vpsllvq vpsrlvd vpsrlvq vpsravd", {"vpsllvd ymm1, ymm2, ymm3/m256",
                                                    "Shifts each element by its own number of bits (AVX2).", none});
    set("vtestps vtestpd", {"vtestps ymm1, ymm2/m256", "Like ptest, but on the sign bits only.", "ZF CF"});
    set("vcvtph2ps vcvtps2ph", {"vcvtph2ps ymm, xmm/m128", "Half precision (float16) ↔ float.", none});
    for (const char* op : {"fmadd", "fmsub", "fnmadd", "fnmsub", "fmaddsub", "fmsubadd"}) {
        for (const char* order : {"132", "213", "231"}) {
            for (const char* t : {"ps", "pd", "ss", "sd"}) {
                std::string n = std::string("v") + op + order + t;
                std::string o = op;
                std::string what = o == "fmadd" ? "a·b + c" : o == "fmsub" ? "a·b − c" : o == "fnmadd" ? "−(a·b) + c"
                                 : o == "fnmsub" ? "−(a·b) − c" : o == "fmaddsub" ? "a·b ± c alternately" : "a·b ∓ c alternately";
                m[n] = {n + " xmm1, xmm2, xmm3/m",
                        "Fused multiply-add (FMA3): " + what + " with a single rounding. The digits " + order +
                            " tell which operands are multiplied (1 = dest): 213 = xmm1·xmm2 + xmm3.", none};
            }
        }
    }

    // ---- examples
    auto example = [&](const char* name, const char* code) {
        auto it = m.find(name);
        if (it != m.end()) it->second.example = code;
    };
    example("mov", "mov rax, [rbx + 8]  ; load a qword from memory");
    example("lea", "lea rcx, [rel msg]  ; the address of a string for printf");
    example("add", "add rax, rbx");
    example("cmp", "cmp rax, 10 / jl .less");
    example("call", "sub rsp, 40 / lea rcx, [msg] / call printf / add rsp, 40");
    example("div", "xor edx, edx / mov rcx, 10 / div rcx  ; rax = rax / 10, rdx = remainder");
    example("idiv", "cqo / idiv rbx");
    example("imul", "imul rax, rbx, 3");
    example("push", "push rbx  ; save a non-volatile register");
    example("loop", "mov ecx, 5 / .l: … / loop .l");
    example("xor", "xor eax, eax  ; rax = 0");
    example("test", "test rax, rax / jz .zero");
    example("shl", "shl rax, 3  ; rax *= 8");
    example("movzx", "movzx eax, byte [rsi]");
    example("cvtsi2sd", "cvtsi2sd xmm0, rax");
    example("addsd", "movsd xmm0, [x] / addsd xmm0, [y]");
    example("rep", "lea rdi, [buf] / mov ecx, 256 / xor eax, eax / rep stosb");
    example("cmovl", "cmp rax, rbx / cmovl rax, rbx  ; rax = max(rax, rbx)");
}

// C runtime and WinAPI functions: where the arguments go and what to know.
struct FunctionInfoEn {
    const char* names;
    const char* args;
    const char* note;
};
const FunctionInfoEn kFunctionsEn[] = {
    {"printf", "rcx = format string, rdx, r8, r9 = arguments 1–3, then on the stack above the shadow space ([rsp+32]…)",
     "Returns the number of characters printed (eax). A double goes both in xmm and in the ordinary register "
     "(movq rdx, xmm1). Needs extern printf and a link preset with the C runtime."},
    {"scanf", "rcx = format (\"%lld\"), rdx, r8, r9 = addresses of the variables",
     "Returns the number of values read (eax); the variable is a dq for %lld, a dd for %d."},
    {"puts", "rcx = the string's address (zero-terminated)", "Prints the string and a line break."},
    {"putchar", "ecx = the character", "Prints one character."},
    {"getchar", "no arguments", "Returns the character code in eax, or -1 (EOF)."},
    {"fgets", "rcx = buffer, edx = size, r8 = FILE*", "Reads a line including \\n; rax = buf, or 0 at the end of the file."},
    {"sprintf", "rcx = buffer, rdx = format, r8, r9 = arguments", "Like printf, but writes into the buffer."},
    {"sscanf", "rcx = string, rdx = format, r8, r9 = addresses", "Like scanf, but reads from a string."},
    {"fprintf", "rcx = FILE*, rdx = format, r8, r9 = arguments", ""},
    {"fscanf", "rcx = FILE*, rdx = format, r8, r9 = addresses", ""},
    {"fopen", "rcx = file name, rdx = mode (\"r\", \"w\", \"a\", \"rb\"…)", "rax = FILE*, or 0 on error."},
    {"fclose", "rcx = FILE*", ""},
    {"malloc", "rcx = size in bytes", "rax = the block's address, or 0. Release it with free."},
    {"calloc", "rcx = count, rdx = element size", "The memory is zeroed."},
    {"realloc", "rcx = the old block, rdx = the new size", ""},
    {"free", "rcx = the block's address", ""},
    {"strlen", "rcx = string", "rax = the length without the zero."},
    {"strcpy", "rcx = destination, rdx = source", ""},
    {"strcat", "rcx = what to append to, rdx = what to append", ""},
    {"strcmp", "rcx = a, rdx = b", "eax < 0, 0 or > 0."},
    {"memcpy", "rcx = destination, rdx = source, r8 = bytes", ""},
    {"memset", "rcx = address, edx = byte value, r8 = count", ""},
    {"rand", "no arguments", "eax = 0…32767. The seed is set with srand."},
    {"srand", "ecx = the seed (for example time(0))", ""},
    {"time", "rcx = 0 or the address of a qword", "rax = seconds since 1970."},
    {"exit", "ecx = exit code", "Ends the program (flushes the output buffers)."},
    {"ExitProcess", "ecx = exit code",
     "Ends the process at once (kernel32). printf buffers are not flushed: after printf, rather ret from main."},
    {"GetStdHandle", "ecx = -10 (input), -11 (output), -12 (errors)", "rax = the console handle for ReadFile / WriteFile."},
    {"WriteFile", "rcx = handle, rdx = buffer, r8d = bytes, r9 = address of a DWORD, 5th argument (0) in qword [rsp+32]",
     "Before call: sub rsp, 40 (or more): shadow space plus room for the 5th argument."},
    {"ReadFile", "rcx = handle, rdx = buffer, r8d = size, r9 = address of a DWORD, qword [rsp+32] = 0", ""},
    {"WriteConsoleA WriteConsoleW", "rcx = handle, rdx = text, r8d = characters, r9 = address of a DWORD, qword [rsp+32] = 0", ""},
    {"ReadConsoleA", "rcx = input handle, rdx = buffer, r8d = size, r9 = address of a DWORD, qword [rsp+32] = 0", ""},
    {"MessageBoxA", "rcx = 0, rdx = text, r8 = caption, r9d = buttons (0: OK)", "Needs user32.lib."},
    {"Sleep", "ecx = milliseconds", ""},
    {"GetTickCount", "no arguments", "eax = milliseconds since Windows started."},
    {"GetLastError", "no arguments", "eax = the code of the last WinAPI error."},
    {"CreateFileA", "rcx = name, edx = access (0x80000000 read, 0x40000000 write), r8d = share, r9 = 0; on the stack: "
                    "[rsp+32] = creation (2 create, 3 open), [rsp+40] = flags (0x80), [rsp+48] = 0",
     "rax = handle, or -1 (INVALID_HANDLE_VALUE). Needs sub rsp, 56 or more."},
    {"CloseHandle", "rcx = handle", ""},
    {"VirtualAlloc", "rcx = 0, rdx = size, r8d = 0x3000 (MEM_COMMIT|MEM_RESERVE), r9d = 4 (PAGE_READWRITE)", ""},
    {"VirtualFree", "rcx = address, rdx = 0, r8d = 0x8000", ""},
    {"GetProcessHeap", "no arguments", "rax = the process heap for HeapAlloc."},
    {"HeapAlloc", "rcx = heap, edx = 8 (zero the memory), r8 = size", ""},
    {"HeapFree", "rcx = heap, edx = 0, r8 = block", ""},
    {"SetConsoleOutputCP", "ecx = code page (65001 is UTF-8)", ""},
};

bool EnglishFunction(const std::string& name, std::string& args, std::string& note) {
    for (const FunctionInfoEn& f : kFunctionsEn) {
        std::istringstream in(f.names);
        std::string n;
        while (in >> n) {
            if (n != name) continue;
            args = f.args;
            note = f.note;
            return true;
        }
    }
    return false;
}

}
}
