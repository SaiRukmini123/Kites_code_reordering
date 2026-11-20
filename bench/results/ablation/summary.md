# Ablation ladder

Each step adds one thing to the step before. 54 valid benchmarks (the reorder-bench programs, validity filter and inputs), cycle cap 200000. Measured cycles come from the VM; predictions from the path replay (src/reorder/path_replay.h) along each program's executed path.

| step | adds |
|---|---|
| C0 | the original program |
| C1 | Greedy scheduler, Textbook hazard model, ArrayOrder contexts, auipc as a barrier |
| C2 | Exact hazard model (the HDU quirk) |
| C3 | Optimal scheduler |
| C4 | auipc pinned instead of a barrier |
| C5 | block-entry contexts (loop weight 10) |
| C6 | VM safety net (applyWithVmCheck) |
| C7 | loop unrolling (unrollAndReorder) |

Reductions are per program, against C0. Data-stall cycles are the replay's stall cycles that cost a cycle (the VM's total_stalls_ also counts stalls of flushed instructions; see ladder.csv). MAPE is the mean absolute % error of the replay's predicted cycles against the measured cycles; Textbook predictions are made for C0 and C6 only.

## rv5-h-f

| step | cycles | data-stall cycles | flush cycles | CPI | faster / same / slower than C0 | mean reduction | median reduction | best program (reduction) | state mismatches | MAPE Exact | MAPE Textbook |
|---|---|---|---|---|---|---|---|---|---|---|---|
| C0 original | 5465 | 266 | 1398 | 1.524 | 0 / 54 / 0 | 0.0% | 0.0% | - | none | 0.00% | 0.00% |
| C1 greedy+textbook | 5315 | 116 | 1398 | 1.483 | 28 / 26 / 0 | 5.2% | 2.6% | hazard_byte_loads (19.0%, 21 -> 17) | none | 0.00% | - |
| C2 +exact | 5315 | 116 | 1398 | 1.483 | 28 / 26 / 0 | 5.2% | 2.6% | hazard_byte_loads (19.0%, 21 -> 17) | none | 0.00% | - |
| C3 +optimal | 5315 | 116 | 1398 | 1.483 | 28 / 26 / 0 | 5.2% | 2.6% | hazard_byte_loads (19.0%, 21 -> 17) | none | 0.00% | - |
| C4 +auipc_pinning | 5314 | 115 | 1398 | 1.482 | 28 / 26 / 0 | 5.3% | 2.6% | hazard_byte_loads (19.0%, 21 -> 17) | none | 0.00% | - |
| C5 +block_entries | 5313 | 114 | 1398 | 1.482 | 29 / 25 / 0 | 5.4% | 3.7% | hazard_byte_loads (19.0%, 21 -> 17) | none | 0.00% | - |
| C6 +safety_net | 5313 | 114 | 1398 | 1.482 | 29 / 25 / 0 | 5.4% | 3.7% | hazard_byte_loads (19.0%, 21 -> 17) | none | 0.00% | 0.00% |
| C7 +unrolling | 5064 | 114 | 1149 | 1.413 | 33 / 21 / 0 | 9.1% | 9.1% | loop_array_sum (33.8%, 154 -> 102) | none | 0.00% | - |

Over all steps: MAPE Exact 0.00% (432 program runs); MAPE Textbook 0.00% (108 runs, C0 and C6).

Exact predictions equal the measured cycles in 432 of 432 runs. Textbook predictions equal them in 108 of 108; misses: none.

Slower than C0: none.

## rv5-h-nf

| step | cycles | data-stall cycles | flush cycles | CPI | faster / same / slower than C0 | mean reduction | median reduction | best program (reduction) | state mismatches | MAPE Exact | MAPE Textbook |
|---|---|---|---|---|---|---|---|---|---|---|---|
| C0 original | 8249 | 3050 | 1398 | 2.301 | 0 / 54 / 0 | 0.0% | 0.0% | - | none | 0.00% | 0.00% |
| C1 greedy+textbook | 7187 | 1988 | 1398 | 2.005 | 47 / 6 / 1 | 13.5% | 10.0% | ref_load_store_alu_mix (41.7%, 24 -> 14) | none | 0.00% | - |
| C2 +exact | 7179 | 1980 | 1398 | 2.003 | 47 / 6 / 1 | 13.9% | 11.4% | ref_load_store_alu_mix (41.7%, 24 -> 14) | none | 0.00% | - |
| C3 +optimal | 7108 | 1909 | 1398 | 1.983 | 48 / 6 / 0 | 14.5% | 12.5% | ref_load_store_alu_mix (41.7%, 24 -> 14) | none | 0.00% | - |
| C4 +auipc_pinning | 7090 | 1891 | 1398 | 1.978 | 48 / 6 / 0 | 14.7% | 12.7% | ref_load_store_alu_mix (41.7%, 24 -> 14) | none | 0.00% | - |
| C5 +block_entries | 7016 | 1817 | 1398 | 1.957 | 51 / 3 / 0 | 15.5% | 13.3% | ref_load_store_alu_mix (41.7%, 24 -> 14) | none | 0.00% | - |
| C6 +safety_net | 7016 | 1817 | 1398 | 1.957 | 51 / 3 / 0 | 15.5% | 13.3% | ref_load_store_alu_mix (41.7%, 24 -> 14) | none | 0.00% | 0.02% |
| C7 +unrolling | 6791 | 1841 | 1149 | 1.894 | 51 / 3 / 0 | 17.7% | 15.3% | loop_array_sum (48.6%, 208 -> 107) | none | 0.00% | - |

Over all steps: MAPE Exact 0.00% (432 program runs); MAPE Textbook 0.01% (108 runs, C0 and C6).

Exact predictions equal the measured cycles in 432 of 432 runs. Textbook predictions equal them in 107 of 108; misses: loop_prefix_sum at C6 (predicted 118, measured 119).

Slower than C0: loop_bubble_sort at C1 (491 -> 497); loop_bubble_sort at C2 (491 -> 497).
