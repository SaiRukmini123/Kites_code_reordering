#pragma once

#include <array>
#include <cstdint>
#include <iostream>
#include <memory>
#include <streambuf>
#include <utility>
#include <vector>

#include "assembler/assembler.h"
#include "config/config.h"
#include "processor/rv5s/rv5s_processor_base.h"

namespace Kites
{
struct Seed
{
    std::vector<std::pair<int, uint64_t>> regs;          // (index, value)
    std::vector<std::pair<uint64_t, uint32_t>> memWords; // (address, value)
};

struct RunResult
{
    unsigned cycles{}, stalls{}, retired{};
    std::array<uint64_t, 32> regs{};
    std::vector<uint32_t> dataWords; // window from data start
    std::vector<uint32_t> stackWords;
    bool hitCap{};
};

namespace reorder_detail
{
struct NullBuffer : std::streambuf
{
    int overflow(int c) override { return c; }
};

// Silences std::cout while alive; the VM prints a line per cycle.
class CoutSilencer
{
public:
    CoutSilencer() { m_old = std::cout.rdbuf(&m_null); }
    ~CoutSilencer() { std::cout.rdbuf(m_old); }

private:
    NullBuffer m_null;
    std::streambuf *m_old = nullptr;
};
} // namespace reorder_detail

template <class VM>
struct Steppable : VM
{
    using Kites::RV5StageVM_Base::StepAndNotify;
};

inline AssembledProgram withText(const AssembledProgram &orig, const std::vector<uint32_t> &words)
{
    AssembledProgram p = orig;
    for (size_t i = 0; i < words.size() && i < p.text_buffer.size(); ++i)
    {
        p.text_buffer[i] = words[i];
    }
    return p;
}

template <class VM>
RunResult runProgram(const AssembledProgram &prog, const Seed &seed, unsigned cap,
                     unsigned dataWindowWords = 64, uint64_t stackTop = 0,
                     unsigned stackWindowWords = 64)
{
    reorder_detail::CoutSilencer silence;
    auto vm = std::make_unique<Steppable<VM>>(); // fresh VM every run: registers survive LoadProgram
    vm->LoadProgram(prog);
    for (const auto &[reg, value] : seed.regs)
    {
        if (reg > 0)
        {
            vm->registers_.WriteGpr(reg, value);
        }
    }
    for (const auto &[address, value] : seed.memWords)
    {
        vm->memory_controller_.writeWord_d(address, value); // same path LoadProgram uses
    }

    RunResult res;
    while (true)
    {
        if (vm->cycle_s_ >= cap)
        {
            res.hitCap = true;
            break;
        }
        if (!vm->StepAndNotify())
        {
            break;
        }
    }
    res.cycles = vm->cycle_s_;
    res.stalls = vm->total_stalls_;
    res.retired = vm->instructions_retired_;
    for (int i = 0; i < 32; ++i)
    {
        res.regs[i] = vm->registers_.ReadGpr(i);
    }
    uint64_t dataStart = vm_config::config.getDataSectionStart();
    for (unsigned i = 0; i < dataWindowWords; ++i)
    {
        res.dataWords.push_back(vm->memory_controller_.readWord_d(dataStart + 4ull * i));
    }
    if (stackTop)
    {
        for (unsigned i = 1; i <= stackWindowWords; ++i)
        {
            res.stackWords.push_back(vm->memory_controller_.readWord_d(stackTop - 4ull * i));
        }
    }
    return res;
}

// Two runs are equivalent only if both finished (neither hit the cycle cap).
inline bool sameState(const RunResult &a, const RunResult &b)
{
    return !a.hitCap && !b.hitCap && a.regs == b.regs && a.dataWords == b.dataWords &&
           a.stackWords == b.stackWords;
}
} // namespace Kites