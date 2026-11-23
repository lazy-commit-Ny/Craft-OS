# Craft-OS

RISC-V 64-bit bare-metal desktop prototype written in C and RISC-V assembly.
It boots on QEMU's `virt` machine using OpenSBI, a virtio-gpu framebuffer, and a
virtio mouse. The desktop uses a 32-bit 800x600 framebuffer, a higher-resolution
UI, six built-in app views, and damage-limited cursor updates to prevent the
screen flashing during mouse movement.

## Requirements

- Clang with the `riscv64-unknown-elf` target and LLD
- QEMU with `qemu-system-riscv64`, OpenSBI, `virtio-gpu-device`, and
  `virtio-mouse-device`
- noVNC and websockify (for browser access)

## Build and run

```bash
make
make run
```

The QEMU window opens automatically when a graphical display is available.
In a headless environment, the VM starts with display output disabled. To
choose explicitly:

```bash
make run QEMU_DISPLAY=gtk
make run QEMU_DISPLAY=none
```

### Run in a browser with noVNC

```bash
make novnc
```

Open [http://localhost:6081/vnc.html?autoconnect=true&resize=scale&path=websockify](http://localhost:6081/vnc.html?autoconnect=true&resize=scale&path=websockify).
In a remote VS Code/Codespaces workspace, forward port `6081` from the Ports
panel and open the forwarded URL. The QEMU VNC server listens only on
`127.0.0.1`; stop the task with `Ctrl+C` to stop QEMU and websockify.

The default ports are `6081` (noVNC) and `5901` (VNC display 1), avoiding the
usual `6080`/`5900` ports. If needed, choose other free ports with
`make novnc NOVNC_PORT=6082 VNC_DISPLAY=2`. The VNC connection has no password;
keep the forwarded port private.

Use the mouse to select apps from the dock. In the serial terminal, keys `1`-`6`
open the corresponding app, Enter toggles the window, and Escape closes it.
