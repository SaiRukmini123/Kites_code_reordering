#include <gtest/gtest.h>

#include <iterator>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "assembler/assembler.h"
#include "processor/rv5s/rv5s_processor_h_f.h"
#include "processor/rv5s/rv5s_processor_h_nf.h"
#include "reorder/cost_model.h"
#include "utils/utils.h"

using namespace Kites::reorder;

namespace
{

// ---------------------------------------------------------------------
// Hand encoders, same idea as in reorder_dep_graph_test.cpp. Used for the
// pure, VM-free unit tests below.
// ---------------------------------------------------------------------
uint32_t addi(int rd, int rs1, int imm) { return ((imm & 0xFFF) << 20) | (rs1 << 15) | (rd << 7) | 0x13; }
uint32_t add(int rd, int rs1, int rs2)  { return (rs2 << 20) | (rs1 << 15) | (rd << 7) | 0x33; }
uint32_t lw(int rd, int rs1, int imm)   { return ((imm & 0xFFF) << 20) | (rs1 << 15) | (2 << 12) | (rd << 7) | 0x03; }

std::vector<DecodedInst> decodeAll(const std::vector<uint32_t> &words)
{
    std::vector<DecodedInst> insts;
    insts.reserve(words.size());
    for (uint32_t w : words)
    {
        insts.push_back(decode(w));
    }
    return insts;
}

// ---------------------------------------------------------------------
// VM-comparison helper: assemble `source`, run it on the real VM, and check
// that predictStalls() agrees with the VM's own total_stalls_ exactly.
// ---------------------------------------------------------------------
template <typename VmType>
void assertMatchesVm(const std::string &name, const std::string &source, PipelineKind kind)
{
    Kites::setupVmStateDirectory();

    auto vm = std::make_unique<VmType>();
    std::istringstream stream(source);
    Kites::AssembledProgram program = Kites::assemble(stream);
    vm->LoadProgram(program);
    vm->DebugRun();

    int predicted = predictStalls(program.text_buffer, kind);
    EXPECT_EQ(static_cast<int>(vm->total_stalls_), predicted)
        << name << ": VM stalls=" << vm->total_stalls_ << " predicted=" << predicted
        << "\nsource:\n"
        << source;
}

} // namespace

// =======================================================================
// Pure unit tests: predictStalls() on hand-built instruction sequences,
// no VM involved.
// =======================================================================

TEST(ReorderCostModel, HF_LoadUseAtDistance1)
{
    // lw x5, 0(x10)  then  add x6, x5, x5  -- real load-use, 1 stall.
    auto block = decodeAll({lw(5, 10, 0), add(6, 5, 5)});
    EXPECT_EQ(predictStalls(block, PipelineKind::HF), 1);
}

TEST(ReorderCostModel, HF_QuirkDistance1)
{
    // lw x5, 0(x10)  then  addi x6, x0, 5  -- addi's raw rs2 bits (imm low
    // bits) happen to equal 5, the load's rd. The HDU reads those bits as a
    // real source register, so this misfires exactly like a load-use.
    auto block = decodeAll({lw(5, 10, 0), addi(6, 0, 5)});
    EXPECT_EQ(predictStalls(block, PipelineKind::HF), 1);
}

TEST(ReorderCostModel, HF_NonLoadProducerAtDistance1_NoStall)
{
    // addi x1,x0,7 then add x2,x1,x1: real RAW dependency, but producer is
    // not a load, so forwarding covers it.
    auto block = decodeAll({addi(1, 0, 7), add(2, 1, 1)});
    EXPECT_EQ(predictStalls(block, PipelineKind::HF), 0);
}

TEST(ReorderCostModel, HF_LoadAtDistance2_NoStall)
{
    // lw x5,0(x10) / addi x9,x0,0 (filler) / add x6,x5,x5: load is 2 back,
    // forwarding covers it in HF.
    auto block = decodeAll({lw(5, 10, 0), addi(9, 0, 0), add(6, 5, 5)});
    EXPECT_EQ(predictStalls(block, PipelineKind::HF), 0);
}

TEST(ReorderCostModel, HNF_HazardAtDistance1_TwoStalls)
{
    auto block = decodeAll({addi(1, 0, 7), add(2, 1, 1)});
    EXPECT_EQ(predictStalls(block, PipelineKind::HNF), 2);
}

TEST(ReorderCostModel, HNF_HazardAtDistance2_OneStall)
{
    auto block = decodeAll({addi(1, 0, 7), addi(9, 0, 0), add(2, 1, 1)});
    EXPECT_EQ(predictStalls(block, PipelineKind::HNF), 1);
}

TEST(ReorderCostModel, HNF_DistanceOneWinsOverDistanceTwo)
{
    // A writes x1 (distance 2 from C), B writes x2 (distance 1 from C), C
    // reads both. The two-cycle hazard (distance 1) must win, not stack
    // with the one-cycle hazard (distance 2): total is 2, not 3.
    auto block = decodeAll({addi(1, 0, 7), addi(2, 0, 9), add(3, 1, 2)});
    EXPECT_EQ(predictStalls(block, PipelineKind::HNF), 2);
}

TEST(ReorderCostModel, NoHazard_NoStall)
{
    auto block = decodeAll({addi(1, 0, 7), addi(2, 0, 9), add(3, 1, 2)});
    // x4 depends on nothing here.
    auto block2 = decodeAll({addi(1, 0, 7), addi(4, 0, 9)});
    EXPECT_EQ(predictStalls(block, PipelineKind::HF), 0);
    EXPECT_EQ(predictStalls(block2, PipelineKind::HNF), 0);
}

TEST(ReorderCostModel, FirstInstructionOfBlock_NoCrash)
{
    auto block = decodeAll({add(1, 2, 3)});
    EXPECT_EQ(predictStalls(block, PipelineKind::HF), 0);
    EXPECT_EQ(predictStalls(block, PipelineKind::HNF), 0);
}

TEST(ReorderCostModel, X0Producer_NeverHazards)
{
    // addi x0, x0, 5 writes nothing real; add x1, x0, x0 reading x0 must not
    // be charged, in either mode.
    auto block = decodeAll({addi(0, 0, 5), add(1, 0, 0)});
    EXPECT_EQ(predictStalls(block, PipelineKind::HF), 0);
    EXPECT_EQ(predictStalls(block, PipelineKind::HNF), 0);
}

TEST(ReorderCostModel, HeadContext_Prev1Distance1Hazard)
{
    // Block itself has no internal hazard; the hazard crosses in from the
    // instruction right before the block (prev1).
    DecodedInst prev1 = decode(addi(1, 0, 7));
    auto block = decodeAll({add(2, 1, 1)});
    EXPECT_EQ(predictStalls(block, PipelineKind::HNF, HeadContext{&prev1, nullptr}), 2);
    EXPECT_EQ(predictStalls(block, PipelineKind::HF, HeadContext{&prev1, nullptr}), 0);
}

TEST(ReorderCostModel, HeadContext_Prev2Distance2Hazard)
{
    DecodedInst prev2 = decode(addi(1, 0, 7));
    DecodedInst prev1 = decode(addi(9, 0, 0));   // filler, pushes prev2 to distance 2
    auto block = decodeAll({add(2, 1, 1)});
    EXPECT_EQ(predictStalls(block, PipelineKind::HNF, HeadContext{&prev1, &prev2}), 1);
}

// =======================================================================
// Model validation: the 6 experiment programs, run against the real VMs.
// =======================================================================

// (1) P -> C (distance 1, real ALU producer) -> N (reads P's dest, distance 2 from N)
const char *kExp1 =
    "addi x1, x0, 7\n"
    "add  x2, x1, x1\n"
    "add  x3, x1, x1\n";

// (2) same as (1) but with one independent instruction between P and C
const char *kExp2 =
    "addi x1, x0, 7\n"
    "addi x4, x0, 99\n"
    "add  x2, x1, x1\n"
    "add  x3, x1, x1\n";

// (3) load L, C uses it (load-use, distance 1), N also uses L's dest x5.
// The setup (addi x10 -> lw) is a REAL RAW dependency, but addi x11 sits
// between them, pushing addi x10 to distance 2 from the lw -- distance 2
// costs 1 cycle in HNF, not 2 (only a distance-1 hazard costs 2). That is
// the whole reason it is 1 stall and not 2: no special case, just the same
// distance rule as everything else.
const char *kExp3 =
    "addi x10, x0, 64\n"
    "addi x11, x0, 0\n"
    "lw   x5, 0(x10)\n"
    "add  x6, x5, x5\n"
    "add  x7, x5, x5\n";

TEST(ReorderCostModel, Exp1_HF)  { assertMatchesVm<Kites::RV5StageProcessorHF>("exp1 h_f", kExp1, PipelineKind::HF); }
TEST(ReorderCostModel, Exp1_HNF) { assertMatchesVm<Kites::RV5StageProcessorHNF>("exp1 h_nf", kExp1, PipelineKind::HNF); }
TEST(ReorderCostModel, Exp2_HF)  { assertMatchesVm<Kites::RV5StageProcessorHF>("exp2 h_f", kExp2, PipelineKind::HF); }
TEST(ReorderCostModel, Exp2_HNF) { assertMatchesVm<Kites::RV5StageProcessorHNF>("exp2 h_nf", kExp2, PipelineKind::HNF); }
TEST(ReorderCostModel, Exp3_HF)  { assertMatchesVm<Kites::RV5StageProcessorHF>("exp3 h_f", kExp3, PipelineKind::HF); }
TEST(ReorderCostModel, Exp3_HNF) { assertMatchesVm<Kites::RV5StageProcessorHNF>("exp3 h_nf", kExp3, PipelineKind::HNF); }

// =======================================================================
// Model validation: the 4 quirk programs from reorder_quirk_test.cpp.
// =======================================================================

std::string quirkSource(int imm)
{
    return "addi x10, x0, 64\n"
           "lw   x5, 0(x10)\n"
           "addi x6, x0, " + std::to_string(imm) + "\n";
}

TEST(ReorderCostModel, Quirk_Imm5_HF)  { assertMatchesVm<Kites::RV5StageProcessorHF>("quirk imm5 h_f", quirkSource(5), PipelineKind::HF); }
TEST(ReorderCostModel, Quirk_Imm6_HF)  { assertMatchesVm<Kites::RV5StageProcessorHF>("quirk imm6 h_f", quirkSource(6), PipelineKind::HF); }
TEST(ReorderCostModel, Quirk_Imm5_HNF) { assertMatchesVm<Kites::RV5StageProcessorHNF>("quirk imm5 h_nf", quirkSource(5), PipelineKind::HNF); }
TEST(ReorderCostModel, Quirk_Imm6_HNF) { assertMatchesVm<Kites::RV5StageProcessorHNF>("quirk imm6 h_nf", quirkSource(6), PipelineKind::HNF); }

// =======================================================================
// Model validation: the straight-line baseline hazard examples from
// examples/*.s (R_B_hazard.s and L_B_hazard.s are excluded -- they branch,
// so they are not single straight-line blocks).
// =======================================================================

const char *kLU =
    "addi x1, x0, 100\n"
    "addi x2, x0, 20\n"
    "sw   x2, 0(x1)\n"
    "lw   x3, 0(x1)\n"
    "add  x6, x3, x1\n";

const char *kRM =
    "addi x3, x0, 55\n"
    "addi x4, x0, 400\n"
    "sw   x3, 0(x4)\n"
    "addi x1, x0, 200\n"
    "add  x2, x1, x1\n"
    "lw   x9, 0(x2)\n";

const char *kLS =
    "addi x1, x0, 100\n"
    "addi x2, x0, 7\n"
    "sw   x2, 0(x1)\n"
    "lw   x3, 0(x1)\n"
    "addi x3, x0, 300\n"
    "sw   x2, 0(x3)\n"
    "lw   x7, 0(x3)\n";

const char *kRS =
    "addi x1, x0, 10\n"
    "add  x2, x1, x1\n"
    "addi x3, x0, 200\n"
    "sw   x2, 0(x3)\n"
    "lw   x8, 0(x3)\n";

const char *kRR =
    "addi x1, x0, 5\n"
    "add  x2, x1, x1\n"
    "sub  x6, x2, x1\n";

TEST(ReorderCostModel, Baseline_LU_HF)  { assertMatchesVm<Kites::RV5StageProcessorHF>("L_U h_f", kLU, PipelineKind::HF); }
TEST(ReorderCostModel, Baseline_LU_HNF) { assertMatchesVm<Kites::RV5StageProcessorHNF>("L_U h_nf", kLU, PipelineKind::HNF); }
TEST(ReorderCostModel, Baseline_RM_HF)  { assertMatchesVm<Kites::RV5StageProcessorHF>("R_M h_f", kRM, PipelineKind::HF); }
TEST(ReorderCostModel, Baseline_RM_HNF) { assertMatchesVm<Kites::RV5StageProcessorHNF>("R_M h_nf", kRM, PipelineKind::HNF); }
TEST(ReorderCostModel, Baseline_LS_HF)  { assertMatchesVm<Kites::RV5StageProcessorHF>("L_S h_f", kLS, PipelineKind::HF); }
TEST(ReorderCostModel, Baseline_LS_HNF) { assertMatchesVm<Kites::RV5StageProcessorHNF>("L_S h_nf", kLS, PipelineKind::HNF); }
TEST(ReorderCostModel, Baseline_RS_HF)  { assertMatchesVm<Kites::RV5StageProcessorHF>("R_S h_f", kRS, PipelineKind::HF); }
TEST(ReorderCostModel, Baseline_RS_HNF) { assertMatchesVm<Kites::RV5StageProcessorHNF>("R_S h_nf", kRS, PipelineKind::HNF); }
TEST(ReorderCostModel, Baseline_RR_HF)  { assertMatchesVm<Kites::RV5StageProcessorHF>("R_R h_f", kRR, PipelineKind::HF); }
TEST(ReorderCostModel, Baseline_RR_HNF) { assertMatchesVm<Kites::RV5StageProcessorHNF>("R_R h_nf", kRR, PipelineKind::HNF); }

// =======================================================================
// x0-producer: writing x0 must never cause a hazard, against the real VM.
// =======================================================================

const char *kX0Producer =
    "addi x0, x0, 5\n"
    "add  x1, x0, x0\n";

TEST(ReorderCostModel, X0Producer_VM_HF)  { assertMatchesVm<Kites::RV5StageProcessorHF>("x0 producer h_f", kX0Producer, PipelineKind::HF); }
TEST(ReorderCostModel, X0Producer_VM_HNF) { assertMatchesVm<Kites::RV5StageProcessorHNF>("x0 producer h_nf", kX0Producer, PipelineKind::HNF); }

// =======================================================================
// Randomized validation: 200 straight-line programs per mode, random ALU,
// M-extension, load and store instructions, with immediates deliberately
// made to collide (in their low 5 bits) with live registers so the HDU
// quirk gets exercised, not just real dependencies.
// =======================================================================

namespace
{

// A small fixed set of base registers is set up once, at the very start,
// and never written again afterward -- so every load/store offset off them
// stays valid for the whole program, no matter what the random body does.
std::string makeRandomProgram(std::mt19937 &rng, int bodyLen)
{
    // Addresses must stay well clear of the program's own instruction
    // footprint (a handful of setup instructions + up to 25 random ones,
    // comfortably under 150 bytes) -- otherwise a generated store can
    // silently overwrite a later instruction's encoded word in memory
    // (self-modifying code), which has nothing to do with the hazard model
    // but still changes what the VM actually executes.
    static const int basesReg[] = {10, 11, 12};
    static const int baseAddr[] = {1000, 1200, 1400};
    static const int pool[] = {5, 6, 7, 8, 9, 13, 14, 15, 16, 17, 18, 19, 20};
    static const int offsets[] = {0, 4, 8, 12, 16};
    static const char *rtypeOps[] = {"add", "sub", "and", "or", "xor", "sll",
                                     "srl", "sra", "mul", "div", "rem"};

    std::uniform_int_distribution<int> pickPool(0, static_cast<int>(std::size(pool)) - 1);
    std::uniform_int_distribution<int> pickBase(0, static_cast<int>(std::size(basesReg)) - 1);
    std::uniform_int_distribution<int> pickOffset(0, static_cast<int>(std::size(offsets)) - 1);
    std::uniform_int_distribution<int> pickOp(0, 13);   // 0=addi, 1..11=rtype, 12=lw, 13=sw
    std::uniform_int_distribution<int> coinFlip(0, 1);
    std::uniform_int_distribution<int> randImm(0, 2047);

    std::ostringstream out;
    for (int i : {0, 1, 2})
    {
        out << "addi x" << basesReg[i] << ", x0, " << baseAddr[i] << "\n";
    }

    for (int i = 0; i < bodyLen; ++i)
    {
        int op = pickOp(rng);
        if (op == 0)
        {
            int rd = pool[pickPool(rng)];
            int rs1 = pool[pickPool(rng)];
            int imm = randImm(rng);
            if (coinFlip(rng))
            {
                // Force the low 5 bits (the HDU's raw "rs2" field) to equal
                // a live register number -- the quirk-collision case.
                int collideWith = pool[pickPool(rng)];
                imm = (imm & ~0x1F) | collideWith;
            }
            out << "addi x" << rd << ", x" << rs1 << ", " << imm << "\n";
        }
        else if (op == 12)
        {
            int rd = pool[pickPool(rng)];
            int base = basesReg[pickBase(rng)];
            int off = offsets[pickOffset(rng)];
            out << "lw x" << rd << ", " << off << "(x" << base << ")\n";
        }
        else if (op == 13)
        {
            int rs2 = pool[pickPool(rng)];
            int base = basesReg[pickBase(rng)];
            int off = offsets[pickOffset(rng)];
            out << "sw x" << rs2 << ", " << off << "(x" << base << ")\n";
        }
        else
        {
            int rd = pool[pickPool(rng)];
            int rs1 = pool[pickPool(rng)];
            int rs2 = pool[pickPool(rng)];
            out << rtypeOps[op - 1] << " x" << rd << ", x" << rs1 << ", x" << rs2 << "\n";
        }
    }
    return out.str();
}

template <typename VmType>
void runRandomizedValidation(PipelineKind kind, unsigned seed)
{
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> lenDist(10, 25);

    for (int trial = 0; trial < 200; ++trial)
    {
        std::string source = makeRandomProgram(rng, lenDist(rng));

        Kites::setupVmStateDirectory();
        auto vm = std::make_unique<VmType>();
        std::istringstream stream(source);
        Kites::AssembledProgram program = Kites::assemble(stream);
        vm->LoadProgram(program);
        vm->DebugRun();

        int predicted = predictStalls(program.text_buffer, kind);
        if (static_cast<int>(vm->total_stalls_) != predicted)
        {
            ADD_FAILURE() << "Mismatch on randomized trial " << trial
                          << ": VM stalls=" << vm->total_stalls_ << " predicted=" << predicted
                          << "\nprogram:\n"
                          << source;
            break;
        }
    }
}

} // namespace

TEST(ReorderCostModel, RandomizedValidation_HF)
{
    runRandomizedValidation<Kites::RV5StageProcessorHF>(PipelineKind::HF, 12345u);
}

TEST(ReorderCostModel, RandomizedValidation_HNF)
{
    runRandomizedValidation<Kites::RV5StageProcessorHNF>(PipelineKind::HNF, 67890u);
}
