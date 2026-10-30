// Included at the end of hints.cpp: the rest of the instruction set (system, x87, SSE/AVX/AVX2, BMI…) with examples.
namespace Hints {
namespace {

void AddExtended(std::unordered_map<std::string, Entry>& m) {
    auto set = [&](const char* names, Entry e) {
        std::istringstream in(names);
        std::string n;
        while (in >> n) m[n] = e;
    };
    auto example = [&](const char* name, const char* code) {
        auto it = m.find(name);
        if (it != m.end()) it->second.example = code;
    };

    // ---- examples for the most used instructions
    example("mov", "mov rax, [rbx + 8]  ; загрузить qword из памяти");
    example("lea", "lea rcx, [rel msg]  ; адрес строки для printf");
    example("add", "add rax, rbx");
    example("cmp", "cmp rax, 10 / jl .less");
    example("call", "sub rsp, 40 / lea rcx, [msg] / call printf / add rsp, 40");
    example("div", "xor edx, edx / mov rcx, 10 / div rcx  ; rax = rax / 10, rdx = остаток");
    example("idiv", "cqo / idiv rbx");
    example("imul", "imul rax, rbx, 3");
    example("push", "push rbx  ; сохранить сохраняемый регистр");
    example("loop", "mov ecx, 5 / .l: … / loop .l");
    example("xor", "xor eax, eax  ; rax = 0");
    example("test", "test rax, rax / jz .zero");
    example("shl", "shl rax, 3  ; rax *= 8");
    example("movzx", "movzx eax, byte [rsi]");
    example("cvtsi2sd", "cvtsi2sd xmm0, rax");
    example("addsd", "movsd xmm0, [x] / addsd xmm0, [y]");
    example("rep", "lea rdi, [buf] / mov ecx, 256 / xor eax, eax / rep stosb");
    example("cmovl", "cmp rax, rbx / cmovl rax, rbx  ; rax = max(rax, rbx)");

    // ---- integer: what the base table does not cover
    set("cmpxchg8b cmpxchg16b", {"cmpxchg16b m128",
                                 "Сравнивает rdx:rax с 16 байтами в памяти; если равны — записывает rcx:rbx и ZF = 1, иначе "
                                 "загружает память в rdx:rax и ZF = 0. С lock — атомарно. Адрес выровнен на 16.", "ZF"});
    set("retf", {"retf | retf imm16", "Дальний возврат: снимает со стека адрес и сегмент кода.", kNoFlags});
    set("jcxz", {"jcxz метка", "Переход, если cx = 0 (16-битный режим).", kNoFlags});
    set("int1", {"int1", "Отладочное прерывание #DB (как пошаговое исполнение).", kNoFlags});
    set("into", {"into", "Прерывание #OF, если OF = 1 (нет в 64-битном режиме).", kNoFlags});
    set("iret iretd iretq", {"iretq", "Возврат из обработчика прерывания: снимает rip, cs, rflags, rsp, ss.",
                             "все флаги восстанавливаются"});
    set("sysret", {"sysret", "Возврат из системного вызова в пользовательский режим (только ядро).", "из r11"});
    set("sysenter sysexit", {"sysenter / sysexit", "Быстрый вход в ядро и выход (32-битные системы).", "IF VM сбрасываются"});
    set("cli", {"cli", "IF = 0: запретить внешние прерывания. Привилегированная — в программе вызывает исключение.", "IF"});
    set("sti", {"sti", "IF = 1: разрешить прерывания. Привилегированная.", "IF"});
    set("pushfd popfd", {"pushfd / popfd", "32-битные версии pushf/popf.", "popfd — все"});
    set("pusha popa pushad popad", {"pushad / popad",
                                    "Сохранить / восстановить все регистры общего назначения (только 16/32-битный код).",
                                    kNoFlags});
    set("movs", {"movs приёмник, источник", "Общая форма movsb/w/d/q с явными операндами.", kNoFlags});
    set("lods", {"lods источник", "Общая форма lodsb/w/d/q.", kNoFlags});
    set("stos", {"stos приёмник", "Общая форма stosb/w/d/q.", kNoFlags});
    set("scas", {"scas приёмник", "Общая форма scasb/w/d/q.", kArith});
    set("cmps", {"cmps a, b", "Общая форма cmpsb/w/d/q.", kArith});
    set("cmpsd", {"cmpsd | cmpsd xmm, xmm/m64, предикат",
                  "Без операндов — строковая инструкция: сравнивает dword [rsi] и [rdi] и сдвигает оба. С операндами xmm — "
                  "сравнение двух double: младшие 64 бита = все единицы, если условие истинно (cmpltsd, cmpeqsd…).", kArith});
    set("ins insb insw insd", {"insb", "Чтение из порта dx в [rdi] (привилегированная в Windows).", kNoFlags});
    set("outs outsb outsw outsd", {"outsb", "Запись [rsi] в порт dx (привилегированная в Windows).", kNoFlags});
    set("in", {"in al/ax/eax, imm8 | dx", "Чтение из порта ввода-вывода. В пользовательской программе Windows — исключение.",
               kNoFlags});
    set("out", {"out imm8 | dx, al/ax/eax", "Запись в порт ввода-вывода (привилегированная).", kNoFlags});
    set("xlat xlatb", {"xlatb", "al = [rbx + al]: перекодировка байта по таблице.", kNoFlags});
    set("rdtscp", {"rdtscp", "Счётчик тактов edx:eax и номер процессора в ecx; ждёт завершения предыдущих инструкций.",
                   kNoFlags});
    set("rdrand", {"rdrand r16/32/64", "Случайное число от аппаратного генератора; CF = 1, если получено.", "CF; остальные = 0"});
    set("rdseed", {"rdseed r16/32/64", "Случайное «зерно» с аппаратного источника энтропии; CF = 1, если получено.",
                   "CF; остальные = 0"});
    set("rdpmc", {"rdpmc", "Счётчик производительности номер ecx → edx:eax.", kNoFlags});
    set("rdmsr", {"rdmsr", "Чтение машинного регистра ecx → edx:eax (только ядро).", kNoFlags});
    set("wrmsr", {"wrmsr", "Запись edx:eax в машинный регистр ecx (только ядро).", kNoFlags});
    set("pause", {"pause", "Подсказка процессору внутри цикла ожидания (spin-lock): экономит энергию.", kNoFlags});
    set("lfence", {"lfence", "Барьер: все предыдущие чтения завершаются до следующих.", kNoFlags});
    set("sfence", {"sfence", "Барьер для записей (особенно невременных movnt*).", kNoFlags});
    set("mfence", {"mfence", "Полный барьер памяти для чтений и записей.", kNoFlags});
    set("prefetch prefetchw prefetcht0 prefetcht1 prefetcht2 prefetchnta",
        {"prefetcht0 m8", "Подсказка заранее загрузить строку кэша (t0 — во все уровни, nta — минуя кэш). Ничего не меняет.",
         kNoFlags});
    set("clflush clflushopt", {"clflush m8", "Сбросить строку кэша с этим адресом в память.", kNoFlags});
    set("ud0 ud1", {"ud1 r, r/m", "Недопустимые инструкции (исключение #UD).", kNoFlags});
    set("andn", {"andn r, r1, r/m (BMI1)", "r = (NOT r1) AND r/m.", "SF ZF; OF = CF = 0"});
    set("bextr", {"bextr r, r/m, r2 (BMI1)", "Извлечь биты: начало — младший байт r2, длина — следующий байт.", "ZF; CF = OF = 0"});
    set("blsi", {"blsi r, r/m (BMI1)", "Оставить младший единичный бит: x & -x.", "ZF SF; CF = (источник ≠ 0)"});
    set("blsmsk", {"blsmsk r, r/m (BMI1)", "Маска до младшего единичного бита включительно: x ^ (x - 1).", "SF; CF"});
    set("blsr", {"blsr r, r/m (BMI1)", "Сбросить младший единичный бит: x & (x - 1).", "ZF SF; CF"});
    set("bzhi", {"bzhi r, r/m, r2 (BMI2)", "Обнулить старшие биты, начиная с номера из r2.", "ZF SF; CF"});
    set("pdep", {"pdep r, r1, r/m (BMI2)", "Разложить младшие биты r1 по позициям единиц маски r/m.", kNoFlags});
    set("pext", {"pext r, r1, r/m (BMI2)", "Собрать биты r1 из позиций единиц маски r/m в младшие разряды.", kNoFlags});
    set("rorx", {"rorx r, r/m, imm8 (BMI2)", "Циклический сдвиг вправо без изменения флагов.", kNoFlags});
    set("sarx shlx shrx", {"shlx r, r/m, r2 (BMI2)", "Сдвиг на r2 без изменения флагов, три операнда.", kNoFlags});
    set("mulx", {"mulx r_hi, r_lo, r/m (BMI2)", "Беззнаковое rdx × r/m → r_hi:r_lo без изменения флагов.", kNoFlags});
    set("adcx", {"adcx r, r/m", "Сложение с переносом, меняет только CF (для длинной арифметики в два потока).", "CF"});
    set("adox", {"adox r, r/m", "Сложение с переносом через OF, меняет только OF.", "OF"});
    set("crc32", {"crc32 r32/64, r/m8/16/32/64 (SSE4.2)", "Накопить CRC-32C (полином Кастаньоли) по данным.", kNoFlags});
    set("movbe", {"movbe r, m | m, r", "Копирование с переворотом порядка байтов (big-endian данные).", kNoFlags});
    set("endbr64 endbr32", {"endbr64", "Метка допустимой цели косвенного перехода (защита CET). Работает как nop.", kNoFlags});
    set("xgetbv", {"xgetbv", "Регистр XCR номер ecx → edx:eax (проверка поддержки AVX операционной системой).", kNoFlags});
    set("xsave xrstor", {"xsave m", "Сохранить / восстановить расширенное состояние процессора (x87, SSE, AVX) по маске "
                                    "edx:eax.", kNoFlags});
    set("lgdt lidt", {"lgdt m", "Загрузить таблицу дескрипторов (GDT / IDT). Только ядро.", kNoFlags});
    set("sgdt sidt", {"sgdt m", "Сохранить адрес и размер GDT / IDT в память.", kNoFlags});
    set("invlpg", {"invlpg m", "Сбросить запись TLB для страницы (только ядро).", kNoFlags});
    set("wbinvd", {"wbinvd", "Записать и сбросить все кэши (только ядро).", kNoFlags});
    set("emms", {"emms", "Освободить регистры MMX для x87 (после инструкций mm0–mm7).", "теги FPU"});
    set("ldmxcsr", {"ldmxcsr m32", "Загрузить MXCSR: режим округления и маски исключений SSE.", kNoFlags});
    set("stmxcsr", {"stmxcsr m32", "Сохранить MXCSR.", kNoFlags});
    set("movnti", {"movnti m, r", "Запись в память мимо кэша (невременная).", kNoFlags});
    set("movntq maskmovq", {"movntq m64, mm", "Невременная запись регистра MMX.", kNoFlags});

    // ---- x87: the rest
    const char* fpu = "C0–C3 FPU";
    set("fist", {"fist m16/m32", "Сохранить st0 как целое (с округлением), не снимая со стека.", fpu});
    set("fisttp", {"fisttp m16/m32/m64", "Сохранить st0 как целое с отбрасыванием дробной части и снять со стека (SSE3).", fpu});
    set("fsubr fsubrp fdivr fdivrp", {"fsubr st0, st(i)", "Обратное вычитание / деление: st0 = st(i) − st0 (÷).", fpu});
    set("fiadd fisub fisubr fimul fidiv fidivr", {"fiadd m16/m32", "Арифметика st0 с целым из памяти.", fpu});
    set("fchs", {"fchs", "st0 = −st0.", fpu});
    set("fabs", {"fabs", "st0 = |st0|.", fpu});
    set("fsqrt", {"fsqrt", "st0 = √st0.", fpu});
    set("fsin fcos", {"fsin", "st0 = sin(st0) (радианы).", fpu});
    set("fsincos", {"fsincos", "st0 = cos(x), st1 = sin(x).", fpu});
    set("fptan", {"fptan", "st0 = tg(st0), затем кладёт 1.0.", fpu});
    set("fpatan", {"fpatan", "st1 = arctg(st1 / st0), снимает st0.", fpu});
    set("fprem fprem1", {"fprem", "Остаток от деления st0 на st1.", fpu});
    set("frndint", {"frndint", "Округлить st0 до целого по текущему режиму.", fpu});
    set("fscale", {"fscale", "st0 = st0 · 2^trunc(st1).", fpu});
    set("fxtract", {"fxtract", "Разделить st0 на порядок и мантиссу.", fpu});
    set("fcom fcomp fcompp fucom fucomp fucompp ficom ficomp ftst",
        {"fcomp st(i) | m32/m64", "Сравнить st0 с операндом; результат в C0, C2, C3 слова состояния (fstsw ax / sahf).", fpu});
    set("fcomi fcomip fucomi fucomip", {"fcomip st0, st(i)",
                                        "Сравнить и сразу выставить ZF, PF, CF (как беззнаковое сравнение: ja/jb).",
                                        "ZF PF CF"});
    set("fxam", {"fxam", "Классифицировать st0 (ноль, бесконечность, NaN…) в C0–C3.", fpu});
    set("fld1 fldz fldpi fldl2e fldl2t fldlg2 fldln2", {"fldpi", "Загрузить константу: 1, 0, π, log2(e), log2(10), lg 2, ln 2.", fpu});
    set("fclex fnclex", {"fclex", "Сбросить флаги исключений FPU.", fpu});
    set("fstsw fnstsw", {"fstsw ax", "Слово состояния FPU → ax (после fcom: sahf и обычные переходы).", kNoFlags});
    set("fstcw fnstcw", {"fstcw m16", "Сохранить управляющее слово FPU (округление, точность).", kNoFlags});
    set("fldcw", {"fldcw m16", "Загрузить управляющее слово FPU.", kNoFlags});
    set("fwait wait", {"fwait", "Дождаться завершения инструкции FPU и обработать исключения.", kNoFlags});
    set("ffree", {"ffree st(i)", "Пометить регистр стека FPU пустым.", kNoFlags});
    set("fincstp fdecstp", {"fincstp", "Сдвинуть указатель вершины стека FPU.", fpu});
    set("fxch", {"fxch st(i)", "Обменять st0 и st(i).", fpu});
    set("fxsave fxrstor", {"fxsave m512", "Сохранить / восстановить состояние x87 и SSE (512 байт, адрес кратен 16).", kNoFlags});
    set("f2xm1", {"f2xm1", "st0 = 2^st0 − 1 (для −1 ≤ st0 ≤ 1).", fpu});
    set("fyl2x fyl2xp1", {"fyl2x", "st1 = st1 · log2(st0), снимает st0.", fpu});
    set("fnop", {"fnop", "Ничего не делает (FPU).", kNoFlags});
    set("fsave fnsave frstor fstenv fnstenv fldenv", {"fsave m", "Сохранить / восстановить состояние или окружение FPU.",
                                                      "сбрасывает FPU (fsave)"});

    // ---- SSE floating point: the rest (each also has an AVX form, see Lookup)
    set("movhps movlps movhpd movlpd", {"movhps xmm, m64 | m64, xmm", "Копировать старшие (h) или младшие (l) 64 бита xmm.", kNoFlags});
    set("movhlps movlhps", {"movhlps xmm1, xmm2", "Копировать старшую половину xmm2 в младшую xmm1 (lh — наоборот).", kNoFlags});
    set("movmskps movmskpd", {"movmskps r32, xmm", "Знаковые биты элементов → младшие биты регистра (после сравнений cmpps).",
                              kNoFlags});
    set("movntps movntpd movntdq movntdqa", {"movntps m128, xmm", "Запись / чтение 16 байт мимо кэша.", kNoFlags});
    set("rcpps rcpss", {"rcpps xmm, xmm/m128", "Приближённое 1/x (точность ~12 бит).", kNoFlags});
    set("rsqrtps rsqrtss", {"rsqrtps xmm, xmm/m128", "Приближённое 1/√x (точность ~12 бит).", kNoFlags});
    set("andnps andnpd pandn", {"andnps xmm, xmm/m128", "Побитовое (NOT приёмник) AND источник.", kNoFlags});
    set("orps orpd por", {"orps xmm, xmm/m128", "Побитовое OR 128 бит.", kNoFlags});
    set("cmpps cmppd cmpss", {"cmpps xmm, xmm/m128, предикат",
                              "Сравнение элементов: результат — маска из единиц (истина) или нулей. Предикат: 0 = eq, 1 = lt, "
                              "2 = le, 4 = neq… (NASM: cmpltps).", kNoFlags});
    set("shufps shufpd", {"shufps xmm1, xmm2/m128, imm8", "Перестановка: два младших элемента из xmm1, два старших из xmm2 — по "
                                                          "битам imm8.", kNoFlags});
    set("unpcklps unpckhps unpcklpd unpckhpd", {"unpcklps xmm1, xmm2/m128",
                                                "Чередовать младшие (l) или старшие (h) элементы двух регистров.", kNoFlags});
    set("cvtss2si", {"cvtss2si r32/64, xmm/m32", "float → целое с округлением по текущему режиму.", kNoFlags});
    set("cvtps2pd", {"cvtps2pd xmm, xmm/m64", "Два младших float → два double.", kNoFlags});
    set("cvtpd2ps", {"cvtpd2ps xmm, xmm/m128", "Два double → два float.", kNoFlags});
    set("cvtdq2ps", {"cvtdq2ps xmm, xmm/m128", "Четыре int32 → четыре float.", kNoFlags});
    set("cvtps2dq cvttps2dq", {"cvtps2dq xmm, xmm/m128", "Четыре float → int32 (tt — с отбрасыванием дробной части).", kNoFlags});
    set("cvtdq2pd", {"cvtdq2pd xmm, xmm/m64", "Два int32 → два double.", kNoFlags});
    set("cvtpd2dq cvttpd2dq", {"cvtpd2dq xmm, xmm/m128", "Два double → два int32 (tt — с отбрасыванием).", kNoFlags});
    set("blendps blendpd", {"blendps xmm1, xmm2/m128, imm8", "Взять элементы из xmm2 там, где бит imm8 = 1 (SSE4.1).", kNoFlags});
    set("blendvps blendvpd pblendvb", {"blendvps xmm1, xmm2/m128, xmm0", "Смешать по знаковым битам маски xmm0 (SSE4.1).", kNoFlags});
    set("pblendw", {"pblendw xmm1, xmm2/m128, imm8", "Смешать 16-битные элементы по битам imm8.", kNoFlags});
    set("dpps dppd", {"dpps xmm1, xmm2/m128, imm8", "Скалярное произведение векторов (какие элементы — по imm8).", kNoFlags});
    set("roundps roundpd roundss roundsd", {"roundsd xmm1, xmm2/m64, imm8",
                                            "Округлить: imm8 = 0 к ближайшему, 1 вниз, 2 вверх, 3 к нулю (SSE4.1).", kNoFlags});
    set("haddps haddpd hsubps hsubpd", {"haddps xmm1, xmm2/m128", "Горизонтальное сложение соседних элементов (SSE3).", kNoFlags});
    set("addsubps addsubpd", {"addsubps xmm1, xmm2/m128", "Чётные элементы вычитаются, нечётные складываются (SSE3).", kNoFlags});
    set("movddup movshdup movsldup", {"movddup xmm1, xmm2/m64", "Размножить элементы (младший double / чётные или нечётные float).",
                                       kNoFlags});
    set("lddqu", {"lddqu xmm, m128", "Невыровненное чтение 16 байт (SSE3).", kNoFlags});
    set("insertps", {"insertps xmm1, xmm2/m32, imm8", "Вставить один float в позицию по imm8, можно обнулить другие.", kNoFlags});
    set("extractps", {"extractps r/m32, xmm, imm8", "Извлечь один float (как 32 бита).", kNoFlags});

    // ---- SSE integer (packed): generated families
    struct Size {
        const char* suffix;
        const char* what;
    };
    const Size sizes[] = {{"b", "16 байт"}, {"w", "8 слов (16 бит)"}, {"d", "4 двойных слова (32 бита)"},
                          {"q", "2 учетверённых слова (64 бита)"}};
    for (const Size& s : sizes) {
        std::string sf = s.suffix;
        m["padd" + sf] = {"padd" + sf + " xmm, xmm/m128", std::string("Сложение упакованных целых: ") + s.what +
                                                             " сразу, с переносом по кругу.", kNoFlags};
        m["psub" + sf] = {"psub" + sf + " xmm, xmm/m128", std::string("Вычитание упакованных целых: ") + s.what + ".", kNoFlags};
        m["pcmpeq" + sf] = {"pcmpeq" + sf + " xmm, xmm/m128",
                            std::string("Сравнение на равенство (") + s.what + "): элемент = все единицы, если равны.", kNoFlags};
        m["pcmpgt" + sf] = {"pcmpgt" + sf + " xmm, xmm/m128",
                            std::string("Сравнение «больше» со знаком (") + s.what + "): маска из единиц.", kNoFlags};
    }
    for (const char* sf : {"sb", "sw"}) {
        m[std::string("padd") + sf] = {std::string("padd") + sf + " xmm, xmm/m128", "Сложение со знаком с насыщением "
                                       "(результат ограничивается, а не переполняется).", kNoFlags};
        m[std::string("psub") + sf] = {std::string("psub") + sf + " xmm, xmm/m128", "Вычитание со знаком с насыщением.", kNoFlags};
    }
    for (const char* sf : {"usb", "usw"}) {
        m[std::string("padd") + sf] = {std::string("padd") + sf + " xmm, xmm/m128", "Сложение без знака с насыщением.", kNoFlags};
        m[std::string("psub") + sf] = {std::string("psub") + sf + " xmm, xmm/m128", "Вычитание без знака с насыщением (не ниже 0).",
                                       kNoFlags};
    }
    set("pmullw pmulld", {"pmullw xmm, xmm/m128", "Умножение упакованных целых, младшие половины результатов.", kNoFlags});
    set("pmulhw pmulhuw", {"pmulhw xmm, xmm/m128", "Умножение 16-битных элементов, старшие 16 бит результатов (uw — без знака).",
                           kNoFlags});
    set("pmuludq pmuldq", {"pmuludq xmm, xmm/m128", "Умножение чётных 32-битных элементов → 64-битные произведения.", kNoFlags});
    set("pmaddwd", {"pmaddwd xmm, xmm/m128", "Перемножить 16-битные пары и сложить соседние произведения в 32 бита.", kNoFlags});
    for (const char* op : {"psll", "psrl", "psra"}) {
        std::string o = op;
        std::string what = o == "psll" ? "влево" : o == "psrl" ? "вправо (логический)" : "вправо с сохранением знака";
        for (const char* sf : {"w", "d", "q"}) {
            if (o == "psra" && std::string(sf) == "q") continue;
            m[o + sf] = {o + sf + " xmm, xmm/imm8", "Сдвиг каждого элемента " + what + ".", kNoFlags};
        }
    }
    set("pslldq psrldq", {"pslldq xmm, imm8", "Сдвиг всего регистра на imm8 байт (влево / вправо).", kNoFlags});
    set("pshufb", {"pshufb xmm1, xmm2/m128", "Переставить байты xmm1 по индексам из xmm2 (бит 7 = обнулить) — SSSE3.", kNoFlags});
    set("pshufd", {"pshufd xmm1, xmm2/m128, imm8", "Переставить 4 двойных слова по 2-битным полям imm8.", kNoFlags});
    set("pshufhw pshuflw", {"pshufhw xmm1, xmm2/m128, imm8", "Переставить старшие (h) или младшие (l) 4 слова.", kNoFlags});
    for (const char* sf : {"bw", "wd", "dq", "qdq"}) {
        m[std::string("punpckl") + sf] = {std::string("punpckl") + sf + " xmm, xmm/m128", "Чередовать младшие элементы двух регистров.",
                                          kNoFlags};
        m[std::string("punpckh") + sf] = {std::string("punpckh") + sf + " xmm, xmm/m128", "Чередовать старшие элементы двух регистров.",
                                          kNoFlags};
    }
    set("packsswb packssdw", {"packsswb xmm, xmm/m128", "Упаковать элементы вдвое меньшего размера со знаковым насыщением.", kNoFlags});
    set("packuswb packusdw", {"packuswb xmm, xmm/m128", "Упаковать с беззнаковым насыщением.", kNoFlags});
    set("pmovmskb", {"pmovmskb r32, xmm", "Старшие биты 16 байт → 16 бит регистра (поиск байта: pcmpeqb + pmovmskb + bsf).", kNoFlags});
    set("pextrb pextrw pextrd pextrq", {"pextrd r/m32, xmm, imm8", "Извлечь элемент номер imm8 в регистр или память.", kNoFlags});
    set("pinsrb pinsrw pinsrd pinsrq", {"pinsrd xmm, r/m32, imm8", "Вставить элемент в позицию imm8.", kNoFlags});
    set("pminsb pminsw pminsd pminub pminuw pminud", {"pminsd xmm, xmm/m128", "Поэлементный минимум (s — со знаком, u — без).", kNoFlags});
    set("pmaxsb pmaxsw pmaxsd pmaxub pmaxuw pmaxud", {"pmaxsd xmm, xmm/m128", "Поэлементный максимум (s — со знаком, u — без).", kNoFlags});
    set("pabsb pabsw pabsd", {"pabsd xmm, xmm/m128", "Модуль каждого элемента (SSSE3).", kNoFlags});
    set("palignr", {"palignr xmm1, xmm2/m128, imm8", "Склеить xmm1:xmm2 и сдвинуть вправо на imm8 байт.", kNoFlags});
    set("ptest", {"ptest xmm1, xmm2/m128", "ZF = 1, если (xmm1 AND xmm2) = 0; CF = 1, если (NOT xmm1 AND xmm2) = 0.", "ZF CF"});
    set("pavgb pavgw", {"pavgb xmm, xmm/m128", "Среднее с округлением вверх, без знака.", kNoFlags});
    set("psadbw", {"psadbw xmm, xmm/m128", "Сумма модулей разностей байтов (для сравнения блоков).", kNoFlags});
    for (const char* sx : {"s", "z"}) {
        for (const char* sf : {"bw", "bd", "bq", "wd", "wq", "dq"}) {
            std::string n = std::string("pmov") + sx + "x" + sf;
            m[n] = {n + " xmm, xmm/m", std::string("Расширить младшие элементы до большего размера ") +
                                           (*sx == 's' ? "со знаком" : "нулями") + " (SSE4.1).", kNoFlags};
        }
    }
    set("pcmpestri pcmpestrm pcmpistri pcmpistrm", {"pcmpistri xmm1, xmm2/m128, imm8",
                                                    "Сравнение строк (SSE4.2): поиск символов, подстрок, диапазонов; режим в imm8.",
                                                    "CF ZF SF OF"});
    set("aesenc aesenclast aesdec aesdeclast", {"aesenc xmm1, xmm2/m128", "Раунд шифрования / расшифрования AES.", kNoFlags});
    set("aesimc aeskeygenassist", {"aeskeygenassist xmm1, xmm2/m128, imm8", "Подготовка ключей AES.", kNoFlags});
    set("pclmulqdq", {"pclmulqdq xmm1, xmm2/m128, imm8", "Умножение без переносов 64×64 → 128 бит (CRC, GCM).", kNoFlags});

    // ---- AVX / AVX2 only
    set("vzeroupper", {"vzeroupper", "Обнулить старшие половины всех ymm. Ставьте перед вызовом обычного SSE-кода и функций C.",
                       kNoFlags});
    set("vzeroall", {"vzeroall", "Обнулить все регистры ymm.", kNoFlags});
    set("vbroadcastss vbroadcastsd vbroadcastf128 vbroadcasti128 vpbroadcastb vpbroadcastw vpbroadcastd vpbroadcastq",
        {"vbroadcastss ymm, m32 | xmm", "Размножить одно значение во все элементы регистра.", kNoFlags});
    set("vperm2f128 vperm2i128", {"vperm2f128 ymm1, ymm2, ymm3/m256, imm8", "Выбрать 128-битные половины из двух регистров.", kNoFlags});
    set("vpermd vpermps", {"vpermd ymm1, ymm2, ymm3/m256", "Переставить 8 элементов по 32 бита по индексам из ymm2 (AVX2).", kNoFlags});
    set("vpermq vpermpd", {"vpermq ymm1, ymm2/m256, imm8", "Переставить 4 элемента по 64 бита по imm8 (AVX2).", kNoFlags});
    set("vpermilps vpermilpd", {"vpermilps ymm1, ymm2, ymm3/imm8", "Перестановка внутри каждой 128-битной половины.", kNoFlags});
    set("vinsertf128 vinserti128", {"vinsertf128 ymm1, ymm2, xmm3/m128, imm8", "Вставить 128 бит в половину ymm (imm8 = 0/1).",
                                    kNoFlags});
    set("vextractf128 vextracti128", {"vextractf128 xmm/m128, ymm, imm8", "Извлечь половину ymm.", kNoFlags});
    set("vmaskmovps vmaskmovpd vpmaskmovd vpmaskmovq", {"vmaskmovps ymm1, ymm2, m256",
                                                        "Чтение / запись только элементов, где знак маски = 1.", kNoFlags});
    set("vgatherdps vgatherdpd vgatherqps vgatherqpd vpgatherdd vpgatherdq vpgatherqd vpgatherqq",
        {"vgatherdps ymm1, [base + ymm2*scale], ymm3", "Собрать элементы из памяти по вектору индексов (AVX2), маска в ymm3.",
         kNoFlags});
    set("vpsllvd vpsllvq vpsrlvd vpsrlvq vpsravd", {"vpsllvd ymm1, ymm2, ymm3/m256",
                                                    "Сдвиг каждого элемента на своё количество бит (AVX2).", kNoFlags});
    set("vtestps vtestpd", {"vtestps ymm1, ymm2/m256", "Как ptest, но только по знаковым битам.", "ZF CF"});
    set("vcvtph2ps vcvtps2ph", {"vcvtph2ps ymm, xmm/m128", "Половинная точность (float16) ↔ float.", kNoFlags});
    for (const char* op : {"fmadd", "fmsub", "fnmadd", "fnmsub", "fmaddsub", "fmsubadd"}) {
        for (const char* order : {"132", "213", "231"}) {
            for (const char* t : {"ps", "pd", "ss", "sd"}) {
                std::string n = std::string("v") + op + order + t;
                std::string o = op;
                std::string what = o == "fmadd" ? "a·b + c" : o == "fmsub" ? "a·b − c" : o == "fnmadd" ? "−(a·b) + c"
                                 : o == "fnmsub" ? "−(a·b) − c" : o == "fmaddsub" ? "a·b ± c попеременно" : "a·b ∓ c попеременно";
                m[n] = {n + " xmm1, xmm2, xmm3/m",
                        "Слитное умножение-сложение (FMA3): " + what + " с одним округлением. Цифры " + order +
                            " — какие операнды перемножаются (1 — приёмник): 213 = xmm1·xmm2 + xmm3.",
                        kNoFlags};
            }
        }
    }
}

}
}
