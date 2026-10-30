#include "hints.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace Hints {
namespace {

struct Entry {
    std::string syntax;
    std::string text;
    std::string flags;  // empty: no "Флаги" line (directives, registers)
};

constexpr const char* kNoFlags = "не меняет";
constexpr const char* kArith = "OF SF ZF AF PF CF";
constexpr const char* kLogic = "SF ZF PF по результату; OF = CF = 0; AF не определён";

// Condition codes shared by jcc / setcc / cmovcc.
struct Condition {
    const char* suffixes;  // synonyms
    const char* meaning;
    const char* test;
};
const Condition kConditions[] = {
    {"e z", "равно / ноль", "ZF = 1"},
    {"ne nz", "не равно / не ноль", "ZF = 0"},
    {"g nle", "больше (числа со знаком)", "ZF = 0 и SF = OF"},
    {"ge nl", "больше или равно (со знаком)", "SF = OF"},
    {"l nge", "меньше (со знаком)", "SF ≠ OF"},
    {"le ng", "меньше или равно (со знаком)", "ZF = 1 или SF ≠ OF"},
    {"a nbe", "выше (без знака, >)", "CF = 0 и ZF = 0"},
    {"ae nb nc", "выше или равно (без знака, ≥) / нет переноса", "CF = 0"},
    {"b nae c", "ниже (без знака, <) / перенос", "CF = 1"},
    {"be na", "ниже или равно (без знака, ≤)", "CF = 1 или ZF = 1"},
    {"s", "результат отрицательный", "SF = 1"},
    {"ns", "результат неотрицательный", "SF = 0"},
    {"o", "переполнение (со знаком)", "OF = 1"},
    {"no", "нет переполнения", "OF = 0"},
    {"p pe", "чётное число единиц в младшем байте", "PF = 1"},
    {"np po", "нечётное число единиц в младшем байте", "PF = 0"},
};

// "64 бит" / "32 бит" / ... for a general-purpose register name.
std::string RegisterSize(const std::string& n) {
    static const char* legacy16[] = {"ax", "bx", "cx", "dx", "si", "di", "bp", "sp"};
    bool numbered = n.size() > 1 && n[0] == 'r' && isdigit(static_cast<unsigned char>(n[1]));  // r8..r15
    if (numbered) {
        char last = n.back();
        return last == 'd' ? "32 бит" : last == 'w' ? "16 бит" : (last == 'b' || last == 'l') ? "8 бит" : "64 бит";
    }
    if (n[0] == 'r') return "64 бит";  // rax, rip...
    if (n[0] == 'e') return "32 бит";
    for (const char* r : legacy16) {
        if (n == r) return "16 бит";
    }
    return "8 бит";  // al, ah, sil...
}

void Add(std::unordered_map<std::string, Entry>& m, const char* names, Entry e) {
    std::istringstream in(names);
    std::string n;
    while (in >> n) m[n] = e;
}

std::unordered_map<std::string, Entry> Build() {
    std::unordered_map<std::string, Entry> m;

    // ---- data movement
    Add(m, "mov", {"mov приёмник, источник",
                   "Копирует источник в приёмник. Операнды: регистр, память, число; память → память нельзя. "
                   "Запись в 32-битный регистр (eax) обнуляет старшие 32 бита rax; запись в ax/al — не трогает.", kNoFlags});
    Add(m, "movzx", {"movzx r16/32/64, r/m8 | movzx r32/64, r/m16",
                     "Копирует меньший операнд в больший, заполняя старшие биты нулями (беззнаковое расширение). "
                     "Для 32 → 64 бит movzx нет: mov eax, ... и так обнуляет старшие биты.", kNoFlags});
    Add(m, "movsx", {"movsx r16/32/64, r/m8 | movsx r32/64, r/m16",
                     "Копирует меньший операнд в больший с расширением знака (для чисел со знаком).", kNoFlags});
    Add(m, "movsxd", {"movsxd r64, r/m32", "Расширяет 32-битное число со знаком до 64 бит.", kNoFlags});
    Add(m, "lea", {"lea регистр, [адрес]",
                   "Вычисляет адрес операнда и кладёт его в регистр — память не читается. Удобно и для арифметики: "
                   "lea rax, [rbx + rcx*4 + 8]. С default rel: lea rcx, [msg] даёт адрес msg.", kNoFlags});
    Add(m, "xchg", {"xchg a, b", "Меняет операнды местами. С операндом в памяти выполняется атомарно (неявный lock).",
                    kNoFlags});
    Add(m, "push", {"push r64 | push [m64] | push imm32",
                    "Уменьшает rsp на 8 и записывает значение в [rsp]. В 64-битном режиме кладутся 8 байт.", kNoFlags});
    Add(m, "pop", {"pop r64 | pop [m64]", "Читает 8 байт из [rsp] в операнд и увеличивает rsp на 8.", kNoFlags});
    Add(m, "bswap", {"bswap r32/r64", "Переворачивает порядок байтов (little-endian ↔ big-endian).", kNoFlags});
    Add(m, "cbw", {"cbw", "Расширяет знак: al → ax.", kNoFlags});
    Add(m, "cwde", {"cwde", "Расширяет знак: ax → eax.", kNoFlags});
    Add(m, "cdqe", {"cdqe", "Расширяет знак: eax → rax.", kNoFlags});
    Add(m, "cwd", {"cwd", "Расширяет знак ax в dx (dx:ax) — перед idiv для 16 бит.", kNoFlags});
    Add(m, "cdq", {"cdq", "Расширяет знак eax в edx (edx:eax) — перед idiv для 32 бит.", kNoFlags});
    Add(m, "cqo", {"cqo", "Расширяет знак rax в rdx (rdx:rax) — перед idiv для 64 бит.", kNoFlags});

    // ---- arithmetic
    Add(m, "add", {"add приёмник, источник", "приёмник = приёмник + источник.", kArith});
    Add(m, "adc", {"adc приёмник, источник",
                   "приёмник = приёмник + источник + CF. Для сложения чисел длиннее регистра.", kArith});
    Add(m, "sub", {"sub приёмник, источник", "приёмник = приёмник − источник.", kArith});
    Add(m, "sbb", {"sbb приёмник, источник", "приёмник = приёмник − (источник + CF). Для длинного вычитания.", kArith});
    Add(m, "inc", {"inc операнд", "Увеличивает на 1.", "OF SF ZF AF PF (CF не меняет)"});
    Add(m, "dec", {"dec операнд", "Уменьшает на 1.", "OF SF ZF AF PF (CF не меняет)"});
    Add(m, "neg", {"neg операнд", "Меняет знак: операнд = −операнд. CF = 1, если операнд был не 0.", kArith});
    Add(m, "cmp", {"cmp a, b",
                   "Вычисляет a − b только ради флагов (результат не сохраняется). Дальше — jcc, setcc или cmovcc: "
                   "jl/jg для чисел со знаком, jb/ja — без знака.", kArith});
    Add(m, "mul", {"mul r/m",
                   "Беззнаковое умножение на rax: rdx:rax = rax × операнд. Для 32 бит: edx:eax = eax × r/m32; "
                   "16: dx:ax; 8: ax = al × r/m8.", "OF = CF = 1, если старшая половина результата не 0; "
                                                    "SF ZF AF PF не определены"});
    Add(m, "imul", {"imul r/m | imul r, r/m | imul r, r/m, imm",
                    "Умножение со знаком. Одна форма — как mul (rdx:rax = rax × r/m); две — r = r × r/m; "
                    "три — r = r/m × imm. В формах с 2–3 операндами старшая половина теряется.",
                    "OF = CF = 1 при переполнении; остальные не определены"});
    Add(m, "div", {"div r/m",
                   "Беззнаковое деление rdx:rax на операнд: rax = частное, rdx = остаток (32 бит: edx:eax → eax, edx). "
                   "Перед делением обнулите rdx (xor edx, edx). Деление на 0 или слишком большое частное — исключение #DE.",
                   "не определены"});
    Add(m, "idiv", {"idiv r/m",
                    "Деление со знаком rdx:rax на операнд: rax = частное, rdx = остаток (знак остатка — как у делимого). "
                    "Перед делением расширьте знак: cqo (64 бит) или cdq (32 бит).", "не определены"});

    // ---- logic, shifts, bits
    Add(m, "and", {"and приёмник, источник", "Побитовое И. Часто — для сброса битов по маске.", kLogic});
    Add(m, "or", {"or приёмник, источник", "Побитовое ИЛИ. Часто — для установки битов.", kLogic});
    Add(m, "xor", {"xor приёмник, источник",
                   "Побитовое исключающее ИЛИ. xor eax, eax — самый быстрый способ обнулить регистр (и весь rax).", kLogic});
    Add(m, "not", {"not операнд", "Инвертирует все биты.", kNoFlags});
    Add(m, "test", {"test a, b",
                    "Вычисляет a AND b только ради флагов. test rax, rax — проверка на ноль (jz) и знак (js).", kLogic});
    Add(m, "shl sal", {"shl операнд, 1 | imm8 | cl",
                       "Сдвиг влево (умножение на 2^n). Выдвинутый бит попадает в CF. Счётчик берётся по модулю 64 (32).",
                       "CF — последний выдвинутый бит; OF определён для сдвига на 1; SF ZF PF по результату"});
    Add(m, "shr", {"shr операнд, 1 | imm8 | cl",
                   "Логический сдвиг вправо, слева приходят нули (деление без знака на 2^n).",
                   "CF — последний выдвинутый бит; SF ZF PF по результату"});
    Add(m, "sar", {"sar операнд, 1 | imm8 | cl",
                   "Арифметический сдвиг вправо: копирует знаковый бит (деление со знаком на 2^n с округлением вниз).",
                   "CF — последний выдвинутый бит; SF ZF PF по результату"});
    Add(m, "rol ror", {"rol/ror операнд, 1 | imm8 | cl", "Циклический сдвиг: выдвинутые биты возвращаются с другой стороны.",
                       "CF — последний перенесённый бит; OF для сдвига на 1"});
    Add(m, "rcl rcr", {"rcl/rcr операнд, 1 | imm8 | cl", "Циклический сдвиг через флаг CF.",
                       "CF; OF для сдвига на 1"});
    Add(m, "shld shrd", {"shld/shrd приёмник, источник, imm8 | cl",
                         "Сдвиг с заполнением битами из второго операнда (сдвиг 128-битной пары).", "CF SF ZF PF; OF для 1"});
    Add(m, "bt", {"bt операнд, номер_бита", "Копирует указанный бит в CF.", "CF; остальные не определены"});
    Add(m, "bts", {"bts операнд, номер_бита", "Копирует бит в CF и устанавливает его в 1.", "CF"});
    Add(m, "btr", {"btr операнд, номер_бита", "Копирует бит в CF и сбрасывает его в 0.", "CF"});
    Add(m, "btc", {"btc операнд, номер_бита", "Копирует бит в CF и инвертирует его.", "CF"});
    Add(m, "bsf", {"bsf приёмник, источник", "Номер младшего единичного бита источника. Если источник 0 — ZF = 1, "
                                             "приёмник не определён.", "ZF"});
    Add(m, "bsr", {"bsr приёмник, источник", "Номер старшего единичного бита источника. Если источник 0 — ZF = 1.", "ZF"});
    Add(m, "popcnt", {"popcnt приёмник, источник", "Количество единичных битов.", "ZF по результату; остальные = 0"});
    Add(m, "lzcnt", {"lzcnt приёмник, источник", "Количество нулей в старших разрядах (для 0 — размер операнда).",
                     "CF = 1, если источник 0; ZF по результату"});
    Add(m, "tzcnt", {"tzcnt приёмник, источник", "Количество нулей в младших разрядах (для 0 — размер операнда).",
                     "CF = 1, если источник 0; ZF по результату"});

    // ---- control flow
    Add(m, "jmp", {"jmp метка | jmp r/m64", "Безусловный переход.", kNoFlags});
    Add(m, "call", {"call метка | call r/m64",
                    "Кладёт адрес возврата в стек (rsp −= 8) и переходит. В Win64 перед call: rsp кратен 16 и "
                    "выделено 32 байта «теневого» места (sub rsp, 40 в начале функции). Аргументы: rcx, rdx, r8, r9.",
                    kNoFlags});
    Add(m, "ret retn", {"ret | ret imm16",
                        "Снимает адрес возврата со стека и переходит по нему; ret imm16 дополнительно освобождает imm16 байт.",
                        kNoFlags});
    Add(m, "loop", {"loop метка", "rcx = rcx − 1; если rcx ≠ 0 — переход. Медленнее, чем dec + jnz.", kNoFlags});
    Add(m, "loope loopz", {"loope метка", "rcx −= 1; переход, если rcx ≠ 0 и ZF = 1.", kNoFlags});
    Add(m, "loopne loopnz", {"loopne метка", "rcx −= 1; переход, если rcx ≠ 0 и ZF = 0.", kNoFlags});
    Add(m, "jrcxz", {"jrcxz метка", "Переход, если rcx = 0.", kNoFlags});
    Add(m, "jecxz", {"jecxz метка", "Переход, если ecx = 0.", kNoFlags});
    Add(m, "leave", {"leave", "Выход из кадра стека: mov rsp, rbp; pop rbp.", kNoFlags});
    Add(m, "enter", {"enter размер, 0", "Создаёт кадр стека: push rbp; mov rbp, rsp; sub rsp, размер.", kNoFlags});
    Add(m, "syscall", {"syscall", "Системный вызов ядра (номер в rax). В Windows не документирован — используйте WinAPI.",
                       "сохраняются в r11"});
    Add(m, "int", {"int imm8", "Программное прерывание. В 64-битном Windows почти не используется.", "IF TF сбрасываются"});
    Add(m, "int3", {"int3", "Точка останова: под отладчиком программа остановится на этой строке.", kNoFlags});
    Add(m, "nop", {"nop", "Ничего не делает (1 байт; с операндом — длинный nop для выравнивания).", kNoFlags});
    Add(m, "hlt", {"hlt", "Останов процессора — в обычной программе вызывает исключение.", kNoFlags});
    Add(m, "ud2", {"ud2", "Гарантированно недопустимая инструкция (исключение #UD).", kNoFlags});

    // ---- flags, strings, misc
    Add(m, "clc", {"clc", "CF = 0.", "CF"});
    Add(m, "stc", {"stc", "CF = 1.", "CF"});
    Add(m, "cmc", {"cmc", "CF = NOT CF.", "CF"});
    Add(m, "cld", {"cld", "DF = 0: строковые инструкции идут вперёд (rsi/rdi растут). Win64 ожидает DF = 0.", "DF"});
    Add(m, "std", {"std", "DF = 1: строковые инструкции идут назад. Верните cld до вызова функций.", "DF"});
    Add(m, "lahf", {"lahf", "ah = SF ZF AF PF CF (младший байт флагов).", kNoFlags});
    Add(m, "sahf", {"sahf", "SF ZF AF PF CF = ah.", "SF ZF AF PF CF"});
    Add(m, "pushf pushfq", {"pushfq", "Кладёт регистр флагов в стек.", kNoFlags});
    Add(m, "popf popfq", {"popfq", "Снимает регистр флагов со стека.", "все (кроме системных)"});
    Add(m, "movsb movsw movsq", {"movsb/w/d/q",
                                 "Копирует элемент [rsi] → [rdi] и сдвигает rsi, rdi. С префиксом rep — повторяет rcx раз "
                                 "(копирование блока).", kNoFlags});
    Add(m, "stosb stosw stosd stosq", {"stosb/w/d/q",
                                       "[rdi] = al/ax/eax/rax и сдвигает rdi. rep stosb — заполнение rcx байт.", kNoFlags});
    Add(m, "lodsb lodsw lodsd lodsq", {"lodsb/w/d/q", "al/ax/eax/rax = [rsi] и сдвигает rsi.", kNoFlags});
    Add(m, "scasb scasw scasd scasq", {"scasb/w/d/q",
                                       "Сравнивает al/ax/eax/rax с [rdi] и сдвигает rdi. repne scasb — поиск байта "
                                       "(например, нуля для длины строки).", kArith});
    Add(m, "cmpsb cmpsw cmpsq", {"cmpsb/w/d/q",
                                 "Сравнивает [rsi] с [rdi] и сдвигает оба. repe cmpsb — сравнение блоков.", kArith});
    Add(m, "rep", {"rep строковая_инструкция", "Повторяет movs/stos/lods rcx раз (rcx уменьшается до 0).", kNoFlags});
    Add(m, "repe repz", {"repe cmps/scas", "Повторяет, пока rcx ≠ 0 и элементы равны (ZF = 1).", kNoFlags});
    Add(m, "repne repnz", {"repne cmps/scas", "Повторяет, пока rcx ≠ 0 и элементы не равны (ZF = 0).", kNoFlags});
    Add(m, "lock", {"lock инструкция", "Делает инструкцию с памятью атомарной для других потоков (add, inc, xadd, cmpxchg…).",
                    kNoFlags});
    Add(m, "xadd", {"xadd приёмник, источник", "Обмен и сложение: временно = приёмник + источник; источник = старый "
                                               "приёмник; приёмник = сумма. lock xadd — атомарный счётчик.", kArith});
    Add(m, "cmpxchg", {"cmpxchg приёмник, источник",
                       "Если rax = приёмник: приёмник = источник, ZF = 1; иначе rax = приёмник, ZF = 0. "
                       "lock cmpxchg — атомарное «сравнить и заменить».", kArith});
    Add(m, "cpuid", {"cpuid", "Сведения о процессоре: функция в eax (и ecx), результат в eax, ebx, ecx, edx.", kNoFlags});
    Add(m, "rdtsc", {"rdtsc", "Счётчик тактов процессора: edx:eax.", kNoFlags});

    // ---- SSE (scalar float/double)
    Add(m, "movss", {"movss xmm, xmm/m32 | movss m32, xmm", "Копирует одно число float (32 бит).", kNoFlags});
    Add(m, "movsd", {"movsd xmm, xmm/m64 | movsd (без операндов)",
                     "С операндами xmm — копирует одно число double (64 бит). Без операндов — строковая инструкция: "
                     "копирует 4 байта [rsi] → [rdi].", kNoFlags});
    Add(m, "movaps movapd movdqa", {"movaps xmm, xmm/m128",
                                    "Копирует 16 байт. Адрес в памяти должен быть выровнен на 16 — иначе исключение.",
                                    kNoFlags});
    Add(m, "movups movupd movdqu", {"movups xmm, xmm/m128", "Копирует 16 байт без требования выравнивания.", kNoFlags});
    Add(m, "movd", {"movd xmm, r/m32 | movd r/m32, xmm", "Копирует 32 бита между регистром общего назначения и xmm.",
                    kNoFlags});
    Add(m, "movq", {"movq xmm, r/m64 | movq r/m64, xmm", "Копирует 64 бита между регистром общего назначения и xmm.",
                    kNoFlags});
    for (const char* op : {"add", "sub", "mul", "div", "min", "max", "sqrt"}) {
        std::string o = op;
        std::string what = o == "add" ? "сложение" : o == "sub" ? "вычитание" : o == "mul" ? "умножение"
                         : o == "div" ? "деление" : o == "min" ? "минимум" : o == "max" ? "максимум" : "квадратный корень";
        m[o + "ss"] = {o + "ss xmm, xmm/m32", "Скалярное " + what + " чисел float (младшие 32 бита xmm).", kNoFlags};
        m[o + "sd"] = {o + "sd xmm, xmm/m64", "Скалярное " + what + " чисел double (младшие 64 бита xmm).", kNoFlags};
        m[o + "ps"] = {o + "ps xmm, xmm/m128", "Векторное " + what + ": 4 числа float сразу.", kNoFlags};
        m[o + "pd"] = {o + "pd xmm, xmm/m128", "Векторное " + what + ": 2 числа double сразу.", kNoFlags};
    }
    Add(m, "cvtsi2sd", {"cvtsi2sd xmm, r/m32/64", "Целое со знаком → double.", kNoFlags});
    Add(m, "cvtsi2ss", {"cvtsi2ss xmm, r/m32/64", "Целое со знаком → float.", kNoFlags});
    Add(m, "cvttsd2si", {"cvttsd2si r32/64, xmm/m64", "double → целое с отбрасыванием дробной части.", kNoFlags});
    Add(m, "cvttss2si", {"cvttss2si r32/64, xmm/m32", "float → целое с отбрасыванием дробной части.", kNoFlags});
    Add(m, "cvtsd2si", {"cvtsd2si r32/64, xmm/m64", "double → целое с округлением по текущему режиму (к ближайшему).",
                        kNoFlags});
    Add(m, "cvtsd2ss", {"cvtsd2ss xmm, xmm/m64", "double → float.", kNoFlags});
    Add(m, "cvtss2sd", {"cvtss2sd xmm, xmm/m32", "float → double.", kNoFlags});
    Add(m, "comisd ucomisd", {"comisd xmm, xmm/m64",
                              "Сравнивает два double и выставляет флаги как при беззнаковом сравнении: дальше ja/jb/je. "
                              "Если хотя бы одно — NaN, ZF = PF = CF = 1.", "ZF PF CF; OF = SF = AF = 0"});
    Add(m, "comiss ucomiss", {"comiss xmm, xmm/m32", "То же для float: флаги как при беззнаковом сравнении (ja/jb/je).",
                              "ZF PF CF; OF = SF = AF = 0"});
    Add(m, "xorps xorpd pxor", {"xorps xmm, xmm/m128", "Побитовое XOR 128 бит. xorps xmm0, xmm0 — обнуление.", kNoFlags});
    Add(m, "andps andpd pand", {"andps xmm, xmm/m128", "Побитовое AND 128 бит (например, модуль числа по маске).",
                                kNoFlags});

    // ---- x87
    Add(m, "fld", {"fld m32/m64/m80 | fld st(i)", "Загружает число в стек FPU (st0).", "C1 FPU"});
    Add(m, "fild", {"fild m16/m32/m64", "Загружает целое число в стек FPU с преобразованием.", "C1 FPU"});
    Add(m, "fst fstp", {"fstp m32/m64/m80 | fstp st(i)", "Сохраняет st0 (fstp — затем снимает его со стека FPU).",
                        "C1 FPU"});
    Add(m, "fistp", {"fistp m16/m32/m64", "Сохраняет st0 как целое (с округлением) и снимает его со стека.", "C1 FPU"});
    Add(m, "fadd faddp fsub fsubp fmul fmulp fdiv fdivp",
        {"fadd m | fadd st0, st(i) | faddp", "Арифметика x87: результат в st0 (или st(i) у формы с p, со снятием).",
         "C1 FPU"});
    Add(m, "finit fninit", {"finit", "Сбрасывает FPU в начальное состояние.", "все флаги FPU"});

    // ---- NASM directives
    Add(m, "db", {"имя db значения", "Байты (8 бит): числа, символы, строки. msg db \"Hi\", 10, 0 — строка с переводом "
                                    "строки и нулём в конце.", ""});
    Add(m, "dw", {"имя dw значения", "Слова по 2 байта.", ""});
    Add(m, "dd", {"имя dd значения", "Двойные слова по 4 байта (int32, float).", ""});
    Add(m, "dq", {"имя dq значения", "Учетверённые слова по 8 байт (int64, double, адреса).", ""});
    Add(m, "dt", {"имя dt значение", "10 байт (80-битное число x87).", ""});
    Add(m, "resb resw resd resq", {"имя resb количество",
                                   "Резервирует место без начального значения (обычно в section .bss): resb — байты, "
                                   "resw — по 2, resd — по 4, resq — по 8.", ""});
    Add(m, "equ", {"ИМЯ equ выражение", "Константа на этапе сборки (не занимает памяти). len equ $ - msg — длина строки.",
                   ""});
    Add(m, "times", {"times количество инструкция/данные", "Повторяет данные или инструкцию: times 64 db 0.", ""});
    Add(m, "section segment", {"section .text | .data | .bss | .rdata",
                               ".text — код, .data — изменяемые данные, .rdata — константы, .bss — неинициализированные "
                               "данные.", ""});
    Add(m, "global", {"global имя", "Делает метку видимой линкеру и другим файлам (например, global main).", ""});
    Add(m, "extern", {"extern имя", "Объявляет символ из другого файла или библиотеки (extern printf).", ""});
    Add(m, "default", {"default rel | default abs",
                       "default rel — обращения [метка] относительно rip: так нужно для 64-битного кода Windows.", ""});
    Add(m, "bits", {"bits 16 | 32 | 64", "Разрядность генерируемого кода (для -f win64 по умолчанию 64).", ""});
    Add(m, "align alignb", {"align n", "Выравнивает следующий адрес на n байт (заполняет nop или нулями).", ""});
    Add(m, "%define", {"%define ИМЯ значение", "Текстовая подстановка препроцессора (как #define в C).", ""});
    Add(m, "%assign", {"%assign ИМЯ выражение", "Числовая переменная препроцессора (можно переопределять).", ""});
    Add(m, "%macro", {"%macro имя число_параметров … %endmacro",
                      "Многострочный макрос; параметры внутри — %1, %2…, локальные метки — %%метка.", ""});
    Add(m, "%include", {"%include \"файл\"", "Вставляет текст файла (Ctrl+клик по имени открывает файл).", ""});
    Add(m, "%if %ifdef %ifndef %else %elif %endif", {"%ifdef ИМЯ … %else … %endif", "Условная сборка.", ""});
    Add(m, "%rep", {"%rep количество … %endrep", "Повторяет блок строк.", ""});

    // ---- generated: jcc, setcc, cmovcc
    for (const Condition& c : kConditions) {
        std::istringstream in(c.suffixes);
        std::string sfx;
        while (in >> sfx) {
            m["j" + sfx] = {"j" + sfx + " метка",
                            std::string("Переход, если ") + c.meaning + " (" + c.test + "). Обычно после cmp или test.",
                            kNoFlags};
            m["set" + sfx] = {"set" + sfx + " r/m8",
                              std::string("Байт = 1, если ") + c.meaning + " (" + c.test + "), иначе 0.", kNoFlags};
            m["cmov" + sfx] = {"cmov" + sfx + " регистр, r/m",
                               std::string("Копирует источник в регистр, только если ") + c.meaning + " (" + c.test +
                                   "). Без перехода — нет промахов предсказания.", kNoFlags};
        }
    }

    // ---- registers (Win64 calling convention roles)
    struct Reg {
        const char* names;
        const char* text;
    };
    const Reg regs[] = {
        {"rax eax ax al ah", "rax (eax, ax, al/ah). Возвращаемое значение функции; неявный операнд mul/div. "
                             "Изменяемый: вызываемая функция может его испортить."},
        {"rbx ebx bx bl bh", "rbx (ebx, bx, bl/bh). Сохраняемый: если функция его меняет, она должна восстановить."},
        {"rcx ecx cx cl ch", "rcx (ecx, cx, cl/ch). 1-й целый аргумент в Win64; счётчик loop и rep; cl — счётчик сдвигов. "
                             "Изменяемый."},
        {"rdx edx dx dl dh", "rdx (edx, dx, dl/dh). 2-й аргумент в Win64; старшая половина mul/div. Изменяемый."},
        {"rsi esi si sil", "rsi (esi, si, sil). Источник строковых инструкций. Сохраняемый в Win64."},
        {"rdi edi di dil", "rdi (edi, di, dil). Приёмник строковых инструкций. Сохраняемый в Win64."},
        {"rbp ebp bp bpl", "rbp (ebp, bp, bpl). Указатель кадра стека (если используется). Сохраняемый."},
        {"rsp esp sp spl", "rsp — указатель стека. Перед call в Win64 должен быть кратен 16; функция выделяет 32 байта "
                           "теневого места для аргументов."},
        {"r8 r8d r8w r8b r8l", "r8 (r8d, r8w, r8b). 3-й аргумент в Win64. Изменяемый."},
        {"r9 r9d r9w r9b r9l", "r9 (r9d, r9w, r9b). 4-й аргумент в Win64; остальные аргументы — в стеке. Изменяемый."},
        {"r10 r10d r10w r10b r10l r11 r11d r11w r11b r11l", "r10, r11 — временные (изменяемые) регистры. r11 портится в syscall."},
        {"r12 r12d r12w r12b r12l r13 r13d r13w r13b r13l r14 r14d r14w r14b r14l r15 r15d r15w r15b r15l",
         "r12–r15 — сохраняемые: функция обязана восстановить их перед возвратом."},
        {"rip eip", "rip — адрес следующей инструкции. [rel метка] адресует относительно rip."},
    };
    for (const Reg& r : regs) {
        std::istringstream in(r.names);
        std::string n;
        while (in >> n) {
            std::string size = RegisterSize(n);
            m[n] = {n + " — регистр, " + size, r.text, ""};
        }
    }
    for (int i = 0; i < 16; i++) {
        std::string x = "xmm" + std::to_string(i);
        m[x] = {x + " — регистр SSE, 128 бит",
                i < 4 ? "Аргументы float/double 1–4 в Win64 (xmm0 — и возвращаемое значение). Изменяемый."
                      : i < 6 ? "Изменяемый (может портиться при вызове функции)."
                              : "Сохраняемый в Win64: функция обязана восстановить его.",
                ""};
    }
    return m;
}

const std::unordered_map<std::string, Entry>& Table() {
    static const auto table = Build();
    return table;
}

std::string LowerAscii(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

// Word wrap at ~width characters (UTF-8 aware), keeping explicit line breaks.
std::string Wrap(const std::string& text, size_t width) {
    std::string out, line, word;
    auto chars = [](const std::string& s) {
        return static_cast<size_t>(std::count_if(s.begin(), s.end(), [](char c) { return (c & 0xC0) != 0x80; }));
    };
    auto flushWord = [&] {
        if (word.empty()) return;
        if (!line.empty() && chars(line) + 1 + chars(word) > width) {
            out += line + "\n";
            line.clear();
        }
        line += (line.empty() ? "" : " ") + word;
        word.clear();
    };
    for (char c : text) {
        if (c == ' ') {
            flushWord();
        } else if (c == '\n') {
            flushWord();
            out += line + "\n";
            line.clear();
        } else {
            word += c;
        }
    }
    flushWord();
    out += line;
    return out;
}
}

std::string For(const std::string& word) {
    auto it = Table().find(LowerAscii(word));
    if (it == Table().end()) return {};
    const Entry& e = it->second;
    std::string card = e.syntax + "\n" + Wrap(e.text, 64);
    if (!e.flags.empty()) card += "\nФлаги: " + e.flags;
    return card;
}

std::string Signature(const std::string& mnemonic) {
    auto it = Table().find(LowerAscii(mnemonic));
    if (it == Table().end() || it->second.flags.empty()) return {};  // instructions only
    const std::string& text = it->second.text;
    size_t dot = text.find(". ");
    return it->second.syntax + "\n" + Wrap(dot == std::string::npos ? text : text.substr(0, dot + 1), 64);
}

size_t HeaderLength(const std::string& card) {
    size_t nl = card.find('\n');
    return nl == std::string::npos ? card.size() : nl;
}

}
