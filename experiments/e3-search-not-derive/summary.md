- expressions: 200; timeouts (failures): 0
- mean length ratio (search/derived): 0.7682
- median length ratio: 1.0000
- search strictly shorter: 83/200 (of which 1 use DUP/SWAP/OVER)
- mean derived length 5.77, mean search length 4.28
- search CPU: total 177.1 s; per expression mean 0.885 s, median 0.002 s, max 7.95 s
- peak RSS (max over per-expression processes): 104.3 MiB
- distinct states stored: total 40,708,123, max per expression 1,506,711; child states examined: total 186,105,870

By derived length:

| derived len | n | mean search len | mean ratio | strictly shorter | mean CPU s |
|---|---|---|---|---|---|
| 3 | 64 | 2.50 | 0.833 | 16 | 0.000 |
| 5 | 42 | 4.19 | 0.838 | 13 | 0.003 |
| 7 | 47 | 4.91 | 0.702 | 26 | 0.070 |
| 9 | 47 | 6.15 | 0.683 | 28 | 3.695 |

Search length histogram: 1: 36, 3: 68, 5: 47, 7: 30, 9: 19

Decision rule (mean ratio <= 0.75): NOT MET

Candidate striking examples (largest saving first):

- #47 `(x - ((x | x) - (z ^ z)))`: derived (9) `PUSHX; PUSHX; PUSHX; OR; PUSHZ; PUSHZ; XOR; SUB; SUB` -> found (1) `PUSHC 0`
- #124 `((x & (x | 3)) | (3 & x))`: derived (9) `PUSHX; PUSHX; PUSHC 3; OR; AND; PUSHC 3; PUSHX; AND; OR` -> found (1) `PUSHX`
- #134 `((y * (z & x)) & (z ^ z))`: derived (9) `PUSHY; PUSHZ; PUSHX; AND; MUL; PUSHZ; PUSHZ; XOR; AND` -> found (1) `PUSHC 0`
- #161 `((5 & ((x ^ x) & z)) * x)`: derived (9) `PUSHC 5; PUSHX; PUSHX; XOR; PUSHZ; AND; AND; PUSHX; MUL` -> found (1) `PUSHC 0`
- #164 `(((z * z) * x) * (y ^ y))`: derived (9) `PUSHZ; PUSHZ; MUL; PUSHX; MUL; PUSHY; PUSHY; XOR; MUL` -> found (1) `PUSHC 0`
- #184 `(((6 * (z + y)) + 5) * 8)`: derived (9) `PUSHC 6; PUSHZ; PUSHY; ADD; MUL; PUSHC 5; ADD; PUSHC 8; MUL` -> found (1) `PUSHC 8`
- #191 `((z - z) & ((y ^ 3) & x))`: derived (9) `PUSHZ; PUSHZ; SUB; PUSHY; PUSHC 3; XOR; PUSHX; AND; AND` -> found (1) `PUSHC 0`
- #126 `(x ^ (z - (x & (z | z))))`: derived (9) `PUSHX; PUSHZ; PUSHX; PUSHZ; PUSHZ; OR; AND; SUB; XOR` -> found (3) `PUSHX; PUSHZ; OR`
- #151 `(z - ((x - x) ^ (z + x)))`: derived (9) `PUSHZ; PUSHX; PUSHX; SUB; PUSHZ; PUSHX; ADD; XOR; SUB` -> found (3) `PUSHX; PUSHC 15; MUL`
- #153 `(y + ((1 * z) | (z & 4)))`: derived (9) `PUSHY; PUSHC 1; PUSHZ; MUL; PUSHZ; PUSHC 4; AND; OR; ADD` -> found (3) `PUSHY; PUSHZ; ADD`

Shorter programs using stack ops:

- #74 `((x ^ y) * ((x ^ y) & 13))`: derived (9) -> found (7) `PUSHX; PUSHY; XOR; PUSHC 13; OVER; AND; MUL`
