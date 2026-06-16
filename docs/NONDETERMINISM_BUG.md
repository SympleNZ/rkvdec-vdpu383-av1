# The AV1 decode non-determinism bug — full triage

VDPU383 (RK3576) V4L2 stateless AV1. Every above-MMIO input matches the vendor
MPP backend, yet our driver's output is wrong — and, critically, **wrong
non-deterministically**: the same frame from fresh state decodes correctly about
half the time and otherwise lands in one of a few discrete wrong states. MPP
decodes the same stream byte-exact on the same silicon. This document is the
evidence trail.

> **History / correction (2026-06-13).** Earlier revisions of this repo described
> this as a *deterministic partial decode* ("the top ~38 % of rows reconstruct,
> the rest is flat DC fallback"). **That framing was incomplete and is corrected
> here.** The deterministic top-band/DC signature was one snapshot of the *wrong*
> outcome; the actual behaviour is **per-decode non-determinism** — the same KEY
> frame, decoded repeatedly from fresh state, produces a *correct* result roughly
> half the time and a *different* wrong (or silent) result the rest of the time.
> The doc was also corrected on a second point: a once-promising **RCB SRAM-vs-DRAM
> placement** lever was **refuted** (§5). The "below-MMIO" conclusion is unchanged
> and is now established more strongly (§3, §4).

## 1. Symptom — per-decode non-determinism

Test vector: `av1-1-b8-02-allintra_20201006.ivf` — 352×288, 4:2:0, 8-bit,
all-intra, a single KEY frame. Decoded repeatedly from fresh module/session state,
the same frame settles into one of a few discrete outcomes:

| Outcome | ~rate | adapted CDF write-back | HW status | notes |
|---|---|---|---|---|
| **CORRECT** | ~50–75 % | the dominant correct value | clean DEC_RDY (`sta=0x1`) | byte-exact |
| **WRONG** | ~20–40 % | a discrete wrong attractor (a few recurring values, sometimes unique per run) | clean DEC_RDY, **no error bits** | reconstruction diverges |
| **SILENT** | ~10–30 % | `0` — no CDF written back at all | self-cleared `dec_en`, no IRQ | HW "completes" but writes no adapted state |

The rate is non-stationary across a session (it can degrade toward more-frequent
silent/wrong as a session runs, then recover). Within a fresh decode the outcome
is a coin-flip: **same input bytes + clean completion → one of several discrete
states.** When wrong, the spatial manifestation varies (a degraded/wrong
reconstruction; the old "top band correct, lower rows collapse to DC" was one such
manifestation, not a fixed signature).

**It is not "HW stops early."** Prefilling the output buffer with a sentinel and
decoding shows it fully overwritten — the HW writes a complete frame; it is the
reconstruction that is wrong/non-deterministic.

## 2. Decode completion is solved (and a NULL-deref had to be fixed first)

The VDPU383 frequently finishes a decode, self-clears `DEC_ENABLE`, and never
raises the completion IRQ ("silent completion"). A watchdog that reads
`LINK_STA_INT` / `DEC_ENABLE` and treats a self-cleared `dec_en` as DONE lets AV1
complete without a D-state hang.

Note: in this downstream tree the watchdog's VP9 link-mode silent-completion
telemetry dereferences `ctx->link_table`, which is NULL in single-shot mode (AV1)
→ watchdog-worker oops → D-state hang on the first decode. The NULL-guard fix is
in `src/rkvdec.c` (`rkvdec_watchdog_func`). Without it, AV1 does not decode at
all. (The watchdog makes decode *complete*; it does not affect the non-determinism
— see §4, where the bug survives masking all interrupts entirely.)

## 3. What was proven equal to MPP, and that MPP is correct

All dumps captured with MPP's `DUMP_VDPU38X_DATAS` on an RK3576 BSP board
(`mpi_dec_test -t 16777224`), against our kernel-side dumps on the mainline V4L2
board, for the same frame.

### 3.1 Every above-MMIO input is byte-identical to MPP
- **Global header (GBL)** — the ~2900-bit packed AV1 uncompressed frame header
  (`global_cfg.dat`) matches MPP byte-for-byte (all frame-header features: CDEF /
  LF / LR / segmentation / global-motion / superres / delta-q / colour config /
  film-grain params).
- **Default CDF** — `av1_default_cdf` == MPP `cdf_rd_def.dat`.
- **Register file** — of 171 common registers, the only differences are benign
  (`reg13` timeout) or incomparable (resolved IOVA vs MPP fd+offset); `reg8`
  dec_mode=4, `reg9`, stream framing (`reg65/66/67`), strides, ref strides all
  match.
- **Bitstream** — our synthesised `[TD][OBU_FRAME][LEB][body]` body matches MPP's
  FRAME body byte-for-byte; adding the SEQ_HDR OBU to match MPP exactly changes
  nothing.

### 3.2 MPP decodes it correctly — verified at output level
On the BSP board, `mpi_dec_test` decodes the full 39-frame `allintra` clip
**byte-identical to the dav1d software reference — 39/39 frames, identical md5
across repeated runs, fully deterministic, no decoder wedge.** So the silicon
decodes this stream correctly and deterministically under the vendor stack; the
defect is in our software path, reachable in principle. (Vendor decode *time*
varied 40→146 ms across the identical runs — incidental cache-warmth timing
jitter, decoupled from correctness.)

## 4. It is HW-internal, below everything we program — proven, not assumed

Two on-hardware results place the non-determinism below the register/MMIO and
below any CPU activity we perform during the decode:

- **Per-decode register read-back is identical regardless of outcome.** Reading
  back the full control/parameter register file immediately before each kick and
  CRC-ing it: the CRC is **byte-identical run-to-run across CORRECT, WRONG, and
  SILENT decodes.** Our register programming is provably deterministic and is not
  the source; RCB slot-4 sits at a fixed address every run; the CDF buffer address
  does not predict the outcome (the same address produced both a correct and a
  wrong decode).
- **It survives masking all link interrupts.** Gating off the INT_EN arming so the
  decode runs with **zero CPU↔HW MMIO between the kick and the watchdog reap**
  leaves the outcome distribution unchanged. So nothing the driver does mid-decode
  (IRQ handling, status acks, re-arms) drives it. (Side observations: the IRQ line
  fires regardless of INT_EN on this IP; SILENT decodes latch an early `0x2`
  line-event the correct ones do not.)

So the same input, same register file, same buffers, with no CPU in the loop,
settles into one of a few discrete states — an un-primed/metastable **internal**
decoder state sampled at decode start.

## 5. RCB size and placement — REFUTED (both)

**Size.** A natural lead is that our RCB is undersized for AV1 — too-small RCB can
produce random results. The source backs the premise: our RCB size table is
derived from MPP's *VP9* `rcb_calc`, while MPP keeps a separate
`vdpu383_av1d_rcb_calc` that is larger in several regions (inter rows ~16% bigger,
plus nonzero strmd-in and fltd-upsc-on-col regions and loop-restoration terms the
VP9 calc zeroes). **Tested by uniformly enlarging every RCB region 2× and 3×:**
the AV1 output is **byte-identical across scale 1/2/3**, and with the coin-flip
live (fresh module loads) the outcome distribution is unchanged. The enlargement
also pushed the RCB from SRAM into DRAM (see placement below), and it brackets the
intra-above-row slot across a ~15× size range (≈1 KB to ≈17 KB) with no effect.
RCB undersize does **not** cause the non-determinism — the IOMMU appears to cover
any undersize.

**Placement.** A controlled per-region SRAM-vs-DRAM A/B gave a **byte-identical
failing outcome** (AV1 1080p MAE 97.6 = 97.6), re-confirmed incidentally by the
size test above (scale=1 ran in SRAM, scale=3 in DRAM, byte-identical). The
earlier "DRAM reconstructed a row" observation was a first-decode artifact of the
§1 non-determinism, not a placement effect. The RCB **slot-4** intra-above-row
sizing theory was independently falsified (MPP's exact slot-4 length changes
nothing). Cache-attribute variants (cached RCB + per-frame `dma_sync`,
`dma-buf-cache`) are also negative.

## 6. The state is pinned per module-load — and the probe warmup is not the cause

The non-determinism is latched **per module-load**, not per-decode. Within a
single `insmod` the result is deterministic (dozens of identical decodes); each
fresh `rmmod`/`insmod` re-rolls it into one of a few discrete states; and a `pm`
suspend/resume cycle transitions it once (a fresh-probe state → a post-resume
state) then holds. It is also **non-stationary across longer timescales** — the
same vector decoded correctly ~half the time in one session and was consistently
wrong in another, with no driver change (a silicon baseline drift, thermal/uptime
the suspects). This per-load latching points at internal state set at power-up /
device init.

The obvious suspect is the **H.264-shaped probe warmup** (a priming decode at
device init that the VP9/H.264 paths rely on; MPP runs no AV1 equivalent) leaving
the entropy/context engine in a state that's right for H.264 but wrong for AV1.
**Tested by disabling the probe warmup entirely** (`warmup_off`, confirmed via
`dmesg` that no warmup ran): 6 fresh loads with the warmup ON and 6 with it OFF
gave the **same wrong-attractor distribution**. The warmup is **not** the pin —
disabling it neither fixes AV1 nor changes the failure. So the per-load state is
set at init by something *other* than the warmup, below every software knob.

> Note on measurement: classify outcomes by **pixel output vs the dav1d reference**,
> not by reading back the adapted CDF / register file — those heavy MMIO read-backs
> themselves perturb the metastable state and shift the outcome distribution.

## 6.5 The submission model (single-shot vs link/CCU) — tested, INCONCLUSIVE (port not yet MPP-faithful)

A function-order trace of the vendor stack (MPP) on the BSP board showed MPP runs
AV1 through the VDPU383 **link/CCU descriptor path** — per frame it builds the
register set into a DMA descriptor the hardware fetches, with *zero* per-frame
register MMIO — whereas this driver programs the registers by direct MMIO and
kicks a single-shot decode. Continuous link submission keeps the IP occupied and
primed across frames; single-shot re-arms it each decode. Since the bug is a
per-init metastability, the submission model was a well-motivated suspect and the
one remaining untested *structural* lever.

**Tested by implementing link/CCU submission for AV1** (the same codec-agnostic
descriptor ring the VP9/H.264 paths use) and running an interleaved A/B battery
— 6 fresh loads single-shot vs 6 fresh loads link mode (depth-4 continuous), in
one session to control for the non-stationary regime, classifying each by
frame-0 pixel md5. Both modes were **0/6 correct**, and — decisively — link mode
landed on the **identical per-load wrong attractors as single-shot** (the same
byte-exact reconstructions, e.g. md5 `beaa2fd68e85` and `74d914e75186`, recur in
both). If the submission model changed the IP's internal metastable state we
would see a different family of outcomes; instead both reach the same wrong
results.

**Caveat — the test is not yet definitive.** A subsequent code audit against the
vendor BSP link driver showed our AV1 link path is **not yet a faithful
reproduction of MPP's CCU model**: it programs the full register file to MMIO
(single-shot state) and bolts a descriptor kick on top, so the hardware never
performs the per-descriptor completion writeback the vendor path relies on
(it completes *silently*, like single-shot, and our watchdog recovers each frame
by reset+resend). It also never runs the genuine continuous CCU ring
(`CCU_WORK_EN`, dynamic node re-stitch, hard-CCU reap are absent). So the A/B
above effectively compared single-shot against single-shot-with-descriptor-
overhead — which is consistent with the identical attractors, but means the
submission model is **not yet excluded**.

**Follow-up (descriptor built faithfully — still no writeback).** We then
rebuilt the link path so the per-frame register set is assembled directly into
the descriptor from the register structs, with no MMIO-first programming and the
single-shot INT_EN arm removed — i.e. the descriptor itself now matches the
vendor's `rkvdec2_link_prepare` (the `tb_reg` node pointers, the `tb_reg_int`
offset, and a zeroed part_r writeback region). The hardware **still writes no
per-task completion status back into the descriptor**, at any pipeline depth —
and, tellingly, the **same failure occurs for VP9** through the same link path.
So this is a shared limitation of how a mainline V4L2 m2m client arms the
single-core link path, not anything AV1-specific, and it lives one level below
the descriptor contents — in the enqueue/init sequence that *arms* the writeback
(cache-clear position, the CFG_DONE window, or a work-mode/writeback-enable
register we have not identified). Until the link path produces clean writebacks
it cannot run as a genuine continuous node-walk, so the submission-model lever
stays **inconclusive — neither confirmed nor refuted.** Resolving it needs a
live MMIO trace of the vendor enqueue, or a pointer from someone with the
link/CCU documentation.

## 7. Conclusion — a shared internal-state class with the sibling bugs

Every programmable input matches MPP, MPP decodes the vector byte-exact and
deterministically on the same silicon, and the failure survives both an identical
register file and the removal of all mid-decode CPU activity. The divergence is an
**un-primed / metastable internal decoder state**, below the MMIO interface, that
the vendor submit path pins and a mainline V4L2 m2m client does not.

This is the same class as the other VDPU383 V4L2 findings:
- **H.264** deblock race — fixed, and the fix is instructive: it was a **power-up
  warmup decode at device probe**, an init/lifecycle operation invisible to any
  per-frame register diff. The same "prime an internal state outside the per-frame
  path" shape may apply here.
- **VP9** small-footprint reference bypass (sibling repo) — small-MC-footprint
  inter frames reconstruct from a *retained internal copy of the most-recent
  reference* (seeded by the immediately-preceding decode) instead of the
  programmed reference; also VP9-specific, also address/cache-invariant.
- **AV1** decode non-determinism (this bug).

The common thread is an internal per-frame context / reference-retention block
whose initialisation is not deterministic from the V4L2 side. A shared root is a
hypothesis, not proven.

## 8. The question

What does the vendor stack do — at device/session init through `mpp_service`, or
in the submit path — that **pins the VDPU383's internal entropy/context state so
an AV1 KEY frame decodes deterministically**, which a mainline V4L2 m2m client
does not, given the per-frame register file, GBL, CDF, and bitstream are all
byte-identical and the result is independent of our register programming and of
all mid-decode CPU activity? A pointer to the relevant IP state or init sequence
would unblock this.

## 9. Reproduce

See [`BUILD_AND_TEST.md`](BUILD_AND_TEST.md). In short: build `src/`, insmod,
decode the all-intra vector **several times from fresh state**, and compare each
output to the dav1d reference (and to each other) — the result varies run-to-run
between correct and a discrete wrong/silent state. MPP cross-dumps: build MPP with
`DUMP_VDPU38X_DATAS`, decode the same `.ivf` with `mpi_dec_test -t 16777224`, and
diff `global_cfg.dat`, `cdf_rd_def.dat`, `regs_full.dat`, and `stream_in.dat`
against the kernel-side dumps (all byte-identical).
