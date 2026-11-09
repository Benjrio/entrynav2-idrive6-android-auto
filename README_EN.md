# Wired Android Auto for BMW HU_ENTRYNAV2

[Русский](README.md) | **English**

Wired Android Auto: USB AOA → AA/TLS → H.264 decoding → RGB through shared fd4 →
EGL/IVI in a dedicated HMI window. Audio, microphone capture, and iDrive control
of AA are not implemented yet. Sessions are limited to 1,200 seconds.

Building requires a local SDK, head unit libraries, and a TLS identity. After
cloning, provide these dependencies as described in [PRIVATE_INPUTS_EN.md](PRIVATE_INPUTS_EN.md).

## Warning and limitation of liability

**You build, install, run, and modify this software entirely at your own risk.**
Experiment only on your own personal device, and only if you are prepared to
accept possible damage or loss of functionality.

The software is provided “as is,” without any guarantee of functionality,
compatibility, or freedom from defects. Using it may cause failures, data loss,
temporary or permanent loss of functionality, or damage to the head unit or
connected devices.

To the maximum extent permitted by applicable law, the developers and copyright
holders are not liable for any direct or indirect damage, including device
damage, data loss, and recovery costs, arising from the use of or inability to
use the software.

The recommendation to experiment only on a personal device concerns safe
operation and does not impose additional restrictions on rights granted by
GPL-3.0-or-later. The disclaimer of warranty and limitation of liability are
also set out in sections 15–17 of [LICENSE](LICENSE).

## Contents

- `src/`: native HMI adapter, control IPC, USB receiver, producer, and headers.
- `assets/`: HMI XML, manifest, and text/image resources.
- `runtime/aa/`: USB selection, reference TLS identity, decoder/TLS libraries,
  and original revision 148 binaries used to validate screen order.
- `vendor/`: the complete set of local compiler inputs, without references to
  a neighboring repository.
- `build.py`, `commands.json`: build definitions for four ARM targets.
- `install-usb.sh`: installation and OPEN/QUIE/status session control.
- `THIRD_PARTY_EN.md`, `third-party/`: third-party notices, licenses, and library sources.

## Building

The supported head unit display is 1280×480; AA video is 800×480.
The native adapter and scene reader require an exact HMI/library profile.

Requirements: Python 3.12+, LLVM clang, and ld.lld with ARM Linux hard-float
support. The target is GNU/Linux ARMv7, not the Android ABI. Building has been
checked on macOS with LLVM from Android NDK 28.2.13676358. The sysroot and GCC
support files are already in `vendor`; the NDK sysroot is not required.
On Windows, build inside WSL using Linux LLVM/Python, preserving case-sensitive
repository filenames. Linux/WSL LLVM has not been separately validated here.

```sh
python3 build.py --cc /path/to/clang --ld /path/to/ld.lld
```

If clang and ld.lld are in PATH, use `python3 build.py`.
You can also set `CC`, `LD_LLD`, or `--out /absolute/output`.
Outputs under `build/` include `libOwnAndroidAuto.so`, `ownandroidauto-control.elf`,
`ownandroidauto-producer.elf`, `aa/hu-aa.elf`, resources, and `compile.json`.
The script only compiles; it does not execute ARM code. Once the local
dependencies are available, building works offline.

## Preparing and running on the head unit

The native adapter checks the exact ABI/build IDs in `src/profile.hpp`.
The installation path and mount checks are defined in `install-usb.sh`:
`/opt/app/cisu`, `/dev/mmcblk0p6`, and ext4. These are parameters of the supported
head unit, not paths on the build computer. Changing them requires separate
profile validation. Operation was tested on head unit version 002_148_002.
A normal restart is required after installation on the head unit.

### Configuring your phone before building

`runtime/aa/usb-device.conf` contains the placeholder `USB_PORT VID PID`.
It deliberately fails validation: the USB session cannot start until it is
filled in. Replace it with one line containing three space-separated values:

```text
<USB-port> <VID-4-hex> <PID-4-hex>
```

Do not include the angle brackets. USB-port is the device name in the head
unit's sysfs: numbers separated by a hyphen and, when a hub is present, dots.
VID and PID must each contain exactly four hexadecimal digits without `0x`.
Use the phone's identifiers **before it enters AOA accessory mode**, in the USB
mode in which it will connect to the head unit. VID/PID may change when the USB
mode changes.

Read the values from `/sys/bus/usb/devices/<USB-port>/idVendor` and `idProduct`
on the target head unit. Identify the port by observing which device appears
when the phone is connected; the USB port on your computer does not determine
the head unit's USB topology. These files can be read manually without
performing AOA negotiation or changing USB settings.
The phone's serial number is not needed. Then run `build.py`: the configuration
will be copied to `build/aa/usb-device.conf`. There is no need to change VID/PID
in the code; the parser and filter are in `src/usb_selection.h`.

The value `auto` is also supported, but selects only a device **already in
standard AOA accessory mode** (`18d1:2d00` / `18d1:2d01`). It does not discover
arbitrary Android devices in their normal USB mode. Selection is rejected if
multiple devices match. These standard AOA IDs in `src/usb-native.h` identify
the protocol, not a phone model.

### Work needed for general device support

- Add discovery of AOA-capable phones in normal USB mode without a fixed VID/PID,
  with controlled AOA protocol checks and physical port selection. Do not send
  AOA requests indiscriminately to every USB device.
- Define behavior for multiple phones and changes in USB topology while
  retaining rejection of ambiguous device claims.
- Validate entry into accessory mode, re-enumeration, disconnection, interface
  release, and subsequent sessions across different phones and AA versions.
- Check AA version negotiation compatibility: `src/aa-phone-version.h` retains
  support for observed 1.1 and 1.7 responses; other versions require separate
  protocol validation.
- Prepare validated ABI profiles and a runtime update procedure for other head
  units. Phone configuration does not remove HMI/firmware restrictions.

After building, copy **the contents of build/** to a new directory on the head
unit, `/tmp/ownandroidauto-kit-usb-01`, using access already configured by the
owner. Passwords, SSH keys, network settings, and automatic connection tools
are not included. In the head unit shell, run:

```sh
sh /tmp/ownandroidauto-kit-usb-01/run-hu.sh install /tmp/ownandroidauto-kit-usb-01
```

The installer checks HMI/ABI compatibility, obtains the actual Plugins paths
through control inspect, validates the persistent mount, and prepares
OwnAndroidAuto registration. It preserves existing files belonging to others
and does not overwrite a completed v24 runtime.
If runtime v24 is already installed, this is **not an update mechanism**:
older binaries may remain active. A new build first needs a separate
version/runtime-path plan or normal removal of the old registration while the
module is not loaded. Do not overwrite a mapped DSO.

On REGISTRATION_PENDING (action23), wait for the next normal complete head unit
startup performed by the owner. No restart or PluginUnload/PluginLoad commands
are included. After registration, connect an Android phone by USB and run:

```sh
sh /tmp/ownandroidauto-kit-usb-01/run-hu.sh open /tmp/ownandroidauto-kit-usb-01
sh /tmp/ownandroidauto-kit-usb-01/run-hu.sh status /tmp/ownandroidauto-kit-usb-01
sh /tmp/ownandroidauto-kit-usb-01/run-hu.sh stop /tmp/ownandroidauto-kit-usb-01
```

`stop` sends QUIE, terminates the application's producer/receiver, and releases
USB and its layer. Use the physical MENU button to return to the native menu.
Do not launch the receiver separately: it requires fd4 inherited from the
producer. The native adapter starts the producer using private IPC and lifecycle
management. USB takeover is restricted to accessory interface 0 with driver
usbfs, not arbitrary USB interfaces.
Logs: `/tmp/ownandroidauto-aa-*/android-auto.log`, summary.json, usb-transport.json,
`/tmp/ownandroidauto-native-*.log`, and `/tmp/ownandroidauto-producer-*.log`.
The screen-order reader only reads the winston process. There is no atomic CAS
for the shared layer order; the producer aborts startup if validation fails.

## License

The project source code is distributed under the **GNU General Public License,
version 3 or (at your option) any later version** — `GPL-3.0-or-later`.
The full text is in [LICENSE](LICENSE).
The program is provided without any warranty, including the implied warranties
of merchantability or fitness for a particular purpose; see LICENSE for details.
Third-party copyright and license notices remain in effect;
see [THIRD_PARTY_EN.md](THIRD_PARTY_EN.md).
