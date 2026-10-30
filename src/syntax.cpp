#include "common.h"

#include <sstream>
#include <unordered_set>

namespace {
using WordSet = std::unordered_set<std::string>;

void AddWords(WordSet& set, const char* words) {
    std::istringstream in(words);
    std::string w;
    while (in >> w) set.insert(w);
}

struct Tables {
    WordSet registers, instructions, directives, dataDirectives, keywords, prefixes;

    Tables() {
        AddWords(registers, R"(
            rax rbx rcx rdx rsi rdi rbp rsp eax ebx ecx edx esi edi ebp esp ax bx cx dx si di bp sp
            al ah bl bh cl ch dl dh sil dil bpl spl cs ds es fs gs ss rip eip ip st)");
        for (int i = 8; i <= 15; i++) {
            std::string r = "r" + std::to_string(i);
            for (const char* suffix : {"", "d", "w", "b", "l"}) registers.insert(r + suffix);
        }
        for (int i = 0; i < 32; i++) {
            for (const char* p : {"xmm", "ymm", "zmm"}) registers.insert(p + std::to_string(i));
        }
        for (int i = 0; i < 16; i++) {
            registers.insert("cr" + std::to_string(i));
            registers.insert("dr" + std::to_string(i));
        }
        for (int i = 0; i < 8; i++) {
            for (const char* p : {"k", "mm", "st"}) registers.insert(p + std::to_string(i));
        }
        for (int i = 0; i < 4; i++) registers.insert("bnd" + std::to_string(i));

        AddWords(directives, R"(
            section segment global extern common static required bits use16 use32 use64 default absolute org cpu
            float align alignb struc endstruc istruc iend at warning map sectalign import export)");
        AddWords(dataDirectives, "db dw dd dq dt do dy dz resb resw resd resq rest reso resy resz equ times incbin");
        AddWords(keywords, R"(
            byte word dword qword tword oword yword zword near far short rel abs strict nosplit wrt seg ptr to
            __float8__ __float16__ __float32__ __float64__ __float80m__ __float80e__ __float128l__ __float128h__
            __utf16__ __utf32__)");
        AddWords(prefixes, "rep repe repz repne repnz lock xacquire xrelease bnd o16 o32 o64 a16 a32 a64");

        AddWords(instructions, R"(
            mov movzx movsx movsxd lea push pop add sub adc sbb inc dec neg not and or xor test cmp mul imul
            div idiv shl shr sal sar rol ror rcl rcr shld shrd bt bts btr btc bsf bsr bswap xchg xadd cmpxchg
            cmpxchg8b cmpxchg16b call ret retn retf jmp loop loope loopne loopz loopnz jcxz jecxz jrcxz
            int int1 int3 into iret iretd iretq syscall sysret sysenter sysexit nop hlt cli sti cld std clc stc
            cmc lahf sahf pushf popf pushfd popfd pushfq popfq pusha popa pushad popad enter leave cbw cwde
            cdqe cwd cdq cqo movs movsb movsw movsd movsq lods lodsb lodsw lodsd lodsq stos stosb stosw stosd
            stosq scas scasb scasw scasd scasq cmps cmpsb cmpsw cmpsd cmpsq ins insb insw insd outs outsb outsw
            outsd in out xlat xlatb cpuid rdtsc rdtscp rdrand rdseed rdpmc rdmsr wrmsr pause lfence sfence
            mfence prefetch prefetchw prefetcht0 prefetcht1 prefetcht2 prefetchnta clflush clflushopt ud2 ud0
            ud1 popcnt lzcnt tzcnt andn bextr blsi blsmsk blsr bzhi pdep pext rorx sarx shlx shrx mulx adcx
            adox crc32 movbe endbr64 endbr32 xgetbv xsave xrstor lgdt lidt sgdt sidt invlpg wbinvd
            fld fst fstp fild fist fistp fisttp fadd faddp fsub fsubp fsubr fsubrp fmul fmulp fdiv fdivp fdivr
            fdivrp fchs fabs fsqrt fsin fcos fsincos fptan fpatan fprem fprem1 frndint fscale fxtract fcom
            fcomp fcompp fcomi fcomip fucom fucomp fucompp fucomi fucomip ftst fxam fld1 fldz fldpi fldl2e
            fldl2t fldlg2 fldln2 finit fninit fclex fnclex fstsw fnstsw fstcw fnstcw fldcw fwait wait ffree
            fincstp fdecstp fxch fxsave fxrstor fiadd fisub fisubr fimul fidiv fidivr ficom ficomp f2xm1 fyl2x
            fyl2xp1 fnop fsave fnsave frstor fstenv fnstenv fldenv emms ldmxcsr stmxcsr movd movq movnti movntq
            maskmovq vzeroupper vzeroall vbroadcastss vbroadcastsd vbroadcastf128 vbroadcasti128 vpbroadcastb
            vpbroadcastw vpbroadcastd vpbroadcastq vperm2f128 vperm2i128 vpermd vpermq vpermps vpermpd
            vpermilps vpermilpd vinsertf128 vextractf128 vinserti128 vextracti128 vmaskmovps vmaskmovpd
            vpmaskmovd vpmaskmovq vgatherdps vgatherdpd vgatherqps vgatherqpd vpgatherdd vpgatherdq vpgatherqd
            vpgatherqq vpsllvd vpsllvq vpsrlvd vpsrlvq vpsravd vtestps vtestpd vcvtph2ps vcvtps2ph)");

        WordSet sse;
        AddWords(sse, R"(
            movaps movups movapd movupd movss movsd movdqa movdqu movhps movlps movhpd movlpd movhlps movlhps
            movmskps movmskpd movntps movntpd movntdq movntdqa addps addpd addss addsd subps subpd subss subsd
            mulps mulpd mulss mulsd divps divpd divss divsd sqrtps sqrtpd sqrtss sqrtsd maxps maxpd maxss maxsd
            minps minpd minss minsd rcpps rcpss rsqrtps rsqrtss andps andpd andnps andnpd orps orpd xorps xorpd
            cmpps cmppd cmpss cmpsd comiss comisd ucomiss ucomisd shufps shufpd unpcklps unpckhps unpcklpd
            unpckhpd cvtsi2ss cvtsi2sd cvtss2si cvtsd2si cvttss2si cvttsd2si cvtss2sd cvtsd2ss cvtps2pd cvtpd2ps
            cvtdq2ps cvtps2dq cvttps2dq cvtdq2pd cvtpd2dq cvttpd2dq paddb paddw paddd paddq paddsb paddsw
            paddusb paddusw psubb psubw psubd psubq psubsb psubsw psubusb psubusw pmullw pmulhw pmulhuw pmulld
            pmuludq pmuldq pmaddwd pand pandn por pxor pcmpeqb pcmpeqw pcmpeqd pcmpeqq pcmpgtb pcmpgtw pcmpgtd
            pcmpgtq psllw pslld psllq psrlw psrld psrlq psraw psrad pslldq psrldq pshufb pshufd pshufhw pshuflw
            punpcklbw punpcklwd punpckldq punpcklqdq punpckhbw punpckhwd punpckhdq punpckhqdq packsswb packssdw
            packuswb packusdw pmovmskb pextrb pextrw pextrd pextrq pinsrb pinsrw pinsrd pinsrq pminsb pminsw
            pminsd pmaxsb pmaxsw pmaxsd pminub pminuw pminud pmaxub pmaxuw pmaxud pabsb pabsw pabsd palignr
            ptest pblendw blendps blendpd blendvps blendvpd pblendvb dpps dppd roundps roundpd roundss roundsd
            haddps haddpd hsubps hsubpd addsubps addsubpd movddup movshdup movsldup lddqu pavgb pavgw psadbw
            pmovsxbw pmovsxbd pmovsxbq pmovsxwd pmovsxwq pmovsxdq pmovzxbw pmovzxbd pmovzxbq pmovzxwd pmovzxwq
            pmovzxdq insertps extractps pcmpestri pcmpestrm pcmpistri pcmpistrm aesenc aesenclast aesdec
            aesdeclast aesimc aeskeygenassist pclmulqdq)");
        for (const std::string& s : sse) {
            instructions.insert(s);
            instructions.insert("v" + s);
        }
        for (const char* op : {"fmadd", "fmsub", "fnmadd", "fnmsub", "fmaddsub", "fmsubadd"}) {
            for (const char* order : {"132", "213", "231"}) {
                for (const char* t : {"ps", "pd", "ss", "sd"}) instructions.insert(std::string("v") + op + order + t);
            }
        }
        std::istringstream cc("o no b c nae ae nb nc e z ne nz be na a nbe s ns p pe np po l nge ge nl le ng g nle");
        std::string c;
        while (cc >> c) {
            instructions.insert("j" + c);
            instructions.insert("cmov" + c);
            instructions.insert("set" + c);
        }
    }
};

const Tables& T() {
    static Tables t;
    return t;
}

bool IsAlpha(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c >= 0x80;  // UTF-8 bytes count as letters
}
bool IsDigit(unsigned char c) { return c >= '0' && c <= '9'; }
bool IsIdentStart(unsigned char c) { return IsAlpha(c) || c == '_' || c == '.' || c == '?' || c == '@'; }
bool IsIdentPart(unsigned char c) {
    return IsAlpha(c) || IsDigit(c) || c == '_' || c == '.' || c == '$' || c == '#' || c == '@' || c == '~' || c == '?';
}

std::string LowerAscii(const char* s, int n) {
    std::string out(s, n);
    for (char& ch : out) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
    return out;
}

int SkipBlanks(const char* s, int n, int i) {
    while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
    return i;
}

std::string WordAt(const char* s, int n, int i) {
    int j = i;
    while (j < n && IsIdentPart(static_cast<unsigned char>(s[j]))) j++;
    return LowerAscii(s + i, j - i);
}
}

void Tokenize(const char* s, int n, std::vector<Token>& out) {
    out.clear();
    const Tables& t = T();
    int i = 0;
    bool statementStart = true;
    while (i < n) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            i++;
            continue;
        }
        if (c == ';') {
            out.push_back({i, n, T_COMMENT});
            break;
        }
        if (c == '\'' || c == '"' || c == '`') {
            int j = i + 1;
            while (j < n) {
                char d = s[j];
                if (c == '`' && d == '\\') {
                    j += 2;
                    continue;
                }
                j++;
                if (d == static_cast<char>(c)) break;
            }
            j = std::min(j, n);
            out.push_back({i, j, T_STRING});
            i = j;
            statementStart = false;
            continue;
        }
        if (c == '%') {
            int j = i + 1;
            while (j < n && (s[j] == '%' || s[j] == '$' || s[j] == '+' || (s[j] == '-' && j == i + 1))) j++;
            while (j < n && IsIdentPart(static_cast<unsigned char>(s[j]))) j++;
            if (j == i + 1) {  // a lone '%' is the modulo operator
                i++;
                statementStart = false;
                continue;
            }
            out.push_back({i, j, T_PREPROCESSOR});
            i = j;
            statementStart = false;
            continue;
        }
        if (IsDigit(c) || (c == '$' && i + 1 < n && IsDigit(static_cast<unsigned char>(s[i + 1])))) {
            int j = i + 1;
            while (j < n && (IsAlpha(static_cast<unsigned char>(s[j])) || IsDigit(static_cast<unsigned char>(s[j]))
                             || s[j] == '_' || s[j] == '.')) j++;
            out.push_back({i, j, T_NUMBER});
            i = j;
            statementStart = false;
            continue;
        }
        if (c == '$') {
            int j = i + 1;
            if (j < n && IsIdentStart(static_cast<unsigned char>(s[j]))) {
                while (j < n && IsIdentPart(static_cast<unsigned char>(s[j]))) j++;  // "$name" escapes a keyword
            } else {
                if (j < n && s[j] == '$') j++;
                out.push_back({i, j, T_NUMBER});
            }
            i = j;
            statementStart = false;
            continue;
        }
        if (IsIdentStart(c)) {
            int j = i + 1;
            while (j < n && IsIdentPart(static_cast<unsigned char>(s[j]))) j++;
            std::string word = LowerAscii(s + i, j - i);
            int k = SkipBlanks(s, n, j);
            if (statementStart && k < n && s[k] == ':') {
                out.push_back({i, k + 1, T_LABEL});
                i = k + 1;
                continue;  // an instruction may follow the label
            }
            TokenKind type;
            bool keepStart = false;
            if (t.registers.count(word)) {
                type = T_REGISTER;
            } else if (statementStart && t.prefixes.count(word)) {
                type = T_INSTRUCTION;
                keepStart = true;
            } else if (statementStart && t.instructions.count(word)) {
                type = T_INSTRUCTION;
            } else if (t.directives.count(word) || t.dataDirectives.count(word)) {
                type = T_DIRECTIVE;
            } else if (t.keywords.count(word)) {
                type = T_KEYWORD;
            } else if (statementStart && (i == 0 || t.dataDirectives.count(WordAt(s, n, k)))) {
                type = T_LABEL;  // "msg db ..." or "SIZE equ 10"
                keepStart = true;
            } else {
                type = T_DEFAULT;
            }
            if (type != T_DEFAULT) out.push_back({i, j, type});
            statementStart = keepStart;
            i = j;
            continue;
        }
        if (c != '[') statementStart = false;
        i++;
    }
}

std::vector<std::string> SyntaxWords(WordClass cls) {
    const Tables& t = T();
    const WordSet* set = nullptr;
    switch (cls) {
    case WordClass::Register: set = &t.registers; break;
    case WordClass::Instruction: set = &t.instructions; break;
    case WordClass::Directive: set = &t.directives; break;
    case WordClass::DataDirective: set = &t.dataDirectives; break;
    case WordClass::Keyword: set = &t.keywords; break;
    case WordClass::Prefix: set = &t.prefixes; break;
    }
    return std::vector<std::string>(set->begin(), set->end());
}
