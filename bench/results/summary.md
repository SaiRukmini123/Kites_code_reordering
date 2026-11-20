# Reordering benchmark results

## Validity filter

- programs found: 55
- valid: 54
- near-duplicates removed: 0
- usable: 54
- rejection reasons (a program can have several):
  - label named 'data' (a section directive): 1

## rv5-h-f

Scheduled with ScheduleMode::Optimal (block-entry scoring). "total cycles" is the raw reordered program; the safety-net line is what is kept.

- programs tried: 54, improved: 29, unchanged: 25, slower: 0, not equivalent: 0
- total cycles: 5465 -> 5313 (2.78% fewer)
- after the VM safety net (applyWithVmCheck keeps a block's new order only if the program stays equivalent and gets no slower): 5465 -> 5313 (2.78% fewer); programs improved: 29, fell back to the original: 0, some blocks rejected: 0
- overall CPI: 1.524 -> 1.482
- mean cycle reduction per program: 5.36% (over all tried), 9.99% (over improved only)
- max cycle reduction: 19.05% (hazard_byte_loads)

| category | tried | improved | cycles before | cycles after | reduction |
|---|---|---|---|---|---|
| aliasing | 8 | 8 | 349 | 307 | 12.03% |
| chain | 10 | 2 | 1859 | 1853 | 0.32% |
| hazard | 17 | 10 | 305 | 283 | 7.21% |
| loop | 10 | 6 | 1895 | 1821 | 3.91% |
| mixed | 9 | 3 | 1057 | 1049 | 0.76% |

Block counts below use the scheduler's weighted block score (scoreBefore/scoreAfter: stalls per way into the block, loop entries weighted 10, plus stalls caused in the next block's first instructions), the objective the scheduler and optimizer minimize; plain predicted stalls are in blocks.csv (predicted_stalls_*). Remaining score is classified only where the optimizer proved it optimal.

Blocks with a predicted stall score: 47 (score removed: 28, reduced: 2, not reduced: 17). Blocks with a higher score after reordering (a stall moved to their head by the block before): 0

| reason | blocks not reduced | % | all blocks with stalls left | % |
|---|---|---|---|---|
| dependency_chain | 3 | 17.6% | 3 | 15.8% |
| nothing_independent | 0 | 0.0% | 0 | 0.0% |
| memory_ordering | 0 | 0.0% | 1 | 5.3% |
| block_boundary | 14 | 82.4% | 15 | 78.9% |
| false_stall_immediate | 0 | 0.0% | 0 | 0.0% |
| other_not_proven_optimal | 0 | 0.0% | 0 | 0.0% |
| scheduler_missed_optimum | 0 | 0.0% | 0 | 0.0% |

## rv5-h-nf

Scheduled with ScheduleMode::Optimal (block-entry scoring). "total cycles" is the raw reordered program; the safety-net line is what is kept.

- programs tried: 54, improved: 51, unchanged: 3, slower: 0, not equivalent: 0
- total cycles: 8249 -> 7016 (14.95% fewer)
- after the VM safety net (applyWithVmCheck keeps a block's new order only if the program stays equivalent and gets no slower): 8249 -> 7016 (14.95% fewer); programs improved: 51, fell back to the original: 0, some blocks rejected: 0
- overall CPI: 2.301 -> 1.957
- mean cycle reduction per program: 15.46% (over all tried), 16.37% (over improved only)
- max cycle reduction: 41.67% (ref_load_store_alu_mix)

| category | tried | improved | cycles before | cycles after | reduction |
|---|---|---|---|---|---|
| aliasing | 8 | 8 | 539 | 438 | 18.74% |
| chain | 10 | 10 | 2457 | 2353 | 4.23% |
| hazard | 17 | 14 | 408 | 347 | 14.95% |
| loop | 10 | 10 | 3047 | 2362 | 22.48% |
| mixed | 9 | 9 | 1798 | 1516 | 15.68% |

Block counts below use the scheduler's weighted block score (scoreBefore/scoreAfter: stalls per way into the block, loop entries weighted 10, plus stalls caused in the next block's first instructions), the objective the scheduler and optimizer minimize; plain predicted stalls are in blocks.csv (predicted_stalls_*). Remaining score is classified only where the optimizer proved it optimal.

Blocks with a predicted stall score: 116 (score removed: 13, reduced: 73, not reduced: 30). Blocks with a higher score after reordering (a stall moved to their head by the block before): 2

| reason | blocks not reduced | % | all blocks with stalls left | % |
|---|---|---|---|---|
| dependency_chain | 4 | 13.3% | 53 | 51.5% |
| nothing_independent | 1 | 3.3% | 5 | 4.9% |
| memory_ordering | 0 | 0.0% | 1 | 1.0% |
| block_boundary | 25 | 83.3% | 43 | 41.7% |
| false_stall_immediate | 0 | 0.0% | 1 | 1.0% |
| other_not_proven_optimal | 0 | 0.0% | 0 | 0.0% |
| scheduler_missed_optimum | 0 | 0.0% | 0 | 0.0% |
