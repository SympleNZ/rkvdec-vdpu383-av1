# rkvdec-vdpu383-av1 — V4L2 stateless AV1 decoder for Rockchip RK3576 (VDPU383)

A **V4L2 stateless AV1 decoder** for the Rockchip **RK3576** SoC's **VDPU383**
video IP, built on top of the now-mainline VDPU383 H.264/H.265 `rkvdec`
infrastructure and the V4L2 stateless AV1 uAPI. Every part of the driver the
software side controls is implemented and **byte-identical to the vendor library
(MPP)** — sequence/frame/tile/film-grain controls, the full global-header pack,
the default CDF tables, reference/DPB management, RCB/SRAM allocation, OBU stream
framing, and the complete register file.

**One outstanding bug** blocks correct output: the decode is
**non-deterministic** — the same frame from fresh state decodes correctly about
half the time and otherwise lands in one of a few discrete wrong (or silent)
states, always with a clean completion status. (The correct-rate is
**non-stationary**: in some sessions the vector lands ~half correct, in others it
is consistently wrong across many fresh loads — no driver change, a silicon
baseline drift. So a single reproduction run may show all-wrong.) We have triaged it exhaustively and
shown it lives **below the MMIO register interface**: every programmable input
matches MPP, the same silicon decodes the same stream correctly *and
deterministically* under MPP, and the non-determinism survives both an identical
register file and the removal of all CPU activity during the decode. This is a
hardware-execution-level question we cannot resolve from the driver side.

This repo is published **downstream-first**: working code plus a precise,
fully-triaged question for the people with the hardware documentation
(Collabora / the VDPU383 maintainers). See
[The decode-non-determinism bug](#the-decode-non-determinism-bug-the-open-question).

## Status

The driver is feature-complete and MPP-faithful on everything the V4L2 stack
controls; the single open defect is the non-deterministic output described above.
Triage is exhaustive and the conclusion is **terminal from the driver side**:

- **Root cause localised to coefficient reconstruction.** A quantiser sweep pins
  the defect to the dequantisation / inverse-transform stage: at maximum quant
  (≈zero residual) the decode is **bit-exact**, and the error appears and grows with
  residual energy. Intra prediction, the above-row context, and entropy/CDF are all
  correct — the driver is bit-exact for zero/low-residual AV1.
- **The full vendor sequence is replicated and the output is still wrong.** Every
  programmable input is byte-identical to MPP (GBL, CDF, register file, bitstream);
  the per-frame register read-back is identical across CORRECT/WRONG/SILENT runs;
  masking all link interrupts changes nothing; the continuous link/CCU submission
  ring engages faithfully; and the complete per-frame clock / IOMMU / reset / PM /
  warmup operation set has been forced and verified. None of it changes the result.
- **The decode starts correct and diverges mid-frame.** Frame 0's first 16 bytes are
  byte-exact to the dav1d reference, yet the frame's Y-MAE is ~90 — the hardware
  receives correct inputs, begins decoding correctly, and diverges during its own
  internal pass. The wrong output is metastable (a discrete wrong/silent state,
  pinned per module-load, re-rolled by a fresh load) but never becomes correct
  without the vendor stack.
- **Same wall as the sibling VP9 driver**
  ([`rkvdec-vdpu383-vp9`](https://github.com/SympleNZ/rkvdec-vdpu383-vp9)), reached
  independently — one hardware-internal issue across both VDPU383 codecs (VP9 fails
  deterministically, AV1 metastably).

The one un-run check is a register-*value* `rwmmio` diff, which needs a full
vendor-kernel rebuild and is very unlikely to surface anything new — the register
values are already proven byte-identical to MPP, and the core clk/IOMMU driver
register values are not our code.

---

## TL;DR

| Capability | State |
|---|---|
| Driver plumbing (probe, 4 controls, start/stop/run/done, codec reg) | ✅ complete |
| Global header (GBL) pack — full AV1 frame header (~2900 bits) | ✅ **byte-identical to MPP** |
| Default CDF tables | ✅ **byte-identical to MPP** |
| OBU stream framing fed to HW (TD + SEQ_HDR + FRAME + body) | ✅ **byte-identical to MPP** |
| Register file (ctrl / codec-params / address regs) | ✅ matches MPP (only addresses + benign fields differ) |
| Decode completion (silent-completion watchdog) | ✅ reliable — every frame completes, no hangs/faults |
| References / DPB / colmv, tiles, KEY + INTER frame types | ✅ implemented |
| **Output correctness** | ❌ **non-deterministic — the open bug (correct ~half the time in good runs; rate is non-stationary)** |
| 10-bit output (P010) | ❌ HW downscales 10→8; P010 not wired (V2) |
| Film-grain synthesis (params packed; FGS apply path) | ⚠️ unvalidated (gated behind the core bug) |
| Mid-stream resolution change (V4L2 `source_change`) | ❌ not handled (V2) |

The driver is, in effect, **feature-complete and MPP-faithful on everything the
V4L2 stack controls — but produces unreliable output because one defect below the
register interface makes the decode non-deterministic.**

---

## The decode-non-determinism bug (the open question)

**Symptom.** Decoding the all-intra conformance vector
`av1-1-b8-02-allintra_20201006.ivf` (352×288, 4:2:0 8-bit, a single KEY frame)
**repeatedly from fresh state**, the same frame settles into one of a few discrete
outcomes: **CORRECT** (~half the runs), **WRONG** (a discrete wrong reconstruction,
clean DEC_RDY, no error bits), or **SILENT** (the HW self-clears `dec_en` and
writes no adapted CDF). Same input bytes + clean completion → a coin-flip between
several discrete states. The correct-rate is also **non-stationary across longer
timescales** — the vector decodes ~half correct in some sessions and is
consistently wrong across many fresh loads in others, with no driver change (a
silicon baseline drift; thermal / uptime the suspects). Within any one
module-load the outcome is fixed (see "pinned per module-load" below). When wrong,
the spatial manifestation varies (a degraded
reconstruction; the old "top band correct, lower rows flat DC" was one such
manifestation, not a fixed signature). It is not "HW stops early" — a sentinel
prefill of the output buffer is fully overwritten, so the HW writes a complete
frame; the reconstruction is what varies.

**What makes this a hardware-level question:** every input we can program or feed
the IP **matches the known-good MPP backend** (byte-level BSP cross-dumps), and MPP
decodes the clip **39/39 frames byte-identical to the dav1d reference,
deterministically, on the same silicon** — yet our V4L2 path is non-deterministic.

| Above-MMIO input | vs MPP | how verified |
|---|---|---|
| Global header (GBL) | byte-identical | dump diff |
| Default CDF tables | byte-identical | dump diff |
| Control + codec-param registers (reg8–106) | identical (bar `reg13` timeout) | `regs_full.dat` vs HW readback |
| Address registers | equivalent (MPP fd+offset vs our resolved IOVA) | — |
| RCB slot sizes / layout (incl. slot-4 intra context) | over-allocated but benign | `vdpu383_av1d_rcb_calc` dump; A/B override = no change |
| Bitstream (TD + SEQ_HDR + FRAME + body) | byte-identical | OBU decode of `stream_in.dat` vs our `strm_scratch` |

Two on-hardware results put the bug **below everything we program and below any
CPU activity during the decode**:

- **Per-decode register read-back is byte-identical regardless of outcome** — the
  full control/parameter register file, CRC'd just before each kick, is the same
  on CORRECT, WRONG, and SILENT runs. Our programming is provably deterministic and
  is not the source; the CDF buffer address does not predict the outcome.
- **It survives masking all link interrupts** — running the decode with zero CPU↔HW
  MMIO between kick and reap leaves the outcome distribution unchanged. Nothing the
  driver does mid-decode drives it.
- **It is pinned per module-load, not per-decode** — deterministic within an
  `insmod`, re-rolled by a fresh load — and the **probe warmup is not the cause**
  (disabling it gives the same wrong distribution). So the latched state is set at
  device init by something other than the warmup.

We also **refuted** the RCB lever in both dimensions: enlarging the RCB 2–3×
(Detlev's "too-small RCB → random results" suggestion) gives **byte-identical**
output, and SRAM-vs-DRAM placement is byte-identical too — bracketing the
intra-above-row slot across a ~15× size range with no effect. We also **probed
the submission model**: the vendor stack runs AV1 through the link/CCU descriptor
path (zero per-frame register MMIO) while we run single-shot direct-MMIO, so we
implemented link/CCU submission for AV1 and A/B-tested it — link mode lands on the
**same per-load wrong attractors** as single-shot. (**Refuted, 2026-06-23.** A live
MPP MMIO trace on the BSP board revealed the missing piece: BSP enables the link
IRQ once at probe, and our link path had left it *disabled* — so it completed
silently with no writeback. Re-enabling it made the writeback appear and the link
path complete cleanly, and the re-run A/B *still* lands on the same per-load
attractors. So the submission model is **not** the cause — it reaches the identical
metastable states either way. Continuous link mode remains valuable as a
*throughput* feature, not a correctness fix.) Full triage in
[`docs/NONDETERMINISM_BUG.md`](docs/NONDETERMINISM_BUG.md).

**Conclusion.** This is the same class as the other VDPU383 V4L2 findings — the
H.264 deblock race (**fixed**, by a power-up warmup decode at probe — an init-time
prime invisible to per-frame diffs) and the VP9 small-footprint reference bypass
(sibling repo) — *registers/headers/CDF/stream all match MPP, the hardware differs
anyway.* The remaining variable is an **un-primed / metastable internal decoder
state** sampled at decode start. We initially supposed the vendor *submit path*
pinned it — but the submit path has since been replicated (the continuous link/CCU
ring engages faithfully), along with the full per-frame clock / IOMMU / reset / PM /
warmup operation set, and the result is unchanged. So the state is established below
even the submit path and the per-frame operation set — at device/session init or in
the IP itself — beyond the V4L2-reproducible surface.

**The question for someone with the VDPU383 docs:** what does the vendor stack do —
at device/session init (`mpp_service`) or inside the IP — that pins the VDPU383's
internal entropy/context state so an AV1 KEY frame decodes deterministically and
correctly, given the per-frame register file, GBL, CDF, and bitstream are all
byte-identical, the submission ring and the full clock/IOMMU/PM/warmup operation set
are replicated, the result is independent of our register programming and all
mid-decode CPU activity, and the decode demonstrably *starts* correct (frame 0's
first 16 bytes byte-exact) and diverges mid-frame? Even a "look at X" would unblock
it.

---

## What works (in detail)

Everything a V4L2 stateless AV1 driver must do is implemented:

- **Codec registration & format:** `AV1F` fourcc on `/dev/video-dec0`;
  `adjust_fmt` / `start` / `stop` / `run` / `done` ops all wired.
- **Full control surface:** `V4L2_CID_STATELESS_AV1_SEQUENCE`, `_FRAME`,
  `_TILE_GROUP_ENTRY`, `_FILM_GRAIN`.
- **Global-header pack:** the complete AV1 uncompressed frame header (≈2900 bits,
  through the film-grain section) — CDEF, loop filter, loop restoration,
  segmentation, global motion / warp, superres, delta-q / delta-lf, tx-mode,
  skip-mode, order hints, intrabc, screen-content, palette/filter-intra/compound
  flags, colour config. **Byte-identical to MPP** for the test frame.
- **Default CDF + per-DPB-slot CDF/segid management:** byte-identical to MPP.
- **References / DPB:** slot-indexed reference base/stride programming, DPB
  tracking with per-slot order hints, V4L2 `reference_frame_ts` lookup,
  collocated-MV (TMVP) buffers.
- **OBU framing:** synthesises `[TD][OBU_FRAME][LEB][body]` into a scratch DMA
  buffer (V4L2 passes the FRAME OBU body); sequence info supplied via the GBL.
- **Tiles:** multi-tile frame-header-size handling; tile-info override for a
  gst single-tile parser quirk.
- **Decode completion:** silent-completion watchdog (the VDPU383 finishes the
  decode and self-clears `DEC_ENABLE` but does not always raise the IRQ). With
  this, AV1 completes **reliably** — no hangs, no IOMMU faults.

### Note: a watchdog NULL-deref fix is required to decode at all

`rkvdec_watchdog_func` in this tree carries VP9 link-mode silent-completion
telemetry that dereferences `ctx->link_table`. In **single-shot** mode (which AV1
uses) `link_table` is NULL, so the watchdog worker oopses on the first decode and
the stream hangs in D-state. The fix (NULL-guard; single-shot
`irq_sta==0 && dec_en==0` ⇒ genuine silent completion) is included here and is
codec-general — it also protects single-shot VP9/HEVC/H.264 in this tree. (This
is a property of *this downstream tree's* VP9 additions, not of mainline rkvdec.)

---

## Hardware & software

- **SoC:** Rockchip RK3576, VDPU383 video decoder IP.
- **Reference board:** NanoPi R76S (also ArmSoM Sige5 — same RK3576).
- **Kernel:** mainline-based 7.0.x (validated on Armbian
  `7.0.1-edge-rockchip64`). Out-of-tree module (`rockchip-vdec.ko`).
- **Userspace (GStreamer path):** `gst-plugins-bad` ≥ 1.28 (`v4l2slav1dec`).

Built on the VDPU383 H.264/H.265 support merged to mainline Linux in early 2026
(Collabora, Detlev Casanova; `rkvdec` de-staged). **VP9 and AV1 for VDPU383 are
not upstream** — AV1-on-RK3576 is on the Collabora roadmap as future work. This is
independent development on top of that mainline infrastructure and the V4L2 AV1
stateless uAPI.

---

## Build, deploy, test

```sh
make -C /lib/modules/$(uname -r)/build M=$PWD/src modules
#   KBUILD_MODPOST_WARN=1 silences the expected unresolved-symbol warnings for an
#   out-of-tree module that references vmlinux symbols.

sudo rmmod rockchip_vdec 2>/dev/null
sudo insmod src/rockchip-vdec.ko
v4l2-ctl -d /dev/video-dec0 --list-formats-out   # expect AV1F (and S264/S265)
```

Decode the all-intra conformance vector **several times from fresh state** and
compare each output to the dav1d reference (and to each other):

```sh
gst-launch-1.0 -q filesrc location=av1-1-b8-02-allintra_20201006.ivf ! \
    ivfparse ! av1parse ! v4l2slav1dec ! videoconvert ! \
    video/x-raw,format=I420 ! filesink location=out.yuv
# Frame 0 (352x288): correct on ~half the runs; otherwise a discrete wrong or
# silent result — the output varies run-to-run (the non-determinism).
```

Full reproduction, the MPP cross-dump procedure, and the triage scripts are in
[`docs/BUILD_AND_TEST.md`](docs/BUILD_AND_TEST.md) and
[`docs/NONDETERMINISM_BUG.md`](docs/NONDETERMINISM_BUG.md).

---

## Credits

Built on **Detlev Casanova / Collabora's** mainline VDPU383 H.264/H.265 `rkvdec`
work and the V4L2 stateless AV1 uAPI. The vendor **MPP** library was the
ground-truth reference throughout. Thanks for the foundation — it made this
possible.

Simon Wright, Symple Solutions, Dunedin NZ.
MIT-licensed (see [LICENSE](LICENSE)).
