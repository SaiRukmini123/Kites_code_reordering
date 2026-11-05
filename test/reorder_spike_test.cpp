#include <gtest/gtest.h>

#include <chrono>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>

#include "assembler/assembler.h"
#include "processor/rv5s/rv5s_processor_h_f.h"
#include "processor/rv5s/rv5s_processor_h_nf.h"
#include "utils/utils.h"

namespace
{
struct NullBuffer : std::streambuf
{
    int overflow(int c) override { return c; }
};
struct SteppableHF : Kites::RV5StageProcessorHF
{
    using Kites::RV5StageVM_Base::StepAndNotify;
};
class CoutSilencer
{
public:
    CoutSilencer() { m_old = std::cout.rdbuf(&m_null); }
    ~CoutSilencer() { std::cout.rdbuf(m_old); }

private:
    NullBuffer m_null;
    std::streambuf *m_old = nullptr;
};

Kites::AssembledProgram assembleText(const std::string &source)
{
    std::istringstream stream(source);
    return Kites::assemble(stream);
}

// 2 * pairCount instructions, with a RAW hazard in every pair.
std::string pairProgram(int pairCount)
{
    std::string text;
    for (int i = 0; i < pairCount; ++i)
    {
        text += "addi x1, x0, 1\nadd x2, x1, x1\n";
    }
    return text;
}

struct Timing
{
    double assembleMs = 0, loadMs = 0, runMs = 0;
};

template <typename VmType>
Timing timeRuns(const std::string &source, int runCount, bool silenced)
{
    using Clock = std::chrono::steady_clock;
    auto ms = [](Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); };
    Kites::setupVmStateDirectory();
    std::unique_ptr<CoutSilencer> guard;
    if (silenced)
    {
        guard = std::make_unique<CoutSilencer>();
    }
    Timing t;
    for (int i = 0; i < runCount; ++i)
    {
        auto t0 = Clock::now();
        auto program = assembleText(source);
        auto t1 = Clock::now();
        auto vm = std::make_unique<VmType>();
        vm->LoadProgram(program);
        auto t2 = Clock::now();
        vm->DebugRun();
        auto t3 = Clock::now();
        t.assembleMs += ms(t1 - t0);
        t.loadMs += ms(t2 - t1);
        t.runMs += ms(t3 - t2);
    }
    t.assembleMs /= runCount;
    t.loadMs /= runCount;
    t.runMs /= runCount;
    return t;
}

void printTiming(const char *label, const Timing &t)
{
    std::cerr << "[SPIKE] " << label << "  assemble=" << t.assembleMs << "ms  load=" << t.loadMs
              << "ms  run=" << t.runMs << "ms  total=" << (t.assembleMs + t.loadMs + t.runMs)
              << "ms per run" << std::endl;
}
} // namespace

// ---- Experiment 1: do injected registers and memory survive? ----
TEST(ReorderSpike, InjectRegisterAfterLoad)
{
    Kites::setupVmStateDirectory();
    auto vm = std::make_unique<Kites::RV5StageProcessorHF>();
    vm->LoadProgram(assembleText("add x6, x5, x5\n"));
    vm->registers_.WriteGpr(5, 21);
    vm->DebugRun();
    auto x6 = vm->registers_.ReadGpr(6);
    std::cerr << "[SPIKE] inject reg AFTER LoadProgram: x6=" << x6
              << (x6 == 42 ? "  -> SURVIVES" : "  -> CLOBBERED") << std::endl;
}

TEST(ReorderSpike, InjectRegisterBeforeLoad)
{
    Kites::setupVmStateDirectory();
    auto vm = std::make_unique<Kites::RV5StageProcessorHF>();
    vm->registers_.WriteGpr(5, 21);
    vm->LoadProgram(assembleText("add x6, x5, x5\n"));
    vm->DebugRun();
    auto x6 = vm->registers_.ReadGpr(6);
    std::cerr << "[SPIKE] inject reg BEFORE LoadProgram: x6=" << x6
              << (x6 == 42 ? "  -> SURVIVES" : "  -> CLOBBERED") << std::endl;
}

TEST(ReorderSpike, InjectMemoryAfterLoad)
{
    Kites::setupVmStateDirectory();
    auto vm = std::make_unique<Kites::RV5StageProcessorHF>();
    vm->LoadProgram(assembleText("addi x10, x0, 100\nlw x7, 0(x10)\n"));
    vm->memory_controller_.writeWord(100, 77);
    vm->DebugRun();
    auto x7 = vm->registers_.ReadGpr(7);
    std::cerr << "[SPIKE] inject mem AFTER LoadProgram: x7=" << x7
              << (x7 == 77 ? "  -> SURVIVES" : "  -> CLOBBERED") << std::endl;
}

// ---- Experiment 2: how long does a run take, and where does the time go? ----
TEST(ReorderSpike, TimePerRun)
{
    for (int pairs : {3, 25, 100})
    {
        std::string label = std::to_string(pairs * 2) + " instr";
        printTiming((label + " loud    ").c_str(),
                    timeRuns<Kites::RV5StageProcessorHF>(pairProgram(pairs), 200, false));
        printTiming((label + " silenced").c_str(),
                    timeRuns<Kites::RV5StageProcessorHF>(pairProgram(pairs), 200, true));
    }
}

// ---- Experiment 3: how much does one run print? ----
TEST(ReorderSpike, HowMuchDoesOneRunPrint)
{
    Kites::setupVmStateDirectory();
    std::ostringstream sink;
    auto *old = std::cout.rdbuf(sink.rdbuf());
    auto vm = std::make_unique<Kites::RV5StageProcessorHF>();
    vm->LoadProgram(assembleText(pairProgram(25)));
    vm->DebugRun();
    std::cout.rdbuf(old);
    std::string out = sink.str();
    std::cerr << "[SPIKE] one 50-instr run printed " << out.size() << " bytes; first 120: "
              << out.substr(0, 120) << std::endl;
}

// ---- Experiment 4: can we cap cycles with our own step loop? ----
template <typename VmType>
unsigned int runWithCap(VmType &vm, unsigned int cap, bool &hitCap)
{
    hitCap = false;
    while (true)
    {
        if (vm.cycle_s_ >= cap)
        {
            hitCap = true;
            break;
        }
        if (!vm.StepAndNotify())
        {
            break;
        }
    }
    return vm.cycle_s_;
}

TEST(ReorderSpike, StepLoopMatchesDebugRun)
{
    Kites::setupVmStateDirectory();
    std::string source = pairProgram(5);
    CoutSilencer guard;

    auto a = std::make_unique<Kites::RV5StageProcessorHF>();
    a->LoadProgram(assembleText(source));
    a->DebugRun();

    auto b = std::make_unique<SteppableHF>();
    b->LoadProgram(assembleText(source));
    bool hitCap = false;
    runWithCap(*b, 100000, hitCap);

    std::cerr << "[SPIKE] DebugRun: cycles=" << a->cycle_s_ << " x2=" << a->registers_.ReadGpr(2)
              << " | step loop: cycles=" << b->cycle_s_ << " x2=" << b->registers_.ReadGpr(2)
              << (a->cycle_s_ == b->cycle_s_ ? "  -> MATCH" : "  -> DIFFERENT") << std::endl;
}

TEST(ReorderSpike, InitialSp)
{
    Kites::setupVmStateDirectory();
    auto vm = std::make_unique<Kites::RV5StageProcessorHF>();
    vm->LoadProgram(assembleText("addi x1, x0, 1\n"));
    std::cerr << "[SPIKE] sp(x2)=0x" << std::hex << vm->registers_.ReadGpr(2)
<< " dataStart=0x" << Kites::vm_config::config.getDataSectionStart() << std::endl;}

TEST(ReorderSpike, CapStopsInfiniteLoop)
{
    Kites::setupVmStateDirectory();
    CoutSilencer guard;
    auto vm = std::make_unique<SteppableHF>();
    vm->LoadProgram(assembleText("loop:\nbeq x0, x0, loop\n"));
    bool hitCap = false;
    auto start = std::chrono::steady_clock::now();
    runWithCap(*vm, 10000, hitCap);
    double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    std::cerr << "[SPIKE] infinite loop: hitCap=" << hitCap << " cycles=" << vm->cycle_s_
              << " took " << elapsed << "ms" << std::endl;
}