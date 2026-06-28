# rkvdec-vdpu383-av1 — V4L2 stateless AV1 decoder for Rockchip RK3576 (VDPU383)

A mainline-Linux **V4L2 stateless AV1 decoder** for the Rockchip **RK3576** SoC's
**VDPU383** video IP, built on top of the now-mainline VDPU383 H.264/H.265
`rkvdec` infrastructure and the V4L2 stateless AV1 uAPI. This is research /
reference work — the first mainline attempt at AV1 on this silicon.

Everything the V4L2 software side controls is implemented and **byte-identical to
the vendor library (MPP)**: sequence/frame/tile/film-grain controls, the full
global-header (GBL) pack, the default CDF probability tables, reference/DPB
management, RCB/SRAM allocation, OBU stream framing, the complete register file,
and both the single-shot and link-descriptor submission paths.

**It does not, however, fully correctly decode AV1.** The honest headline, proven
exhaustively below:

> **The AV1 decode failure is a driver bug, not silicon.** The same mainline 7.0
> kernel, device tree and board that our V4L2 driver gets wrong, the vendor MPP
> decoder — built as an out-of-tree module — gets **bit-exact right** (39 frames,
> matching MPP-on-BSP and the dav1d reference). Yet the residual defect sits
> **below every interface the mainline V4L2 stateless driver controls**: every
> input the driver builds is byte-identical to MPP, and the bug survives every
> register, buffer, reset, clock and submission-path change we can make. The
> silicon is capable; this driver cannot reach the part of it that matters.

This repo is published **downstream-first**: complete, evidence-bounded code plus
a precisely characterised question for the people with the VDPU383 hardware
documentation (Rockchip / Collabora). Full triage in
[`docs/NONDETERMINISM_BUG.md`](docs/NONDETERMINISM_BUG.md).

---

## Status at a glance

| Capability | State |
|---|---|
| Driver plumbing (probe, 4 controls, start/stop/run/done, codec reg) | ✅ complete |
| Global header (GBL) pack — full AV1 frame header (~2900 bits) | ✅ **byte-identical to MPP** |
| Default CDF probability tables | ✅ **byte-identical to MPP** |
| OBU stream framing fed to HW (TD + SEQ_HDR + FRAME + body) | ✅ **byte-identical to MPP** |
| Register file (ctrl / codec-params / address regs) | ✅ **byte-equivalent to MPP** (only IOVAs + benign fields differ) |
| RCB / SRAM allocation | ✅ implemented; geometry exonerated as a cause |
| References / DPB / colmv, tiles, KEY + INTER frame types | ✅ implemented |
| Decode completion (silent-completion watchdog) | ✅ reliable — every frame completes, no hangs/faults |
| Single-shot **and** link-descriptor submission | ✅ both implemented |
| **CDEF secondary-strength un-remap** | ✅ **fixed (default-on)** — see below |
| **Output correctness** | ❌ **wrong — the residual driver bug, below the V4L2 interface** |
| 10-bit output (P010) | ❌ HW downscales 10→8; P010 not wired (future) |
| Film-grain synthesis (params packed; FGS apply path) | ⚠️ unvalidated (gated behind the core bug) |
| Mid-stream resolution change (V4L2 `source_change`) | ❌ not handled (future) |

The driver is **feature-complete and MPP-faithful on everything the V4L2 stack
controls** — and still produces wrong output, because one defect lives below that
stack.

---

## The investigation, and why the conclusion is airtight

The decode of even the simplest possible frame — the all-intra conformance vector
`av1-1-b8-02-allintra_20201006.ivf` (352×288, 4:2:0 8-bit, a single
fully-independent KEY frame) — is wrong. The reconstruction first diverges at the
AV1 superblock boundary (byte 128 of row 0): superblock 0 decodes correctly from
the default CDF, then the entropy/symbol decode desynchronises and everything
after cascades. With a soft IP reset before each decode the wrong output is
deterministic; without it, it is non-deterministic (a small set of discrete wrong
attractors carried as HW state between decodes).

### 1. It is a driver bug, not silicon (the decisive measurement)

We built the vendor **MPP** decoder as an out-of-tree module (`rk_vcodec`) and ran
it on the **same mainline 7.0.1 kernel, the same mainline device tree, the same
mainline platform (clocks / power / IOMMU), and the same silicon** our V4L2 driver
uses — vendor `mpi_dec_test` + unmodified `librockchip_mpp.so.1`:

| Path | AV1 allintra (39 frames) full md5 | runs | determinism |
|---|---|---|---|
| MPP-on-BSP (6.1 vendor kernel, golden) | `ca5876b5e61e2a37bb53` | 4 | deterministic |
| **MPP-on-mainline** (7.0.1, OOT `rk_vcodec`) | `ca5876b5e61e2a37bb53` | 3 | deterministic |
| Independent software (ffmpeg `libdav1d`, NV12) | `ca5876b5e61e2a37bb53` | — | spec reference |
| **Our V4L2 driver** (7.0.1, same board) | wrong / coin-flip | many | non-deterministic |

MPP-on-mainline decodes AV1 **bit-exactly correct**, matching both MPP-on-BSP and
the independent dav1d software reference. Our V4L2 driver, on the identical kernel
and silicon, is wrong. This **overturned an earlier "below-MMIO silicon"
conclusion**: the platform is not the cause (MPP gets it right on it), and the
silicon is not the cause (MPP gets it right through it). The coin-flip we had
attributed to hardware metastability is a property of our driver — MPP is
deterministic on the same silicon, zero variance across runs.

### 2. The bug is narrowed below the V4L2 interface (every input proven, every lever tested)

We then drove the bug to the limit on the same board, dumping MPP's golden inputs
per frame and diffing ours byte-for-byte. **Every static input the driver builds
is byte-identical to MPP:**

| Input the HW reads | vs MPP | how verified |
|---|---|---|
| Default CDF probability tables | byte-identical | dump diff (29600 B) |
| Coeff-CDF offset / layout | identical | reg178 base + 0x1b20 in both |
| Global header (GBL, 672 B) | byte-identical (after the CDEF fix) | dump diff |
| Register file (reg8–235, ctrl + params) | byte-equivalent | same-board readback + descriptor diff |
| OBU stream (TD + SEQ_HDR + FRAME + body) | byte-identical | parse of MPP `stream_in.dat` |
| RCB region sizes/layout | exonerated | exact MPP proportions A/B = no change |

**No input is the cause.** And every operational lever we could identify and
toggle was tested same-board and came back negative — matching MPP's values or
behaviour does **not** fix it:

- **Submission model is excluded.** The correct vendor path is the link-descriptor
  flow (the HW DMA-reads ~250 decode registers from a DDR descriptor; MPP writes
  only 7 registers to live MMIO). We implemented the link descriptor and drove the
  continuous ring via a deterministic V4L2 client — **38 append events, ring
  genuinely engaged, frames pipelining** — and the decode is still wrong. Both
  single-shot register tuning and the link/ring path land on the same wrong
  attractor. Single-shot is a confirmed dead end for this codec.
- **Resets / seeding excluded.** IP soft-reset (`ip_reset`, which collapses the
  variance to a deterministic baseline), full CRU reset including the CABAC /
  entropy block, and the captured MPP probe-time priming pulse — all negative. No
  reset we perform clears the wrong decode; the wrong state is *generated by the
  run*, not left stale.
- **Platform knobs excluded.** `pmu_idle` (not required for correct AV1), the
  RK3576 H.264 warmup (an H.264-specific fix), per-decode IOMMU TLB flush, buffer
  coherency (every buffer the HW reads is `dma_alloc_coherent`), the cache window,
  and the live decoder clock rates (our ~594/1000 MHz already match MPP's
  DT-assigned operating rates; a further pin to 200/297 MHz changed nothing) — all
  byte-identical-to-MPP or A/B-negative.
- **The maximal faithful combination is negative.** Per the rule that an
  MPP-faithful behaviour stays on even when individually negative (so the cure can
  emerge from the combination), we stacked every faithful lever together. Still
  wrong — and *more* variable, proving our levers are approximations of MPP's
  operations, not exact reproductions.
- **A continuous 39-frame session does not converge.** 0/39 correct, cycling
  through only five distinct wrong outputs — it never "warms up" into a correct
  decode the way the single-MPP-session hypothesis would predict.

### 3. The CDF entropy decode itself diverges

On the deterministic baseline, the CDF the HW *reads* is byte-identical to MPP,
but the adapted CDF the HW *writes back* is only 67.8% identical — diverging from
the very first context either side adapts (byte 7). The hardware symbol decoder
produces different symbols than MPP's from the **same** CDF input, same bitstream,
same registers, same clean reset, on the **same silicon**. That is a true
entropy-decode divergence with every driver-provided input verified correct.

### 4. The residual

Combining all of the above: from a clean reset, with literally every byte the HW
reads verified byte-identical to MPP, and every register/buffer/reset/clock/
submission lever matched or A/B-ruled-out same-board, the decode is
deterministically wrong while MPP on the identical silicon is correct.

**The residual is below every interface the mainline V4L2 stateless driver
controls** — it is sub-MMIO operation ordering/timing relative to power/clock/PM
transitions, invisible to register and buffer dumps, that the vendor driver's
structure produces and our re-implementation does not. It is driver-addressable in
principle (the silicon is capable — MPP proves it), but **not fixable through any
interface this driver has**. Resolving it cleanly would need cycle/bus-level AXI
tracing we do not have on this board, or the VDPU383 documentation.

The sibling [`rkvdec-vdpu383-vp9`](https://github.com/SympleNZ/rkvdec-vdpu383-vp9)
driver reached the same wall independently — one driver-reachable class of defect
across both VDPU383 V4L2 codecs.

### The one correctness fix this work produced: the CDEF un-remap bug

In narrowing the GBL header to byte-identity with MPP, we found and fixed a real
bug. AV1 §5.9.19 remaps a CDEF secondary strength of 3 → 4; the V4L2 control
stores the post-remap value (4), and our packer applied a plain `& 0x3` mask,
turning 4 into **0**. Un-remapping 4 → 3 before the 2-bit mask (for both
`y_sec_strength` and `uv_sec_strength`) makes the packed GBL byte-identical to
MPP. This is a genuine correctness fix for CDEF on streams that use a secondary
strength of 4, and it is **shipped default-on** (CDEF is AV1-only; HEVC/VP9/H.264
are unaffected). It does **not** fix the entropy-decode bug — CDEF is a
post-reconstruction filter, applied after the desync occurs.

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
  skip-mode, order hints, intrabc, screen-content, palette / filter-intra /
  compound flags, colour config. **Byte-identical to MPP.**
- **Default CDF + per-DPB-slot CDF/segid management:** byte-identical to MPP.
- **References / DPB:** slot-indexed reference base/stride programming, DPB
  tracking with per-slot order hints, V4L2 `reference_frame_ts` lookup,
  collocated-MV (TMVP) buffers.
- **OBU framing:** synthesises `[TD][OBU_FRAME][LEB][body]` into a scratch DMA
  buffer (V4L2 passes the FRAME OBU body); sequence info supplied via the GBL.
- **Tiles:** multi-tile frame-header-size handling; tile-info override for a
  GStreamer single-tile parser quirk.
- **Decode completion:** a silent-completion watchdog (the VDPU383 finishes the
  decode and self-clears `DEC_ENABLE` but does not always raise the IRQ). With it,
  AV1 completes **reliably** — no hangs, no IOMMU faults.

### A watchdog NULL-deref fix is required to decode at all

`rkvdec_watchdog_func` in this tree carries VP9 link-mode silent-completion
telemetry that dereferences `ctx->link_table`. In **single-shot** mode (which AV1
uses by default) `link_table` is NULL, so the watchdog worker oopses on the first
decode and the stream hangs in D-state. The fix (NULL-guard; single-shot
`irq_sta==0 && dec_en==0` ⇒ genuine silent completion) is included here and is
codec-general — it also protects single-shot VP9/HEVC/H.264 in this tree. This is
a property of *this downstream tree's* VP9 additions, not of mainline rkvdec.

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

The `src/` tree is the full VDPU38x rkvdec driver (H.264 / H.265 / VP9 / AV1
backends for both VDPU381 and VDPU383, plus the shared CABAC, RCB, link and core
support); `rkvdec-vdpu383-av1.c` is the AV1 backend.

```sh
make -C /lib/modules/$(uname -r)/build M=$PWD/src modules
#   KBUILD_MODPOST_WARN=1 silences the expected unresolved-symbol warnings for an
#   out-of-tree module that references vmlinux symbols.

sudo rmmod rockchip_vdec 2>/dev/null
sudo insmod src/rockchip-vdec.ko
v4l2-ctl -d /dev/video-dec0 --list-formats-out   # expect AV1F (and S264/S265/VP9F)
```

Decode the all-intra conformance vector and compare the output to the dav1d
reference:

```sh
gst-launch-1.0 -q filesrc location=av1-1-b8-02-allintra_20201006.ivf ! \
    ivfparse ! av1parse ! v4l2slav1dec ! videoconvert ! \
    video/x-raw,format=I420 ! filesink location=out.yuv
# Frame 0 (352x288): decodes wrong — superblock 0 correct, then the entropy
# decode desyncs and cascades (the residual driver bug). Run-to-run it lands in a
# small set of discrete wrong states; a soft IP reset makes it deterministic.
```

Full reproduction, the MPP cross-dump procedure and the triage scripts are in
[`docs/BUILD_AND_TEST.md`](docs/BUILD_AND_TEST.md) and
[`docs/NONDETERMINISM_BUG.md`](docs/NONDETERMINISM_BUG.md).

---

## Disposition

AV1 endpoint decode is **not currently required** (the shipping cast path is HEVC;
YouTube playback uses yt-dlp + HLS software decode). Where correct AV1 decode is
needed on this silicon, the path is the **vendor MPP stack** — either on the BSP
kernel or as the out-of-tree build on mainline 7.0 proven correct here (published:
[`rkvdec-vdpu383-mpp-mainline`](https://github.com/SympleNZ/rkvdec-vdpu383-mpp-mainline);
licensing clean) — not this V4L2 driver.

This V4L2 AV1 work stands as a **complete, evidence-bounded characterisation**: the
first mainline-Linux V4L2 stateless AV1 attempt on the RK3576 VDPU383, the
infrastructure to drive it (controls, GBL pack, CDF tables, register/descriptor
construction, both submission paths), one real correctness fix (CDEF un-remap),
and the bug narrowed to the exact silicon boundary — a driver-reachable defect
that no interface this driver controls can reach. The active hunt is **paused, not
abandoned**: the deterministic harness, the `ip_reset` baseline, and the one cheap
re-entry point (a same-board MMU register-bank diff, MPP vs ours) are documented
for anyone — with the VDPU383 docs or cycle-level tracing — who wants to take it
further.

---

## Credits

Built on **Detlev Casanova / Collabora's** mainline VDPU383 H.264/H.265 `rkvdec`
work and the V4L2 stateless AV1 uAPI. The vendor **MPP** library was the
ground-truth reference throughout, and building it out-of-tree on mainline was what
turned the conclusion from "silicon" to "driver". Thanks for the foundation — it
made this possible.

Simon Wright, Symple Solutions, Dunedin NZ.

**GPL-2.0** (Linux kernel out-of-tree module), consistent with the mainline
Rockchip `rkvdec` driver it derives from. Source files carry
`SPDX-License-Identifier: GPL-2.0` headers (see [LICENSE](LICENSE)).
