#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "assembler/assembler.h"
#include "config/config.h"
#include "processor/rvss/rvss_processor.h"
#include "reorder/block_finder.h"
#include "reorder/cost_model.h"
#include "reorder/decoded_inst.h"
#include "reorder/program_runner.h"

namespace Kites::reorder
{

// Path replay: the exact cycle count and stall count of a program on rv5-h-f
// and rv5-h-nf, predicted from the path the program really executes.
//
// tracePath() runs the program once on the single-cycle VM and records every
// instruction it executes. replayPath() then walks that path through the
// hazard check of cost_model.h (checkHazard(), with the same window shifting
// as issue()), using the words of any order that keeps every block in place.
// Reordering inside blocks never moves a branch or a block boundary, so the
// reordered program executes the same blocks in the same order with the same
// branch outcomes, and one trace serves the original and every reordering.
//
// Control rules, read from rv5s_processor_base.cpp and
// rv5s_processor_h_f.cpp / rv5s_processor_h_nf.cpp and checked on both VMs in
// test/reorder_path_replay_test.cpp:
//  - Falling into the next instruction (a branch that is not taken, or a block
//    that simply ends) keeps the two-slot hazard window, so a hazard can cross
//    a block boundary.
//  - A taken conditional branch resolves in MEM: 3 flush cycles, and the
//    target starts with an empty window. The instruction fetched right after
//    the branch is still hazard-checked in the cycle the branch executes. If it
//    stalls, total_stalls_ counts that stall, but the flush discards the
//    instruction in the next cycle, so the stall costs no cycle.
//  - A jump (jal, jalr) resolves in EX: 2 flush cycles and an empty window.
//    The instruction after it is discarded before the hazard check, so no
//    stall is counted for it.
// So, for a run that ends normally:
//     cycles        = retired + 4 + stallCycles + flushCycles
//     total_stalls_ = stallCycles + discardedStalls
//
// Limits: rv5-h-f and rv5-h-nf ignore ecall and run to the end of the text,
// while the single-cycle VM stops at the exit ecall. The replay therefore
// matches the pipelines only when the exit ecall is the last instruction of
// the text (the bench filter requires this) or the program runs off the end.
// Programs that read input are not supported.

// One run of a program on the single-cycle VM.
struct ExecutedPath
{
    std::vector<uint32_t> indices;   // instruction index (pc / 4 from the text start) of each executed instruction
    std::vector<bool> taken;         // per entry: a conditional branch whose condition held
    bool finished = false;           // stopped at the exit ecall or ran off the end of the text
    bool endedAtExitEcall = false;   // stopped at the exit ecall, which is the last entry of indices
};

// Does the conditional branch `word` jump, given its two source values?
// Evaluated from the registers rather than from the next PC, because a taken
// branch to the next instruction still flushes the pipeline.
inline bool branchConditionHolds(uint32_t word, uint64_t a, uint64_t b)
{
    switch ((word >> 12) & 0x7)
    {
    case 0b000:
        return a == b;   // beq
    case 0b001:
        return a != b;   // bne
    case 0b100:
        return static_cast<int64_t>(a) < static_cast<int64_t>(b);    // blt
    case 0b101:
        return static_cast<int64_t>(a) >= static_cast<int64_t>(b);   // bge
    case 0b110:
        return a < b;    // bltu
    case 0b111:
        return a >= b;   // bgeu
    default:
        return false;
    }
}

// Runs `program` from `seed` on the single-cycle VM, one instruction at a
// time, and records the path. It stops just BEFORE executing the exit ecall
// (an ecall with a7 == 10) and records that ecall as executed: the
// single-cycle VM's exit syscall calls exit(0), which would end the whole
// calling process (a test binary would then report success). Gives up after
// maxSteps instructions (finished stays false).
inline ExecutedPath tracePath(const AssembledProgram &program, const Seed &seed = {},
                              uint64_t maxSteps = 1000000)
{
    reorder_detail::CoutSilencer quiet;   // the VM prints a line every step
    auto vm = std::make_unique<RVSSProcessor>();
    vm->LoadProgram(program);
    for (const auto &[reg, value] : seed.regs)
    {
        if (reg > 0)
        {
            vm->registers_.WriteGpr(reg, value);
        }
    }
    for (const auto &[address, value] : seed.memWords)
    {
        vm->memory_controller_.writeWord_d(address, value);
    }

    const std::vector<uint32_t> &words = program.text_buffer;
    const uint64_t textStart = vm_config::config.getTextSectionStart();
    ExecutedPath path;
    while (path.indices.size() < maxSteps)
    {
        const uint64_t pc = vm->program_counter_;
        if (pc < textStart || (pc - textStart) / 4 >= words.size())
        {
            path.finished = true;   // ran off the end of the text
            break;
        }
        const size_t index = static_cast<size_t>((pc - textStart) / 4);
        const uint32_t word = words[index];
        const DecodedInst d = decode(word);
        path.indices.push_back(static_cast<uint32_t>(index));
        path.taken.push_back(d.is_branch && branchConditionHolds(word, vm->registers_.ReadGpr(d.rs1),
                                                                 vm->registers_.ReadGpr(d.rs2)));
        // RVSSProcessor::Execute() sends every opcode-0x73, funct3-0 word to
        // HandleSyscall(), which calls exit(0) when a7 == 10.
        const bool syscall = (word & 0x7F) == 0x73 && ((word >> 12) & 0x7) == 0;
        if (syscall && vm->registers_.ReadGpr(17) == 10)
        {
            path.finished = true;
            path.endedAtExitEcall = true;
            break;
        }
        vm->Step();
    }
    return path;
}

// One hazard the HDU saw on the replayed path.
struct StallRecord
{
    uint64_t consumerPc = 0;               // the instruction that stalled
    uint64_t producerPc = 0;               // the instruction whose register it waited for
    int cycles = 0;                        // stall cycles this hazard cost (1 or 2)
    bool producerInEarlierBlock = false;   // the producer issued before the consumer's block was entered
    bool trueRead = false;                 // the matching rs1/rs2 field is a register the consumer really
                                           // reads; false: the field is immediate bits (the HDU quirk)
    bool discarded = false;                // the consumer was fetched after a taken branch and flushed,
                                           // so this stall is counted by the VM but costs no cycle
};

struct ReplayResult
{
    uint64_t retired = 0;           // instructions on the path
    uint64_t stallCycles = 0;       // stalls of instructions that retire (each costs a cycle)
    uint64_t discardedStalls = 0;   // stalls the VM counts on discarded instructions (cost nothing)
    uint64_t flushCycles = 0;       // 3 per taken branch + 2 per jump
    uint64_t takenBranches = 0;
    uint64_t jumps = 0;
    uint64_t predictedCycles = 0;   // retired + 4 + stallCycles + flushCycles
    std::vector<StallRecord> stalls;

    // What the VM's total_stalls_ reports.
    uint64_t countedStalls() const { return stallCycles + discardedStalls; }
};

namespace replay_detail
{
// A window slot: the producer as the HDU sees it, plus where it came from.
struct Slot
{
    Producer producer{};
    uint64_t pc = 0;
    uint64_t blockEntry = 0;   // which block execution issued it (0 = a bubble)
};

// One instruction through the HDU: the same loop as issue() in cost_model.h
// (raw rs1/rs2 fields, a bubble shifted in per stall cycle, then re-checked),
// but it also records which producer each hazard came from.
inline int issueTracked(Slot &older, Slot &newer, const DecodedInst &consumer, uint64_t consumerPc,
                        uint64_t blockEntry, PipelineKind kind, bool discarded, std::vector<StallRecord> &records)
{
    const uint8_t rs1 = consumer.rs1;
    const uint8_t rs2 = consumer.rs2;
    Slot prev = older;
    Slot cur = newer;
    int stalls = 0;
    while (true)
    {
        const int hazard = checkHazard(cur.producer, prev.producer, rs1, rs2, kind);
        if (hazard == 0)
        {
            older = cur;
            newer = Slot{asProducer(consumer), consumerPc, blockEntry};
            return stalls;
        }
        // checkHazard() looks at the newer slot first (HF: only the newer slot).
        const Slot &from = checkHazard(cur.producer, Producer{}, rs1, rs2, kind) > 0 ? cur : prev;
        StallRecord r;
        r.consumerPc = consumerPc;
        r.producerPc = from.pc;
        r.cycles = hazard;
        r.producerInEarlierBlock = from.blockEntry != blockEntry;
        r.trueRead = (consumer.reads_rs1 && rs1 == from.producer.rd) || (consumer.reads_rs2 && rs2 == from.producer.rd);
        r.discarded = discarded;
        records.push_back(r);
        stalls += hazard;
        for (int cycle = 0; cycle < hazard; ++cycle)
        {
            prev = cur;
            cur = Slot{};
        }
    }
}
} // namespace replay_detail

// Replays `path` (traced on the original program) with the instruction words
// `words`, which must keep every block of `blocks` in place: the original
// words, or any reordering inside blocks.
inline ReplayResult replayPath(const std::vector<uint32_t> &words, const std::vector<Block> &blocks,
                               const ExecutedPath &path, PipelineKind kind)
{
    std::vector<DecodedInst> insts;
    insts.reserve(words.size());
    for (uint32_t w : words)
    {
        insts.push_back(decode(w));
    }
    std::vector<bool> blockStart(words.size(), false);
    for (const Block &b : blocks)
    {
        if (b.begin < blockStart.size())
        {
            blockStart[b.begin] = true;
        }
    }
    const uint64_t textStart = vm_config::config.getTextSectionStart();

    ReplayResult r;
    replay_detail::Slot older;   // empty window: the pipeline starts empty
    replay_detail::Slot newer;
    uint64_t entry = 0;          // block executions so far
    for (size_t t = 0; t < path.indices.size(); ++t)
    {
        const size_t index = path.indices[t];
        if (t > 0)
        {
            const size_t prev = path.indices[t - 1];
            if (insts[prev].is_jump)
            {
                ++r.jumps;
                r.flushCycles += 2;
                older = newer = replay_detail::Slot{};
            }
            else if (insts[prev].is_branch && path.taken[t - 1])
            {
                ++r.takenBranches;
                r.flushCycles += 3;
                if (prev + 1 < insts.size())   // the instruction fetched behind the branch
                {
                    replay_detail::Slot o = older;
                    replay_detail::Slot n = newer;
                    r.discardedStalls += replay_detail::issueTracked(o, n, insts[prev + 1], textStart + 4 * (prev + 1),
                                                                     entry + 1, kind, true, r.stalls);
                }
                older = newer = replay_detail::Slot{};
            }
        }
        if (blockStart[index])
        {
            ++entry;
        }
        r.stallCycles += replay_detail::issueTracked(older, newer, insts[index], textStart + 4 * index, entry, kind,
                                                     false, r.stalls);
    }
    r.retired = path.indices.size();
    r.predictedCycles = r.retired + 4 + r.stallCycles + r.flushCycles;
    return r;
}

// The same, with the blocks the scheduler uses for `program` (every code
// label is a leader).
inline ReplayResult replayPath(const AssembledProgram &program, const std::vector<uint32_t> &words,
                               const ExecutedPath &path, PipelineKind kind)
{
    return replayPath(words, findBlocks(program.text_buffer, labelLeaders(program)), path, kind);
}

} // namespace Kites::reorder
