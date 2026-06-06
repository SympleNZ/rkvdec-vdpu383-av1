# The AV1 partial-decode bug — full triage

VDPU383 (RK3576) V4L2 stateless AV1. Every above-MMIO input matches the vendor
MPP backend, yet our driver's output is a partial frame while MPP decodes the
same vector correctly on the same silicon. This document is the evidence trail.

## 1. Symptom

Test vector: `av1-1-b8-02-allintra_20201006.ivf` — 352×288, 4:2:0, 8-bit,
all-intra. Per-row luma signature of decoded frame 0 (W=352):

```
row   0 : real picture (unique≈59 values/row, varied pixels)
row  64 : real
row  96 : real / transitional
row 128 : 0x26 fill  (unique=6, ~constant)  <-- DC fallback
row 160..287 : 0x26 fill
```

Top ~38 % of rows reconstruct; the rest is flat AV1 **DC intra-prediction
fallback**. PSNR ≈ 11 dB. 0/13 Chromium 8-bit conformance vectors bit-exact.
KEY and INTER show the identical pattern.

**It is not "HW stops early."** Prefilling the output buffer with a sentinel and
decoding shows >99.9 % overwritten — the HW writes the entire frame. The defect
is that the **intra above-row prediction context for super-block row ≥ 1 is
wrong**: lower SB rows predict from garbage above-context and collapse to DC.

## 2. Decode completion is solved (and a NULL-deref had to be fixed first)

The VDPU383 frequently finishes a decode, self-clears `DEC_ENABLE`, and never
raises the completion IRQ ("silent completion"). A watchdog that reads
`LINK_STA_INT` / `DEC_ENABLE` and treats a self-cleared `dec_en` as DONE makes
AV1 complete reliably (no hangs, no IOMMU faults).

Note: in this downstream tree the watchdog's VP9 link-mode silent-completion
telemetry dereferences `ctx->link_table`, which is NULL in single-shot mode (AV1)
→ watchdog-worker oops → D-state hang on the first decode. The NULL-guard fix is
in `src/rkvdec.c` (`rkvdec_watchdog_func`). Without it, AV1 does not decode at
all; with it, AV1 decodes reliably (the partial frame below).

## 3. What was proven equal to MPP (cross-dumped on the BSP)

All dumps captured with MPP's `DUMP_VDPU38X_DATAS` on an RK3576 BSP board
(`mpi_dec_test -t 16777224`), against our kernel-side dumps on the mainline V4L2
board, for the same frame 0.

### 3.1 Global header (GBL) — byte-identical
The ~2900-bit packed AV1 uncompressed frame header (`global_cfg.dat`) matches
MPP byte-for-byte (established earlier; re-confirmed). All frame-header features
(CDEF / LF / LR / segmentation / global-motion / superres / delta-q / colour
config / film-grain params) are packed and correct.

### 3.2 Default CDF — byte-identical
`av1_default_cdf` == MPP `cdf_rd_def.dat`.

### 3.3 Register file — matches MPP
MPP dumps the full `Vdpu383RegSet` (`regs_full.dat`: ctrl_regs @reg8, comm_paras
@reg64, comm_addrs @reg128). Mapped to register numbers and diffed against a
kernel-side readback of `reg8..reg232` taken immediately before the decode kick.
Of 171 common registers, the only differences are benign or incomparable:

| reg | MPP | ours | meaning | verdict |
|-----|-----|------|---------|---------|
| 13 | 0x00ffffff | 0x02cfffff | core_timeout_threshold | benign |
| 104/105/106 | 0 | strides | ref7 strides | benign (all-intra has no refs; ref0–6 match) |
| 141…161 odd | exact | over-alloc | RCB slot sizes | benign (see §4) |
| even 128–216 | fd-relative | resolved IOVA | base addresses | incomparable (MPP fd+offset; kernel adds base at IOCTL) |

Everything else identical: `reg8` dec_mode=4 (AV1), `reg9` important_en=0, stream
framing (`reg65` start_bit, `reg66` stream_len, `reg67` global_len), output
strides (`reg68/70`), ref0–6 strides (`reg83–103`).

### 3.4 Bitstream — byte-identical
MPP feeds HW `[TD][SEQ_HDR(11)][FRAME(36248)]`; its FRAME body == the `.ivf`
frame-0 FRAME body byte-for-byte. Our driver synthesises
`[TD][OBU_FRAME][LEB=36248][36248-byte body]` (V4L2 passes the FRAME OBU body;
sequence info comes via the GBL). Our body matches MPP's FRAME body byte-for-byte.
Adding the SEQ_HDR OBU so our stream is **byte-identical to MPP** changes nothing
— the in-stream SEQ_HDR is not load-bearing (HW takes seq from the GBL).

## 4. The RCB slot-4 theory — falsified

A long-standing lead held that RCB **slot 4** (`RCB_INTRA_IN_ROW`, the intra
above-row context, reg148) was the cause. It is not:

- MPP's `vdpu383_av1d_rcb_calc` sizes slot 4 at **1088 B** (AV1-specific:
  `ALIGN(in_tl_row·(bit_depth=10+2)·intra_uv_coef[420]=2, 512)` bits → bytes).
  Our codec-agnostic `vdpu383_rcb_sizes[]` over-allocates it (≈12288 B). The HW
  reads/writes structured above-row context into slot 4 either way (confirmed by
  a zero-prefill + post-decode non-zero scan: ~2092 non-zero B/frame).
- **A/B test:** programming slot-4's length register to MPP's 1088 → **byte-
  identical partial decode, no change.** reg149 is not a per-SB-row stride; the
  over-allocation is benign.
- A RAM redirect of slot 4 (to perturb its contents) **hangs the IP** — the RCB
  lives in on-chip SRAM and a RAM IOVA is invalid for the HW's RCB access path.
  (This also means an earlier "RAM-override changed the fallback values, so HW
  reads slot 4" observation does not reproduce — the override just wedges decode.)

So the intra above-row context is HW-written and HW-read within a correctly-based,
adequately-sized buffer, yet SB row ≥ 1 still predicts from wrong context.

## 5. Conclusion — below the MMIO interface

GBL + CDF + every register + the full bitstream all match the known-good MPP
backend, and the same silicon decodes the vector correctly under MPP — so the
divergence is **below the MMIO register interface**, where the V4L2 driver has no
visibility. This is the same shape as the other two open VDPU383 V4L2 bugs:

- **H.264** deblock race (rows 4/12 mod 16, non-deterministic) — every register /
  clock / cache / RCB matched to MPP, still races; wiring it through the
  link-table submit path raced at the same rate. Below-MMIO.
- **VP9** compound (`reference_mode = SELECT`) collapses to an alt-ref copy —
  registers/GBL/probs byte-identical to MPP; below-MMIO.
- **AV1** intra above-row context for SB row ≥ 1 (this bug).

## 5b. A lever below the register interface: RCB placement (SRAM vs DRAM)

Reading the Rockchip BSP kernel driver (`drivers/video/rockchip/mpp/mpp_rkvdec2.c`)
shows the working stack treats RCB differently from a mainline V4L2 client, in a way
the `regs_full.dat` diff could not see:

- `mpp_set_rcbbuf()` runs **in the kernel at submit time** and rewrites the RCB base
  registers (reg140–160): it points them at the SRAM window only when the DT provides
  `rockchip,rcb-iova` **and** `frame_width >= rcb_min_width`; otherwise it leaves them
  pointing at the HAL's **DRAM** buffers.
- The BSP board's live DT has **no `rcb-iova` / `rcb-min-width`**, so the working MPP
  stack decodes this vector with **DRAM RCB**. Because the base-register rewrite is in
  the kernel, *after* the HAL dump, §3.3's register comparison never saw it.
- Our mainline V4L2 driver always uses **SRAM** RCB (`rkvdec-rcb.c`; slot-4 `type=1`).

**Experiment.** Forcing all-DRAM RCB in our driver (gate off the SRAM `gen_pool`
path) **changed the regional decode pattern**: super-block row 4 (rows 256–287), which
is flat fill under SRAM RCB, **reconstructed** under DRAM RCB on the first decode after
module load. There is also a first-decode-vs-subsequent state dependence. Stable across
reruns.

So the partial decode is **sensitive to RCB intra-above-row buffer placement and state**
— it is *not* fully opaque silicon; it is at least partly in our control. Neither SRAM
nor DRAM alone produces a correct frame, which is consistent with a HW-internal above-row
read-after-write path that behaves differently for the SRAM vs DRAM ports (HW→HW, no CPU
in the loop). Untried levers: cached RCB + cache management (the BSP allocates through
`dma-buf-cache`), `IOMMU_CACHE`, per-frame RCB init, and exact replication of MPP's RCB
sizing/placement.

This is shared infrastructure — the same RCB allocator and in-kernel base rewrite apply
to H.264/HEVC/VP9 — so the SRAM-vs-DRAM *divergence* (and the dump blind spot) is
cross-codec. But A/B-testing the placement on the other codecs shows the *sensitivity* is
**AV1-specific**: forcing DRAM RCB does not change the H.264 deblock race (clean 6/8 vs
5/8, N=8) or the VP9 compound collapse (487/789 frames wrong under both). Only AV1's
intra above-row context responds to RCB placement.

## 6. The question

Is there a one-time device / session setup the vendor stack performs through its
`mpp_service` kernel path — outside the per-frame register programming a V4L2 m2m
client issues — that arms the VDPU383's intra above-row-context reconstruction
across super-block rows? A pointer to the relevant IP state would unblock this;
no expectation of a full investigation.

## 7. Reproduce

See [`BUILD_AND_TEST.md`](BUILD_AND_TEST.md). In short: build `src/`, insmod,
decode the all-intra vector, inspect the per-row luma signature (rows 0–~96 real,
128+ flat fill). MPP cross-dumps: build MPP with `DUMP_VDPU38X_DATAS`, decode the
same `.ivf` with `mpi_dec_test -t 16777224`, and diff `global_cfg.dat`,
`cdf_rd_def.dat`, `regs_full.dat`, and `stream_in.dat` against the kernel-side
dumps.
