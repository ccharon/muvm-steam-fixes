# muvm-steam-fixes, turned out it was a regression in kernel 7.1.5, **fixed in kernel 7.1.6** keeping it just for reference

Two workarounds that make Steam usable under [muvm](https://github.com/AsahiLinux/muvm)
on Apple Silicon. They are independent and address different bugs.

| # | Symptom | Cause |
|---|---|---|
| 1 | The VM dies the moment the guest touches the GPU | `KVM_PFN_ERR_RO_FAULT` on dma-buf mappings |
| 2 | Steam sits on "Waiting for network" forever | no system D-Bus in the guest |

With both in place Steam runs with hardware-accelerated OpenGL:

```
OpenGL core profile renderer: Apple M2 (G14G B0)
OpenGL core profile version:  4.6 (Core Profile) Mesa 26.0.8
```

## 1 — GPU: `muvm-dmabuf-fixup.so`

muvm runs Steam inside a libkrun VM because FEX needs a 4 KiB page size while
Apple Silicon runs with 16 KiB. For GPU acceleration libkrun maps graphics
buffers from the host into the guest. Any attempt to use the GPU ends
immediately in:

```
ERROR krun_vmm::linux::vstate] Failure during vcpu run: Bad address (os error 14)
```

libkrun places these buffers into the VM's shared memory window as **dma-bufs**
via `mmap(MAP_FIXED)`. Such regions are `VM_PFNMAP` — they have no `struct page`
and are not managed like ordinary memory:

```
fff854810000-fff854814000 rw-s 105cc8000 00:0c 99   /dmabuf:
VmFlags: rd wr sh mr mw ms pf de dd
                          ^^ VM_PFNMAP
```

When the guest **writes** to such a page, KVM has to map it in and takes the
`hva_to_pfn_remapped()` path. Finding no writable PTE there, it returns
`KVM_PFN_ERR_RO_FAULT`; `user_mem_abort()` then fails with `-EFAULT` and
`KVM_RUN` takes the VM down with it. KVM cannot fault in `VM_PFNMAP` memory on
its own — the VMM has to do it, and libkrun does not. This is a libkrun bug,
not a kernel bug and not a misconfiguration. Buffers backed by a `memfd` are
unaffected; they have ordinary VMA flags.

**Workaround:** the library interposes `mmap` via `LD_PRELOAD` and touches every
page of a freshly mapped dma-buf exactly once with a read-modify-write. The same
value is written back, so contents are unchanged — but afterwards the PTE exists
as writable before the guest ever reaches the page.

The interposer only acts on `MAP_FIXED` **and** `MAP_SHARED` **and**
`PROT_READ|PROT_WRITE` **and** a file descriptor pointing at `/dmabuf:`.
Everything else is left alone, in particular the loader's read-only `MAP_FIXED`
mappings, where a write would fault immediately.

Works in both GPU modes (`--gpu-mode=drm` and `=venus`). muvm defaults to `drm`,
so no argument is needed.

## 2 — Network: `muvm-guest-dbus.sh`

Steam stops on its loading screen at "Waiting for network" even though the
network is fine. Steam's own probes in
`~/.local/share/Steam/logs/connection_log.txt` all pass:

```
Connectivity test (23.194.190.235:80): OK!
IPv6 HTTP connectivity test - SUCCESS
IPv6 UDP connectivity test (port 27019) - SUCCESS
```

Steam does not derive network *state* from such probes, though — it reads it
from NetworkManager over D-Bus. The muvm guest has its own `/run` and therefore
no system bus. (Passing the host's socket through virtiofs is not an option;
you cannot connect to a socket that way.) The consequence shows up in
`cef_log.txt`:

```
"SystemNetworkStore - ERROR TypeError:
 SteamClient.System.Network.RegisterForDeviceChanges is not a function"
```

The client never registers its network interface, the UI's JavaScript throws,
the SystemNetworkStore stays empty, and the "Waiting for network" placeholder
never goes away. The client itself is long done and reports
`SetLoginState: WaitingForCredentials - OK`.

**Workaround:** start a system D-Bus inside the guest through muvm's `-x` hook,
which runs before the guest server while still root. A bare bus is sufficient;
NetworkManager does not need to run — which is just as well, since it would try
to take over `eth0`, whose configuration inside the guest comes from passt.

## Usage

```sh
make
make install
```

This installs four files under `~/.local`:

| File | Purpose |
|---|---|
| `lib/muvm-dmabuf-fixup.so` | the GPU library |
| `lib/muvm-guest-dbus.sh` | starts the system bus in the guest |
| `bin/steam-aarch64` | wrapper setting `LD_PRELOAD` and `muvm -x` |
| `share/applications/steam.desktop` | menu entry pointing at the wrapper |

The `.desktop` entry is needed because the shipped one invokes
`/usr/bin/steam-aarch64` by absolute path and would bypass the wrapper in
`PATH`. The wrapper calls `muvm` directly, because `steam-aarch64` hardcodes its
muvm invocation and forwards trailing arguments to Steam — `-x` would never
reach muvm.

Then start Steam as usual, from the menu or via `steam-aarch64`. This requires
`~/.local/bin` to precede `/usr/bin` in `PATH`:

```sh
command -v steam-aarch64      # must print ~/.local/bin/steam-aarch64
```

`make install` refuses to run while a muvm VM is up, since the library is mapped
in that process.

Self-test for part 1, without Steam and without a running X server:

```sh
make check
```

Runs muvm, has the guest execute `eglinfo`, and reports success only if no
`EFAULT` occurred and a real GPU renderer was reported.

For other programs the pieces work on their own:

```sh
LD_PRELOAD=~/.local/lib/muvm-dmabuf-fixup.so \
    muvm -x ~/.local/lib/muvm-guest-dbus.sh --gpu-mode=drm <command>
```

`LD_PRELOAD` is **not** propagated into the guest — muvm forwards only a fixed
set of environment variables, so the x86 processes under FEX never see the
aarch64 library.

### Debugging

```sh
MUVM_FIXUP_LOG=/tmp/fixup.log muvm <command>
```

Logs every mapping the interposer handled. Without the variable it is silent.

### Removal

```sh
make uninstall
```

Removes all four files. Nothing on the system itself was modified, so no further
steps are needed.

## Limitations

Both are workarounds, not proper fixes. The right places would be libkrun's
`resource_map_blob` for part 1, and muvm providing a guest system bus for part 2.
Once that happens upstream, this can go away via `make uninstall`.

Touching pages eagerly also means every page of a buffer is faulted in at map
time instead of on demand. For very large buffers that costs a little time.

Tested on a MacBook Air M2 running Gentoo/Asahi with kernel 7.1.5, Mesa 26.0.8,
muvm 0.6.0, libkrun 1.18.0, virglrenderer 1.2.0. See [ENVIRONMENT.md](ENVIRONMENT.md)
for the full set of versions and the relevant configuration files.
