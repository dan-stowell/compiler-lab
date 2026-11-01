# Cycle 2 plan (revised after review)

E4 (ring heap) and E6 (cross-TU lexing) were settled on paper; see JOURNAL.md. Two experiments remain, in the usual format: one question, one number, one input, one rule.

## E5: Dependency slices (assumption: compiling a function needs the whole TU resident)
- Question: for each function defined in `chibicc -E parse.c`, how large is its verified closure slice as a fraction of the TU, measured in tokens of top-level declarations? Report the median over all functions.
- Closure: start from the function's body. Follow identifiers to top-level declarations (typedefs, struct/union/enum tags, enum constants, globals, prototypes). For any other function, include only its declarator (prototype), never its body. Keep declarations in their original order.
- Verify every function: compile slice+function with chibicc. Compare the target function's asm after stripping `.loc` and `.file`, renumbering `.L..N` and string-literal labels, and comparing only that function's text. A slice that fails to compile or differs counts as the full TU (100%).
- Rule: median ≤10% means content-addressed slices could feed stateless workers. Otherwise, the header's mutually referencing structs pull in too much.

## E7: Output-addressed asm over history (assumption: a compiler is a local function of its input)
- Question: compile each historical commit's `.c` files with a fixed compiler (chibicc at HEAD). What is the median per-commit fraction of functions whose asm (one function's section) hash changes? Measure it twice: on raw asm, and with labels renumbered within each function and `.loc` stripped.
- Commits whose files HEAD chibicc can't compile are excluded and counted.
- Rule: if the normalized median is ≤3%, output addressing removes the ripple that closure keys showed in E2. If raw minus normalized is >5 points, hidden global counters in the compiler cause ripple, a property of the compiler, not of the program.
