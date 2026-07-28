# Environment

The system these workarounds were built and verified against. Copies of the
relevant configuration live in [`environment/`](environment/).

## Hardware

MacBook Air M2, Apple M2 (G14G B0), 10 GPU cores, 16 GiB RAM, Gentoo on Asahi.

Two CPU properties matter for the GPU workaround, both read from EL0 —
see [`environment/cpu-id-registers.txt`](environment/cpu-id-registers.txt):

| Feature | Value | |
|---|---|---|
| `ID_AA64MMFR2_EL1.FWB` | 0 | FEAT_S2FWB absent |
| `CTR_EL0.DIC` | 0 | absent |

KVM only maps cacheable `VM_PFNMAP` memory when both are present. They are not,
so such mappings can never be faulted in as cacheable on this machine.

## Versions

Full list: [`environment/package-versions.txt`](environment/package-versions.txt)

| | |
|---|---|
| kernel | 7.1.5-p2-asahi-dist, 16 KiB pages |
| guest kernel | 6.12.87 (libkrunfw), 4 KiB pages |
| muvm | 0.6.0 |
| libkrun / libkrunfw | 1.18.0 / 5.4.0 |
| virglrenderer | 1.2.0 |
| Mesa | 26.0.8 |
| FEX | 2607, rootfs `fex-rootfs-gentoo-20260401` |
| dbus | 1.16.2 |
| passt | 2025.12.15 |

The 16 KiB host vs. 4 KiB guest page size is why muvm is needed at all: FEX
requires 4 KiB pages.

## Kernel

[`environment/kernel-config.txt`](environment/kernel-config.txt) — the KVM,
dma-buf, virtio and `DRM_ASAHI` switches. Nothing here needs changing; the
options the workarounds depend on are all present:

```
CONFIG_KVM=y
CONFIG_DMA_SHARED_BUFFER=y
CONFIG_DRM_ASAHI=m
```

## Steam launcher

[`environment/usr-bin-steam-aarch64`](environment/usr-bin-steam-aarch64) —
the distribution's launcher. Its last line is why the project ships its own
wrapper instead of setting an environment variable:

```sh
exec muvm /usr/bin/steam-muvm "${@}"
```

The muvm invocation is fixed and trailing arguments go to Steam, so `-x` cannot
be passed through it. [`environment/usr-bin-steam-muvm`](environment/usr-bin-steam-muvm)
is the inner script that starts FEX.

## FEX

[`environment/fex-Config.json`](environment/fex-Config.json) — note that every
thunk is disabled:

```json
"ThunksDB": { "cuda": 0, "fex_thunk_test": 0, "asound": 0,
              "drm": 0, "Vulkan": 0, "WaylandClient": 0, "GL": 0 }
```

[`environment/fex-ThunksDB.json`](environment/fex-ThunksDB.json) shows what the
`GL` entry would do when enabled: overlay `libGL-guest.so` onto `/usr/lib/libGL.so.1`
inside the x86 rootfs, forwarding GL calls to the guest's native driver.

This matters for reading the verification results correctly. `make check` runs
`eglinfo` as a native aarch64 binary, so it measures the guest's own GL stack —
which is what the dma-buf workaround restores. Whether x86 programs under FEX
reach the GPU depends on these thunks, which is a separate concern from either
workaround here.

## Portage

- [`environment/portage-package.accept_keywords-steam`](environment/portage-package.accept_keywords-steam)
  — the `~arm64` unmasks for the whole Steam stack, most from the `asahi` overlay
- [`environment/portage-package.use-virglrenderer`](environment/portage-package.use-virglrenderer)
  — `venus` USE flag, required for `--gpu-mode=venus`

`VIDEO_CARDS="asahi virgl zink"` in `make.conf`.

## Guest network

Set up by passt, not by NetworkManager. Inside the guest:

```
eth0   UP   192.168.2.23/24   2003:e3:4f24:4200:5894:efff:fee4:cee/64
/etc/resolv.conf: nameserver 192.168.2.1
```

The host's `/etc/resolv.conf` must point at a real nameserver. A
`systemd-resolved` stub address such as `127.0.0.53` is not reachable from the
guest and would break DNS there — on this system NetworkManager writes the
router directly, so the situation does not arise.

## Regenerating

The files under `environment/` are copies taken by hand. To refresh them:

```sh
cp /usr/bin/steam-aarch64 environment/usr-bin-steam-aarch64
cp /usr/bin/steam-muvm    environment/usr-bin-steam-muvm
python3 -m json.tool /usr/share/fex-emu/ThunksDB.json > environment/fex-ThunksDB.json
python3 -m json.tool ~/.config/fex-emu/Config.json    > environment/fex-Config.json
cp /etc/portage/package.accept_keywords/steam environment/portage-package.accept_keywords-steam
cp /etc/portage/package.use/virglrenderer     environment/portage-package.use-virglrenderer
zcat /proc/config.gz | grep -E '^CONFIG_(KVM|DMA_SHARED_BUFFER|UDMABUF|DRM_ASAHI|VIRTIO_)' \
    | sort > environment/kernel-config.txt
```
