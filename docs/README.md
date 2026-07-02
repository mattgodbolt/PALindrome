# docs/

The design and reference notes the code assumes. Start with the top-level
README (what the project is, how to build and run it, the signal-flow graph);
these go deeper on single topics.

- [performance.md](performance.md) - where the render/live pipeline spends its
  time and which optimisations paid off (and which dead-ended). The
  authoritative perf record: start here before any speed work.
- [corpus.md](corpus.md) - the reference captures under `corpus/`: what each
  clip is, how it was captured, and its load-bearing quirks.
- [TDA3561A.md](TDA3561A.md) - transcription/notes for the Philips TDA3561A
  PAL decoder IC, the period chroma chip the colour path models (the datasheet
  PDF sits alongside).
- [Firetrack_BW_Trick.md](Firetrack_BW_Trick.md) - how Firetrack's BBC Micro
  loader forced B&W on a PAL set: an ident-parity attack, with the
  reproduction recipe the planned synthetic test uses (issue #40).
- ETD_Info_Sheet_21W_PAL_Coding_Revision.pdf - BBC engineering training
  reference for PAL coding (the Long-Tc/Short-Tc reference model the chroma
  decoder's APC/ident comments cite).
