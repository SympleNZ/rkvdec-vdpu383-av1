# Build, deploy, reproduce

## Build (out-of-tree module)

Against your kernel headers/source (developed on Armbian `7.0.1-edge-rockchip64`,
RK3576):

```sh
KBUILD_MODPOST_WARN=1 make -C /lib/modules/$(uname -r)/build M=$PWD/src modules
#   KBUILD_MODPOST_WARN=1 turns the expected "unresolved symbol" notices for an
#   out-of-tree module (it references vmlinux exports) into warnings instead of
#   fatal modpost errors. A clean tree builds rockchip-vdec.ko (~6.5 MB w/ debug).

sudo rmmod rockchip_vdec 2>/dev/null
sudo insmod src/rockchip-vdec.ko
v4l2-ctl -d /dev/video-dec0 --list-formats-out   # expect AV1F (and S264/S265)
```

> The board may auto-load an installed `rockchip-vdec.ko` at boot; always `rmmod`
> then `insmod` this build to be sure you are testing it.

## Decode the all-intra conformance vector (several times)

```sh
for i in 1 2 3 4 5 6; do
  rmmod rockchip_vdec; insmod src/rockchip-vdec.ko   # fresh state each run
  gst-launch-1.0 -q \
    filesrc location=av1-1-b8-02-allintra_20201006.ivf ! ivfparse ! av1parse ! \
    v4l2slav1dec ! videoconvert ! video/x-raw,format=I420 ! filesink location=out$i.yuv
done
md5sum out*.yuv     # compare to each other and to the dav1d reference
```

The decode is **non-deterministic**: about half the runs are correct (byte-exact
to dav1d), the rest land in a discrete wrong or silent state. `gst` returns 0 and
writes a full-size YUV every time; the defect is in the content, not the framing,
and it varies run-to-run. (The dav1d reference: `ffmpeg -i
av1-1-b8-02-allintra_20201006.ivf -pix_fmt yuv420p ref.yuv`.)

## MPP ground-truth cross-dumps (on an RK3576 BSP board)

```sh
# Build MPP with the data-dump path enabled, then:
mkdir -p av1                                  # mkdir is non-recursive in the dump path
mpi_dec_test -i av1-1-b8-02-allintra_20201006.ivf -t 16777224   # 16777224 = MPP_VIDEO_CodingAV1
# Per-frame dumps land in ./av1/Frame0000/:
#   global_cfg.dat  cdf_rd_def.dat  regs_full.dat  stream_in.dat  colmv_cur_frame.dat
```

Dump format for `*.dat` (the `vdpu38x_dump_data_to_file` text-hex): per byte the
low nibble is written first then the high nibble; each 16-byte line is reversed
(little-endian). `regs_full.dat` is the raw `Vdpu383RegSet` struct (ctrl_regs
@reg8, comm_paras @reg64, comm_addrs @reg128). The triage scripts used to decode
and diff these (`decode_stream.py`, the register-mapping C helper, the per-row Y
scanner) accompany the development notes for this driver.

## What to look at

The actionable evidence is in [`NONDETERMINISM_BUG.md`](NONDETERMINISM_BUG.md):
GBL, CDF, the register file, and the bitstream are all byte-identical to MPP; the
per-decode register read-back is identical regardless of outcome; the bug survives
masking all interrupts; and MPP decodes the clip 39/39 byte-exact and
deterministically on the same silicon — so the divergence is below the MMIO
interface.
