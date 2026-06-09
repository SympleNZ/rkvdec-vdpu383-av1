# rkvdec-vdpu383-av1 — V4L2 stateless AV1 decoder for Rockchip RK3576 (VDPU383)

A **V4L2 stateless AV1 decoder** for the Rockchip **RK3576** SoC's **VDPU383**
video IP, built on top of the now-mainline VDPU383 H.264/H.265 `rkvdec`
infrastructure and the V4L2 stateless AV1 uAPI. Every part of the driver the
software side controls is implemented and **byte-identical to the vendor library
(MPP)** — sequence/frame/tile/film-grain controls, the full global-header pack,
the default CDF tables, reference/DPB management, RCB/SRAM allocation, OBU stream
framing, and the complete register file.

**One outstanding bug** blocks correct output: a **partial / regional decode** —
the top of each frame reconstructs correctly and the rest falls back to flat DC.
We have triaged it exhaustively and shown it lives **below the MMIO register
interface**: every programmable input matches MPP, the same silicon decodes the
same stream correctly under MPP, yet our V4L2 stack's output is partial. This is
a hardware-execution-level question we cannot resolve from the driver side.

This repo is published **downstream-first**: working code plus a precise,
fully-triaged question for the people with the hardware documentation
(Collabora / the VDPU383 maintainers). See
[The partial-decode bug](#the-partial-decode-bug-the-open-question).

> Status as of 2026-06-09. Independent development on the RK3576 VDPU383, sibling
> of [`rkvdec-vdpu383-vp9`](https://github.com/SympleNZ/rkvdec-vdpu383-vp9) — same
> SoC, same source tree, same downstream-first approach.

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
| **Output correctness** | ❌ **partial / regional decode — the open bug (every frame)** |
| 10-bit output (P010) | ❌ HW downscales 10→8; P010 not wired (V2) |
| Film-grain synthesis (params packed; FGS apply path) | ⚠️ unvalidated (gated behind the core bug) |
| Mid-stream resolution change (V4L2 `source_change`) | ❌ not handled (V2) |

The driver is, in effect, **feature-complete and MPP-faithful on everything the
V4L2 stack controls — but produces no usable output because one defect below the
register interface corrupts every frame.**

---

## The partial-decode bug (the open question)

> **Latest (2026-06-09, [`PARTIAL_DECODE_BUG.md` §8](docs/PARTIAL_DECODE_BUG.md)):** an IOMMU
> fault-probe confirms the HW **writes** the slot-4 intra above-row context (it is produced, not
> skipped). The surviving top portion scales with **content, not a fixed row count** — within
> one content it is a roughly fixed *fraction* across resolutions (Sintel ~65% at 360/720/1080),
> but across content it ranges from ~38% (all-intra vector) to **0% (Big Buck Bunny 1080p/4K,
> fully blank)** — ruling out a simple fixed-size "caps N rows" buffer and pointing at a
> content-driven internal-state exhaustion. Still below the MMIO interface.

**Symptom.** Decoding the all-intra conformance vector
`av1-1-b8-02-allintra_20201006.ivf` (352×288, 4:2:0 8-bit), the **top ~38 % of
rows reconstruct correctly** and the remainder is flat AV1 **DC intra-prediction
fallback** (a near-constant fill). PSNR ≈ 11 dB; 0/13 Chromium 8-bit conformance
vectors are bit-exact. KEY and INTER frames show the identical pattern. It is not
"HW stops early" — prefilling the output buffer and observing >99.9 % overwrite
shows HW writes the whole frame, but the **intra above-row prediction context for
super-block row ≥ 1 is wrong**, so lower rows predict from garbage and collapse
to DC.

**What makes this a hardware-level question:** every input we can program or feed
the IP now **matches the known-good MPP backend**, verified by byte-level
cross-dumps on the BSP, yet the same silicon produces a partial frame under our
V4L2 driver while decoding the vector correctly under MPP:

| Above-MMIO input | vs MPP | how verified |
|---|---|---|
| Global header (GBL) | byte-identical | dump diff |
| Default CDF tables | byte-identical | dump diff |
| Control + codec-param registers (reg8–106) | identical (bar `reg13` timeout) | `regs_full.dat` vs HW readback |
| Address registers | equivalent (MPP fd+offset vs our resolved IOVA) | — |
| RCB slot sizes / layout (incl. slot-4 intra context) | over-allocated but benign | `vdpu383_av1d_rcb_calc` dump; A/B override = no change |
| Bitstream (TD + SEQ_HDR + FRAME + body) | byte-identical | OBU decode of `stream_in.dat` vs our `strm_scratch` |

We also falsified the previously-leading theory that the RCB **slot-4** intra
above-row context buffer was the cause: a RAM redirect of slot 4 hangs the IP
(SRAM-only access path), and programming MPP's exact slot-4 length changes
nothing. The full triage — including the register diff, the RCB sizing analysis,
and the stream byte-comparison — is in
[`docs/PARTIAL_DECODE_BUG.md`](docs/PARTIAL_DECODE_BUG.md).

**Conclusion.** This is the same below-MMIO class as the VP9 compound-prediction
collapse (still open) and the H.264 deblock race — which we have since **fixed**
(an RK3576 power-up warmup at `pm_runtime` resume; 64/64 bit-exact), showing this
class can be unblocked from outside per-frame register programming:
*registers/headers/CDF/stream all match MPP, the hardware differs anyway.* The
remaining variable is the IP's internal intra above-row-context state machine, or
a HW-session/context setup MPP performs through its `mpp_service` kernel path that
has no V4L2 register equivalent — below the MMIO interface, where the driver has
no visibility.

**A lever was found below the register interface (RCB placement).** Reading the
Rockchip BSP kernel driver (`mpp_rkvdec2.c`) shows `mpp_set_rcbbuf()` **rewrites the
RCB base registers in the kernel at submit time** — pointing them at SRAM only when
the DT wires `rockchip,rcb-iova` and the frame is wider than `rcb_min_width`,
otherwise leaving RCB in **DRAM**. The BSP board's DT has no `rcb-iova`, so the
working MPP stack decodes with **DRAM RCB** — and because that rewrite happens in the
kernel *after* the HAL register dump, it is invisible to the `regs_full.dat` diff
above. Our mainline V4L2 driver always uses **SRAM** RCB. Forcing all-DRAM RCB in our
driver **changed the regional decode pattern** (a super-block row that was fill under
SRAM reconstructed under DRAM), with a first-decode-vs-subsequent state dependence.
So the failure is **sensitive to RCB intra-above-row buffer placement and state** —
i.e. partly in our control — though neither SRAM nor DRAM alone yields a correct
frame, consistent with a HW-internal above-row read-after-write path difference.

**The question for someone with the VDPU383 docs:** what is the RCB intra-above-row
buffer contract the vendor stack relies on (placement / coherency / the in-kernel
base rewrite) that arms the VDPU383 above-row-context reconstruction across
super-block rows, which a mainline V4L2 m2m client doesn't replicate? Even a
"look at X" would unblock it.

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

Decode the all-intra conformance vector and inspect the per-row signature:

```sh
gst-launch-1.0 -q filesrc location=av1-1-b8-02-allintra_20201006.ivf ! \
    ivfparse ! av1parse ! v4l2slav1dec ! videoconvert ! \
    video/x-raw,format=I420 ! filesink location=out.yuv
# Frame 0 (352x288): rows 0..~96 reconstruct; rows ~128+ are flat DC fill.
```

Full reproduction, the MPP cross-dump procedure, and the triage scripts are in
[`docs/BUILD_AND_TEST.md`](docs/BUILD_AND_TEST.md) and
[`docs/PARTIAL_DECODE_BUG.md`](docs/PARTIAL_DECODE_BUG.md).

---

## Credits

Built on **Detlev Casanova / Collabora's** mainline VDPU383 H.264/H.265 `rkvdec`
work and the V4L2 stateless AV1 uAPI. The vendor **MPP** library was the
ground-truth reference throughout. Thanks for the foundation — it made this
possible.

Simon Wright, Symple Solutions, Dunedin NZ.
MIT-licensed (see [LICENSE](LICENSE)).
