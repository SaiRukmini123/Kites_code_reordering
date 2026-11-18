#!/usr/bin/env python3
"""Single source of truth for the hazard benchmark programs.

Generates (stdlib only, no network):
  bench/hazard/<name>.s           - programs, for kites-cli
  bench/hazard/expected.csv       - hand-derived stall counts per VM
  test/reorder_hazard_cases.h     - case table + seed, shared by reorder_hazard_programs_test.cpp
                                    and reorder_stage2_pipeline_test.cpp
  bench/programs/reference/<name>.s - the same programs in benchmark format (header with category
                                    and inputs, exit ecall at the end) for `kites-cli reorder-bench`

Run from the repo root:  python3 bench/hazard/gen_hazard.py

Cost model (taken from src/processor/rv5s/rv5s_hdu.cpp, NOT from textbook rules):
  * The HDU reads rs1 = bits 19:15 and rs2 = bits 24:20 of EVERY instruction, whether or not the
    instruction really has those operands (I-type immediates, LUI/JAL immediates, ...).
    So an immediate whose low 5 bits name a register being produced causes a false stall.
    Programs below only use immediates whose low 5 bits are 0 (32, 64) or offsets 0/4, so the
    hand-predictions are clean. The model below uses the raw fields, so it would catch a slip.
  * h_nf (no forwarding): producer 1 ahead -> 2 stalls, 2 ahead -> 1 stall, 3+ ahead -> 0.
  * h_f (forwarding): only a LOAD directly before its consumer stalls, 1 cycle.
  * Taken-branch bubbles are NOT in 'stalls'. Straight-line cycles = retired + 4 + stalls.
"""
import os, re, sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
DATA = 0x10000000

def reg(s):
    m = re.fullmatch(r"x(\d+)", s.strip()); assert m, s
    return int(m.group(1))

def encode(line, pc, labels):
    op, _, rest = line.partition(" ")
    a = [t.strip() for t in rest.split(",")]
    if op in ("add", "sub"):
        f7 = 0x20 if op == "sub" else 0
        return f7 << 25 | reg(a[2]) << 20 | reg(a[1]) << 15 | 0 << 12 | reg(a[0]) << 7 | 0x33
    if op == "addi":
        return (int(a[2]) & 0xFFF) << 20 | reg(a[1]) << 15 | reg(a[0]) << 7 | 0x13
    if op == "lw":
        m = re.fullmatch(r"(-?\d+)\((x\d+)\)", a[1])
        return (int(m.group(1)) & 0xFFF) << 20 | reg(m.group(2)) << 15 | 2 << 12 | reg(a[0]) << 7 | 0x03
    if op == "sw":
        m = re.fullmatch(r"(-?\d+)\((x\d+)\)", a[1]); imm = int(m.group(1)) & 0xFFF
        return (imm >> 5) << 25 | reg(a[0]) << 20 | reg(m.group(2)) << 15 | 2 << 12 | (imm & 31) << 7 | 0x23
    if op == "beq":
        off = labels[a[2]] - pc; i = off & 0x1FFF
        return (i >> 12 & 1) << 31 | (i >> 5 & 63) << 25 | reg(a[1]) << 20 | reg(a[0]) << 15 | (i >> 1 & 15) << 8 | (i >> 11 & 1) << 7 | 0x63
    raise ValueError(line)

def assemble(src):
    lines, labels, pc = [], {}, 0
    for raw in src:
        if raw.endswith(":"): labels[raw[:-1]] = pc; continue
        lines.append((raw, pc)); pc += 4
    return [encode(l, p, labels) for l, p in lines]

def writes_gpr(w):
    return (w & 0x7F) in (0x33, 0x13, 0x03, 0x37, 0x17, 0x6F, 0x67) and (w >> 7 & 31) != 0

def model_stalls(words, forwarding):
    t, total = [], 0
    for i, w in enumerate(words):
        rs1, rs2 = w >> 15 & 31, w >> 20 & 31          # raw fields, like the HDU
        ti = (t[i - 1] + 1) if i else 0
        for dist in (1, 2):
            j = i - dist
            if j < 0 or not writes_gpr(words[j]): continue
            rd = words[j] >> 7 & 31
            if rd not in (rs1, rs2): continue
            if forwarding:
                if dist == 1 and (words[j] & 0x7F) == 0x03: ti = max(ti, t[j] + 2)
            else:
                ti = max(ti, t[j] + 3)
        if i: total += ti - t[i - 1] - 1
        t.append(ti)
    return total

# name -> (why, instructions, expected (h_f, h_nf) stalls derived BY HAND, memory words seeded at DATA)
# x10 is seeded with DATA by the test (and by the comment header for kites-cli), so no la/li is needed.
P = {
 "load_use":        ("lw then immediate use",                  ["lw x5, 0(x10)", "add x6, x5, x5", "addi x7, x0, 32"], (1, 2)),
 "load_gap":        ("same as load_use, independent instr moved into the gap", ["lw x5, 0(x10)", "addi x7, x0, 32", "add x6, x5, x5"], (0, 1)),
 "raw_adjacent":    ("ALU result used by next instr",          ["addi x5, x0, 64", "add x6, x5, x5"], (0, 2)),
 "raw_dist2":       ("ALU result used 2 later",                ["addi x5, x0, 64", "addi x7, x0, 32", "add x6, x5, x5"], (0, 1)),
 "raw_dist3":       ("ALU result used 3 later: no hazard",     ["addi x5, x0, 64", "addi x7, x0, 32", "addi x8, x0, 32", "add x6, x5, x5"], (0, 0)),
 "dep_chain":       ("fully dependent chain, nothing to move", ["addi x5, x0, 64", "add x6, x5, x5", "add x7, x6, x6", "add x8, x7, x7"], (0, 6)),
 "two_loads_use":   ("second load feeds the use",              ["lw x5, 0(x10)", "lw x6, 4(x10)", "add x7, x5, x6"], (1, 2)),
 "load_branch":     ("branch right after its load (not taken, word0=7)", ["lw x5, 0(x10)", "beq x5, x0, done", "addi x6, x0, 32", "done:", "addi x7, x0, 64"], (1, 2)),
 "load_store":      ("loaded value stored next cycle",         ["lw x5, 0(x10)", "sw x5, 4(x10)", "addi x7, x0, 32"], (1, 2)),
 "independent":     ("no dependences at all",                  ["addi x5, x0, 32", "addi x6, x0, 64", "addi x7, x0, 32", "addi x8, x0, 64"], (0, 0)),
 # Programs with room to move: independent chains written back to back, so a scheduler can
 # interleave them. x4 is never written, because `lw/sw ..., 4(x10)` puts 4 in the raw rs2 field.
 # two_alu_chains, h_nf: every consumer directly follows its producer -> 4 x 2 = 8. h_f: no loads -> 0.
 "two_alu_chains":  ("two independent 3-long ALU chains, written one after the other",
                     ["addi x5, x0, 64", "add x6, x5, x5", "add x7, x6, x6",
                      "addi x11, x0, 32", "add x12, x11, x11", "add x13, x12, x12"], (0, 8)),
 # two_load_use_pairs: h_f: two load-use pairs -> 2 x 1 = 2. h_nf: same two pairs -> 2 x 2 = 4.
 "two_load_use_pairs": ("two load-use pairs plus two independent addi at the end",
                     ["lw x5, 0(x10)", "add x6, x5, x5", "lw x7, 4(x10)", "add x8, x7, x7",
                      "addi x11, x0, 32", "addi x12, x0, 64"], (2, 4)),
 # load_store_alu_mix: h_f: only lw -> addi stalls -> 1. h_nf: lw->addi, addi->sw, addi x11->add,
 # addi x13->add, add x14->add x15 are all distance 1 -> 5 x 2 = 10.
 "load_store_alu_mix": ("load -> addi -> store chain, then two ALU chains that join",
                     ["lw x5, 0(x10)", "addi x6, x5, 32", "sw x6, 4(x10)",
                      "addi x11, x0, 64", "add x12, x11, x11", "addi x13, x0, 32", "add x14, x13, x13",
                      "add x15, x12, x14"], (1, 10)),
}

# Benchmark category of each program (hazard | loop | aliasing | chain | mixed).
CATEGORY = {
    "load_use": "hazard", "load_gap": "hazard", "raw_adjacent": "chain", "raw_dist2": "hazard",
    "raw_dist3": "hazard", "dep_chain": "chain", "two_loads_use": "hazard", "load_branch": "hazard",
    "load_store": "hazard", "independent": "hazard", "two_alu_chains": "mixed",
    "two_load_use_pairs": "hazard", "load_store_alu_mix": "mixed",
}
SEED = f"x10=0x{DATA:X},mem:0x{DATA:X}=7,mem:0x{DATA + 4:X}=0"

def write_reference(name, why, src):
    out = os.path.join(ROOT, "bench", "programs", "reference"); os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, name + ".s"), "w") as f:
        f.write(f"# name: ref_{name}\n# category: {CATEGORY[name]}\n# source: reference\n"
                f"# inputs: {SEED}\n# description: {why}\n"
                f"# GENERATED by bench/hazard/gen_hazard.py (same body as the hazard test table, plus the\n"
                f"# exit ecall the benchmark filter requires).\n")
        f.write("\n".join(src) + "\naddi x17, x0, 10\necall\n")

def main():
    out = os.path.join(ROOT, "bench", "hazard"); os.makedirs(out, exist_ok=True)
    rows, cases = ["name,instructions,hf_stalls,hnf_stalls,why"], []
    for name, (why, src, (ehf, ehnf)) in P.items():
        words = assemble(src)
        mhf, mhnf = model_stalls(words, True), model_stalls(words, False)
        assert (mhf, mhnf) == (ehf, ehnf), f"{name}: model {(mhf, mhnf)} != hand {(ehf, ehnf)}"
        with open(os.path.join(out, name + ".s"), "w") as f:
            f.write(f"# {why}\n# expected stalls: rv5-h-f={ehf} rv5-h-nf={ehnf}\n"
                    f"# Reference listing. Real runs are done by test/reorder_hazard_programs_test.cpp, which seeds\n"
                    f"# x10 = 0x{DATA:X} and mem[0x{DATA:X}] = 7 (so the programs need no la/li).\n")
            f.write("\n".join(src) + "\n")
        rows.append(f'{name},{len(words)},{ehf},{ehnf},"{why}"')
        write_reference(name, why, src)
        body = "\\n".join(src) + "\\n"
        cases.append(f'    {{"{name}", "{body}", {len(words)}, {ehf}, {ehnf}}},')
    open(os.path.join(out, "expected.csv"), "w").write("\n".join(rows) + "\n")
    tmpl = open(os.path.join(os.path.dirname(__file__), "cases_template.h.in")).read()
    open(os.path.join(ROOT, "test", "reorder_hazard_cases.h"), "w").write(tmpl.replace("@CASES@", "\n".join(cases)))
    print("ok:", len(P), "programs; model == hand predictions for all")

main()
