# Phase 2 brief: invent computational worlds (2026-11-01)

Goal: invent and test new computational models, not optimize compilers. Each model gets:
1. the smallest language or execution semantics that demonstrates it;
2. a minimal working implementation;
3. one operation that should need orders of magnitude less data movement than a conventional implementation;
4. a measured complete cost: instruction generation, recomputation, and i-cache traffic where observable;
5. a workload where it fails badly;
6. a judgment on whether it scales to useful computation.

Themes: programs as generators; data structures as executable descriptions; no separation between compiling and executing; IRs that never exist; memory residency as a semantic constraint; duplicating computation freely to avoid communication.

Ranking criterion: bytes moved and what the model changes about what a program *is*, not speed. A model 50× slower that moves 1000× fewer bytes counts as a success.

Conventional compilers serve only as controls. Keep the phase-1 scoping: one question, one number, one decision rule per experiment.

Lessons from phase 1 that apply:
- Every mechanism lost to a cheap conventional baseline unless the question had no conventional answer, so each model names its strongest baseline up front.
- Requiring bit-identity with a conventional result forced mechanisms to copy quirks; judge by semantics.
- Decode or interpretation cost (E12: ~70× native) dominates any "run compressed" idea unless the model avoids per-instruction decoding.
