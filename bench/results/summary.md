# Reordering benchmark results

## Validity filter

- programs found: 55
- valid: 54
- near-duplicates removed: 0
- usable: 54
- rejection reasons (a program can have several):
  - label named 'data' (a section directive): 1

## rv5-h-f

- programs tried: 54, improved: 28, unchanged: 26, slower: 0, not equivalent: 0
- total cycles: 5465 -> 5315 (2.74% fewer)
- with the keep-original rule (reordered kept only if equivalent and faster): 5465 -> 5315 (2.74% fewer), no program slower
- overall CPI: 1.524 -> 1.483
- mean cycle reduction per program: 5.22% (over all tried), 10.07% (over improved only)
- max cycle reduction: 19.05% (hazard_byte_loads)

| category | tried | improved | cycles before | cycles after | reduction |
|---|---|---|---|---|---|
| aliasing | 8 | 8 | 349 | 308 | 11.75% |
| chain | 10 | 1 | 1859 | 1854 | 0.27% |
| hazard | 17 | 10 | 305 | 283 | 7.21% |
| loop | 10 | 6 | 1895 | 1821 | 3.91% |
| mixed | 9 | 3 | 1057 | 1049 | 0.76% |

Blocks with predicted stalls: 40 (stalls removed: 27, reduced: 1, not reduced: 12). Blocks with more predicted stalls after reordering (a stall moved to their head by the block before): 0

| reason | blocks not reduced | % | all blocks with stalls left | % |
|---|---|---|---|---|
| dependency_chain | 3 | 25.0% | 3 | 23.1% |
| nothing_independent | 0 | 0.0% | 0 | 0.0% |
| memory_ordering | 1 | 8.3% | 2 | 15.4% |
| block_boundary | 8 | 66.7% | 8 | 61.5% |
| false_stall_immediate | 0 | 0.0% | 0 | 0.0% |
| other_not_proven_optimal | 0 | 0.0% | 0 | 0.0% |

## rv5-h-nf

- programs tried: 54, improved: 47, unchanged: 6, slower: 1, not equivalent: 0
- total cycles: 8249 -> 7179 (12.97% fewer)
- with the keep-original rule (reordered kept only if equivalent and faster): 8249 -> 7173 (13.04% fewer), no program slower
- overall CPI: 2.301 -> 2.003
- mean cycle reduction per program: 13.92% (over all tried), 16.02% (over improved only)
- max cycle reduction: 41.67% (ref_load_store_alu_mix)

| category | tried | improved | cycles before | cycles after | reduction |
|---|---|---|---|---|---|
| aliasing | 8 | 8 | 539 | 440 | 18.37% |
| chain | 10 | 7 | 2457 | 2387 | 2.85% |
| hazard | 17 | 14 | 408 | 347 | 14.95% |
| loop | 10 | 9 | 3047 | 2451 | 19.56% |
| mixed | 9 | 9 | 1798 | 1554 | 13.57% |

Blocks with predicted stalls: 127 (stalls removed: 22, reduced: 63, not reduced: 42). Blocks with more predicted stalls after reordering (a stall moved to their head by the block before): 8

| reason | blocks not reduced | % | all blocks with stalls left | % |
|---|---|---|---|---|
| dependency_chain | 2 | 4.8% | 28 | 26.7% |
| nothing_independent | 0 | 0.0% | 1 | 1.0% |
| memory_ordering | 0 | 0.0% | 2 | 1.9% |
| block_boundary | 39 | 92.9% | 49 | 46.7% |
| false_stall_immediate | 0 | 0.0% | 1 | 1.0% |
| other_not_proven_optimal | 1 | 2.4% | 24 | 22.9% |
