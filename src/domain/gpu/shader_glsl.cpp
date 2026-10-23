#include "domain/gpu/shader_glsl.h"

#include <array>
#include <format>
#include <set>
#include <string_view>
#include <vector>

namespace hexlith::domain::gpu {
namespace {

// The opcodes and fields are the interpreter's; shader_interpreter.cpp cites
// GBATEK for each.
namespace opcode {
constexpr std::uint32_t Add = 0x00;
constexpr std::uint32_t Dp3 = 0x01;
constexpr std::uint32_t Dp4 = 0x02;
constexpr std::uint32_t Dph = 0x03;
constexpr std::uint32_t Ex2 = 0x05;
constexpr std::uint32_t Lg2 = 0x06;
constexpr std::uint32_t Mul = 0x08;
constexpr std::uint32_t Sge = 0x09;
constexpr std::uint32_t Slt = 0x0A;
constexpr std::uint32_t Flr = 0x0B;
constexpr std::uint32_t Max = 0x0C;
constexpr std::uint32_t Min = 0x0D;
constexpr std::uint32_t Rcp = 0x0E;
constexpr std::uint32_t Rsq = 0x0F;
constexpr std::uint32_t Mova = 0x12;
constexpr std::uint32_t Mov = 0x13;
constexpr std::uint32_t Dphi = 0x18;
constexpr std::uint32_t Sgei = 0x1A;
constexpr std::uint32_t Slti = 0x1B;
constexpr std::uint32_t Break = 0x20;
constexpr std::uint32_t Nop = 0x21;
constexpr std::uint32_t End = 0x22;
constexpr std::uint32_t Breakc = 0x23;
constexpr std::uint32_t Call = 0x24;
constexpr std::uint32_t Callc = 0x25;
constexpr std::uint32_t Callu = 0x26;
constexpr std::uint32_t Ifu = 0x27;
constexpr std::uint32_t Ifc = 0x28;
constexpr std::uint32_t Loop = 0x29;
constexpr std::uint32_t Emit = 0x2A;
constexpr std::uint32_t SetEmit = 0x2B;
constexpr std::uint32_t Jmpc = 0x2C;
constexpr std::uint32_t Jmpu = 0x2D;
constexpr std::uint32_t CmpPair = 0x2E >> 1;
constexpr std::uint32_t MadiGroup = 0x30 >> 3;
constexpr std::uint32_t MadGroup = 0x38 >> 3;
}  // namespace opcode

constexpr std::uint32_t Field(std::uint32_t word, unsigned low, unsigned width) {
    return (word >> low) & ((1U << width) - 1);
}

constexpr std::uint32_t LastInstruction = ShaderUnitState::CodeWords - 1;
constexpr std::string_view Components = "xyzw";

// What every translation shares: the registers, the PICA's arithmetic, and
// the three stacks of control flow with the interpreter's rules.
constexpr std::string_view Prelude = R"(vec4 pica_v[16];
vec4 pica_r[16];
vec4 pica_o[16];
ivec3 pica_a;
bvec2 pica_cmp;
bool pica_unsupported;

// Zero times infinity is zero, not a NaN (ADR 0018).
float pica_mul(float a, float b) {
    precise float product = a * b;
    return (isnan(product) && !isnan(a) && !isnan(b)) ? 0.0 : product;
}

vec4 pica_mul4(vec4 a, vec4 b) {
    return vec4(pica_mul(a.x, b.x), pica_mul(a.y, b.y), pica_mul(a.z, b.z), pica_mul(a.w, b.w));
}

// The first operand when it is the greater or the lesser, the second
// otherwise, so that a NaN first loses and a NaN second wins.
vec4 pica_max4(vec4 a, vec4 b) {
    return vec4(a.x > b.x ? a.x : b.x, a.y > b.y ? a.y : b.y, a.z > b.z ? a.z : b.z, a.w > b.w ? a.w : b.w);
}

vec4 pica_min4(vec4 a, vec4 b) {
    return vec4(a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y, a.z < b.z ? a.z : b.z, a.w < b.w ? a.w : b.w);
}

// MOVA's conversion, a NaN or a value out of range becoming the most negative
// integer.
int pica_trunc(float value) {
    return (isnan(value) || value >= 2147483648.0 || value < -2147483648.0) ? (-2147483647 - 1) : int(value);
}

// A uniform addressed relative to an address register: an offset outside a
// signed byte counts as zero, the index wraps at 80h, past c95 reads ones.
vec4 pica_relative(uint index, int offset) {
    if (offset < -128 || offset > 127) {
        offset = 0;
    }
    uint at = (index + uint(offset)) & 0x7Fu;
    return at >= 96u ? vec4(1.0) : pica_float(at);
}

// The stacks drop their oldest entry when pushed full.
uvec2 pica_ifs[8];
int pica_if_count;
uvec2 pica_calls[4];
int pica_call_count;
// Entry, end, iterations left and increment; the counter to restore.
uvec4 pica_loops[4];
int pica_loop_counters[4];
int pica_loop_count;

void pica_push_if(uint else_address, uint end_address) {
    if (pica_if_count == 8) {
        for (int at = 1; at < 8; ++at) {
            pica_ifs[at - 1] = pica_ifs[at];
        }
        --pica_if_count;
    }
    pica_ifs[pica_if_count] = uvec2(else_address, end_address);
    ++pica_if_count;
}

void pica_push_call(uint end_address, uint return_address) {
    if (pica_call_count == 4) {
        for (int at = 1; at < 4; ++at) {
            pica_calls[at - 1] = pica_calls[at];
        }
        --pica_call_count;
    }
    pica_calls[pica_call_count] = uvec2(end_address, return_address);
    ++pica_call_count;
}

void pica_push_loop(uint entry_address, uint end_address, uint parameters) {
    if (pica_loop_count == 4) {
        for (int at = 1; at < 4; ++at) {
            pica_loops[at - 1] = pica_loops[at];
            pica_loop_counters[at - 1] = pica_loop_counters[at];
        }
        --pica_loop_count;
    }
    int start = int((parameters >> 8u) & 0xFFu);
    pica_a.z = start;
    pica_loops[pica_loop_count] = uvec4(entry_address, end_address, parameters & 0xFFu, (parameters >> 16u) & 0xFFu);
    pica_loop_counters[pica_loop_count] = start;
    ++pica_loop_count;
}

// After every instruction: calls close first, up to four at once, the fourth
// leaving the counter alone; then an if; then a loop.
uint pica_close_scopes(uint old_counter, bool breaking, uint counter) {
    uint next = old_counter + 1u;
    for (int closed = 0; closed < 4; ++closed) {
        if (pica_call_count == 0 || pica_calls[pica_call_count - 1].x != next) {
            break;
        }
        if (closed < 3) {
            counter = pica_calls[pica_call_count - 1].y;
            next = counter;
        }
        --pica_call_count;
    }
    if (pica_if_count > 0 && pica_ifs[pica_if_count - 1].x == old_counter + 1u) {
        counter = pica_ifs[pica_if_count - 1].y;
        --pica_if_count;
    }
    if (pica_loop_count > 0 && (pica_loops[pica_loop_count - 1].y == old_counter + 1u || breaking)) {
        int top = pica_loop_count - 1;
        pica_a.z += int(pica_loops[top].w);
        if (!breaking && pica_loops[top].z != 0u) {
            pica_loops[top].z -= 1u;
            counter = pica_loops[top].x;
        } else {
            counter = pica_loops[top].y;
            if (pica_loop_count > 1) {
                pica_a.z = pica_loop_counters[top];
            }
            --pica_loop_count;
        }
    }
    return counter;
}
)";

// An operand descriptor, as the interpreter reads it.
class Descriptor {
public:
    explicit Descriptor(std::uint32_t word) : word_(word) {}

    [[nodiscard]] bool Writes(std::size_t component) const {
        return Field(word_, static_cast<unsigned>(3 - component), 1) != 0;
    }

    // A source with its selectors and negation applied.
    [[nodiscard]] std::string Apply(std::size_t source, const std::string& value) const {
        constexpr std::array<unsigned, 3> NegateBits = {4, 13, 22};
        constexpr std::array<unsigned, 3> FirstSelector = {11, 20, 29};
        std::string swizzle;
        for (std::size_t component = 0; component < Components.size(); ++component) {
            const unsigned shift =
                FirstSelector.at(source) - (2 * static_cast<unsigned>(component));
            swizzle += Components.at(Field(word_, shift, 2));
        }
        const std::string selected = std::format("{}.{}", value, swizzle);
        return Field(word_, NegateBits.at(source), 1) != 0 ? std::format("(-{})", selected)
                                                           : selected;
    }

private:
    std::uint32_t word_;
};

std::string SourceRegister(std::uint32_t raw, std::uint32_t relative) {
    constexpr std::uint32_t FirstTemporary = 0x10;
    constexpr std::uint32_t FirstUniform = 0x20;
    constexpr std::array<std::string_view, 4> Offsets = {"", "pica_a.x", "pica_a.y", "pica_a.z"};
    if (raw < FirstTemporary) {
        return std::format("pica_v[{}]", raw);
    }
    if (raw < FirstUniform) {
        return std::format("pica_r[{}]", raw - FirstTemporary);
    }
    if (relative == 0) {
        return std::format("pica_float({}u)", raw - FirstUniform);
    }
    return std::format("pica_relative({}u, {})", raw - FirstUniform, Offsets.at(relative));
}

std::string DestinationRegister(std::uint32_t raw) {
    constexpr std::uint32_t FirstTemporary = 0x10;
    return raw < FirstTemporary ? std::format("pica_o[{}]", raw)
                                : std::format("pica_r[{}]", raw - FirstTemporary);
}

// The masked components of a result written to a register.
std::string
Write(std::uint32_t destination, const Descriptor& descriptor, const std::string& result) {
    std::string text = std::format("            precise vec4 result = {};\n", result);
    const std::string target = DestinationRegister(destination);
    for (std::size_t component = 0; component < Components.size(); ++component) {
        if (descriptor.Writes(component)) {
            text += std::format(
                "            {0}.{1} = result.{1};\n", target, Components.at(component));
        }
    }
    return text;
}

// Format 2's condition, its reference bits and operator known.
std::string Condition(std::uint32_t word) {
    std::string x = std::format("(pica_cmp.x == {})", Field(word, 25, 1) != 0 ? "true" : "false");
    std::string y = std::format("(pica_cmp.y == {})", Field(word, 24, 1) != 0 ? "true" : "false");
    switch (Field(word, 22, 2)) {
    case 0:
        return std::format("({} || {})", x, y);
    case 1:
        return std::format("({} && {})", x, y);
    case 2:
        return x;
    default:
        return y;
    }
}

std::string Bool(std::uint32_t word) {
    return std::format("pica_bool({}u)", Field(word, 22, 4));
}

class Translator {
public:
    Translator(const ShaderUnitState& unit, std::uint32_t entry) : unit_(&unit), entry_(entry) {}

    GlslShader Translate() {
        GlslShader shader;
        shader.source = std::string{Prelude};
        shader.source +=
            std::format("\n// The program from {:03X}h.\nbool pica_run(uint limit) {{\n", entry_);
        shader.source +=
            "    pica_if_count = 0;\n    pica_call_count = 0;\n    pica_loop_count = 0;\n    pica_unsupported = false;\n";
        shader.source += std::format("    uint counter = {}u;\n", entry_);
        shader.source += "    for (uint steps = 0u; steps < limit; ++steps) {\n";
        shader.source += "        uint old_counter = counter;\n        bool breaking = false;\n";
        shader.source += "        switch (counter) {\n";
        for (const std::uint32_t address : Reachable()) {
            shader.source += std::format(
                "        case {}u: {{ // {:03X}h: {:08X}\n", address, address, Word(address));
            shader.source += Instruction(address);
            shader.source += "            break;\n        }\n";
        }
        // The last word of code memory is END whatever it holds, and nothing
        // else is ever reached.
        shader.source += "        default:\n            return true;\n        }\n";
        shader.source +=
            "        counter = pica_close_scopes(old_counter, breaking, counter + 1u);\n";
        shader.source += "    }\n    return false;\n}\n";
        shader.unsupported = unsupported_;
        return shader;
    }

private:
    [[nodiscard]] std::uint32_t Word(std::uint32_t address) const {
        return address < LastInstruction ? unit_->code.at(address) : opcode::End << 26U;
    }

    [[nodiscard]] static bool IsFlow(std::uint32_t word) {
        return Field(word, 29, 3) != opcode::MadGroup && Field(word, 29, 3) != opcode::MadiGroup &&
               Field(word, 27, 5) != opcode::CmpPair && Field(word, 26, 6) >= opcode::Break;
    }

    // Every address the counter can take: each instruction's successor, and
    // for flow control its target, the target's successor and the end of the
    // scope it opens. A loop's end and a scope's return are among those.
    [[nodiscard]] std::set<std::uint32_t> Reachable() const {
        std::set<std::uint32_t> reached;
        std::vector<std::uint32_t> pending = {entry_};
        while (!pending.empty()) {
            const std::uint32_t address = pending.back();
            pending.pop_back();
            if (address >= LastInstruction || !reached.insert(address).second) {
                continue;
            }
            const std::uint32_t word = Word(address);
            if (IsFlow(word) && Field(word, 26, 6) == opcode::End) {
                continue;
            }
            pending.push_back(address + 1);
            if (IsFlow(word)) {
                const std::uint32_t target = Field(word, 10, 12);
                const std::uint32_t count = Field(word, 0, 8);
                pending.insert(pending.end(), {target, target + 1, target + count});
            }
        }
        return reached;
    }

    [[nodiscard]] std::string Instruction(std::uint32_t address) {
        const std::uint32_t word = Word(address);
        if (Field(word, 29, 3) == opcode::MadGroup || Field(word, 29, 3) == opcode::MadiGroup) {
            return MultiplyAdd(word);
        }
        if (Field(word, 27, 5) == opcode::CmpPair) {
            return Compare(word);
        }
        const std::uint32_t code = Field(word, 26, 6);
        return code < opcode::Break ? Arithmetic(word, code) : FlowControl(word, code, address);
    }

    std::string Unsupported() {
        unsupported_ = true;
        return "            pica_unsupported = true;\n";
    }

    std::string Arithmetic(std::uint32_t word, std::uint32_t code);
    std::string MultiplyAdd(std::uint32_t word);
    std::string Compare(std::uint32_t word);
    std::string FlowControl(std::uint32_t word, std::uint32_t code, std::uint32_t address);

    const ShaderUnitState* unit_;
    std::uint32_t entry_;
    bool unsupported_ = false;
};

std::string Translator::Arithmetic(std::uint32_t word, std::uint32_t code) {
    const bool inverted = code == opcode::Dphi || code == opcode::Sgei || code == opcode::Slti;
    const std::uint32_t relative = Field(word, 19, 2);
    const Descriptor descriptor{unit_->descriptors.at(Field(word, 0, 7))};
    const std::string first =
        descriptor.Apply(0,
                         inverted ? SourceRegister(Field(word, 14, 5), 0)
                                  : SourceRegister(Field(word, 12, 7), relative));
    const std::string second =
        descriptor.Apply(1,
                         inverted ? SourceRegister(Field(word, 7, 7), relative)
                                  : SourceRegister(Field(word, 7, 5), 0));
    const std::uint32_t destination = Field(word, 21, 5);
    const auto dot = [&](std::size_t terms, bool homogeneous) {
        std::string text = std::format(
            "            vec4 left = {};\n            vec4 right = {};\n", first, second);
        if (homogeneous) {
            text += "            left.w = 1.0;\n";
        }
        text += "            precise float sum = 0.0;\n";
        for (std::size_t c = 0; c < terms; ++c) {
            text += std::format("            sum = sum + pica_mul(left.{0}, right.{0});\n",
                                Components.at(c));
        }
        return text + Write(destination, descriptor, "vec4(sum)");
    };
    switch (code) {
    case opcode::Add:
        return Write(destination, descriptor, std::format("{} + {}", first, second));
    case opcode::Mul:
        return Write(destination, descriptor, std::format("pica_mul4({}, {})", first, second));
    case opcode::Max:
        return Write(destination, descriptor, std::format("pica_max4({}, {})", first, second));
    case opcode::Min:
        return Write(destination, descriptor, std::format("pica_min4({}, {})", first, second));
    case opcode::Sge:
    case opcode::Sgei:
        return Write(
            destination, descriptor, std::format("vec4(greaterThanEqual({}, {}))", first, second));
    case opcode::Slt:
    case opcode::Slti:
        return Write(destination, descriptor, std::format("vec4(lessThan({}, {}))", first, second));
    case opcode::Flr:
        return Write(destination, descriptor, std::format("floor({})", first));
    case opcode::Mov:
        return Write(destination, descriptor, first);
    case opcode::Dp3:
        return dot(3, false);
    case opcode::Dp4:
        return dot(4, false);
    case opcode::Dph:
    case opcode::Dphi:
        return dot(4, true);
    case opcode::Rcp:
        return Write(destination, descriptor, std::format("vec4(1.0 / {}.x)", first));
    case opcode::Rsq:
        return Write(destination, descriptor, std::format("vec4(1.0 / sqrt({}.x))", first));
    case opcode::Ex2:
        return Write(destination, descriptor, std::format("vec4(exp2({}.x))", first));
    case opcode::Lg2:
        return Write(destination, descriptor, std::format("vec4(log2({}.x))", first));
    case opcode::Mova: {
        std::string text = std::format("            vec4 value = {};\n", first);
        for (std::size_t c = 0; c < 2; ++c) {
            if (descriptor.Writes(c)) {
                text += std::format("            pica_a.{0} = pica_trunc(value.{0});\n",
                                    Components.at(c));
            }
        }
        return text;
    }
    default:
        return Unsupported();
    }
}

// MAD and MADI: not fused, the product rounded before the sum.
std::string Translator::MultiplyAdd(std::uint32_t word) {
    const bool inverted = Field(word, 29, 3) == opcode::MadiGroup;
    const std::uint32_t relative = Field(word, 22, 2);
    const Descriptor descriptor{unit_->descriptors.at(Field(word, 0, 5))};
    const std::string first = descriptor.Apply(0, SourceRegister(Field(word, 17, 5), 0));
    const std::string second =
        descriptor.Apply(1,
                         inverted ? SourceRegister(Field(word, 12, 5), 0)
                                  : SourceRegister(Field(word, 10, 7), relative));
    const std::string third =
        descriptor.Apply(2,
                         inverted ? SourceRegister(Field(word, 5, 7), relative)
                                  : SourceRegister(Field(word, 5, 5), 0));
    return Write(Field(word, 24, 5),
                 descriptor,
                 std::format("pica_mul4({}, {}) + {}", first, second, third));
}

// CMP's operators 6 and 7 always hold.
std::string Translator::Compare(std::uint32_t word) {
    constexpr std::array<std::string_view, 6> Operators = {"==", "!=", "<", "<=", ">", ">="};
    const Descriptor descriptor{unit_->descriptors.at(Field(word, 0, 7))};
    std::string text =
        std::format("            vec4 left = {};\n            vec4 right = {};\n",
                    descriptor.Apply(0, SourceRegister(Field(word, 12, 7), Field(word, 19, 2))),
                    descriptor.Apply(1, SourceRegister(Field(word, 7, 5), 0)));
    const std::array<std::uint32_t, 2> operators = {Field(word, 24, 3), Field(word, 21, 3)};
    for (std::size_t c = 0; c < 2; ++c) {
        const std::uint32_t op = operators.at(c);
        text += op < Operators.size()
                    ? std::format("            pica_cmp.{0} = left.{0} {1} right.{0};\n",
                                  Components.at(c),
                                  Operators.at(op))
                    : std::format("            pica_cmp.{} = true;\n", Components.at(c));
    }
    return text;
}

// A jump sets the counter one short of its target, since it steps on after
// every instruction.
std::string Translator::FlowControl(std::uint32_t word, std::uint32_t code, std::uint32_t address) {
    const std::uint32_t count = Field(word, 0, 8);
    const std::uint32_t target = Field(word, 10, 12);
    const std::string jump = std::format("counter = {}u - 1u;", target);
    const std::string call =
        std::format("pica_push_call({}u, {}u); {}", target + count, address + 1, jump);
    const std::string branch =
        std::format("pica_push_if({}u, {}u); }} else {{ {} }}", target, target + count, jump);
    switch (code) {
    case opcode::Nop:
        return "";
    case opcode::End:
        return "            return true;\n";
    case opcode::Break:
        return "            breaking = true;\n";
    case opcode::Breakc:
        return std::format("            breaking = {};\n", Condition(word));
    case opcode::Jmpc:
        return std::format("            if ({}) {{ {} }}\n", Condition(word), jump);
    case opcode::Jmpu:
        return std::format("            if ({} == {}) {{ {} }}\n",
                           Bool(word),
                           (count & 1U) == 0 ? "true" : "false",
                           jump);
    case opcode::Call:
        return std::format("            {}\n", call);
    case opcode::Callc:
        return std::format("            if ({}) {{ {} }}\n", Condition(word), call);
    case opcode::Callu:
        return std::format("            if ({}) {{ {} }}\n", Bool(word), call);
    case opcode::Ifc:
        return std::format("            if ({}) {{ {}\n", Condition(word), branch);
    case opcode::Ifu:
        return std::format("            if ({}) {{ {}\n", Bool(word), branch);
    case opcode::Loop:
        return std::format("            pica_push_loop({}u, {}u, pica_int({}u));\n",
                           address + 1,
                           target + 1,
                           Field(word, 22, 2));
    case opcode::Emit:
    case opcode::SetEmit:
    default:
        return Unsupported();
    }
}

}  // namespace

GlslShader TranslateShader(const ShaderUnitState& unit, std::uint32_t entry) {
    return Translator{unit, entry}.Translate();
}

}  // namespace hexlith::domain::gpu
