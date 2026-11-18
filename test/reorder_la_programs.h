#pragma once
// Random programs that use `la` in the middle of blocks, shared by the
// pinning and assembly-writer tests.

#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "reorder/decoded_inst.h"

namespace reorder_test
{

struct LaProgram
{
    std::string name;
    std::string source;
};

// A .data section with four 8-word arrays, three base registers (x10-x12)
// pointed at them with la (and re-pointed mid-block), loads/stores through
// those bases, and ALU work including addi immediates whose low bits name a
// live register (the hazard-unit quirk). Straight-line: every la expands to
// auipc + addi, and the cost model is exact for the VM on this code.
inline std::string makeLaProgram(std::mt19937 &rng, int bodyLen)
{
    static const int baseReg[] = {10, 11, 12};
    static const int pool[] = {5, 6, 7, 8, 9, 13, 14, 15, 16, 17, 18, 19, 20};
    static const char *rOps[] = {"add", "sub", "and", "or", "xor", "mul"};
    auto pick = [&rng](int n) { return std::uniform_int_distribution<int>(0, n - 1)(rng); };

    std::ostringstream out;
    out << ".data\n";
    for (int a = 0; a < 4; ++a)
    {
        out << "arr" << a << ": .word";
        for (int k = 0; k < 8; ++k)
        {
            out << (k ? ", " : " ") << (10 * a + k + 1);
        }
        out << "\n";
    }
    out << ".text\n";
    for (int b : baseReg)
    {
        out << "la x" << b << ", arr" << pick(4) << "\n";
    }
    for (int i = 0; i < bodyLen; ++i)
    {
        const int op = pick(10);
        const int base = baseReg[pick(3)];
        const int rd = pool[pick(13)], rs1 = pool[pick(13)], rs2 = pool[pick(13)];
        if (op <= 1)
        {
            out << "la x" << base << ", arr" << pick(4) << "\n";
        }
        else if (op == 2)
        {
            out << "lw x" << rd << ", " << 4 * pick(8) << "(x" << base << ")\n";
        }
        else if (op == 3)
        {
            out << "sw x" << rs2 << ", " << 4 * pick(8) << "(x" << base << ")\n";
        }
        else if (op == 4)
        {
            int imm = pick(2048);
            if (pick(2) == 1)
            {
                imm = (imm & ~0x1F) | pool[pick(13)];
            }
            out << "addi x" << rd << ", x" << rs1 << ", " << imm << "\n";
        }
        else
        {
            out << rOps[pick(6)] << " x" << rd << ", x" << rs1 << ", x" << rs2 << "\n";
        }
    }
    return out.str();
}

inline std::vector<LaProgram> laPrograms(unsigned seed, int count)
{
    std::mt19937 rng(seed);
    std::vector<LaProgram> programs;
    for (int t = 0; t < count; ++t)
    {
        const int len = std::uniform_int_distribution<int>(12, 25)(rng);
        programs.push_back({"la#" + std::to_string(t), makeLaProgram(rng, len)});
    }
    return programs;
}

// Indices i where `la` put auipc at i and its `addi rd, rd, lo` at i + 1.
inline std::vector<size_t> laPairs(const std::vector<uint32_t> &words)
{
    using Kites::reorder::decode;
    using Kites::reorder::Kind;
    std::vector<size_t> pairs;
    for (size_t i = 0; i + 1 < words.size(); ++i)
    {
        const auto a = decode(words[i]);
        const auto b = decode(words[i + 1]);
        if (a.kind == Kind::Auipc && b.kind == Kind::OpImm && ((b.word >> 12) & 0x7) == 0 && b.rd == a.rd &&
            b.rs1 == a.rd)
        {
            pairs.push_back(i);
        }
    }
    return pairs;
}

} // namespace reorder_test
