# Compiling Armbian for HiKey960

The mainline Linux kernel requires specific configuration parameters to properly function on the HiKey960 without crashing. Standard Armbian build scripts often overwrite or lack these specific patches.

## Automated Flawless Build

To ensure a stable build, we have provided an automated injection script: `scripts/01-build-armbian.sh`.

### Prerequisites
You must have the official Armbian build framework cloned to your local machine:
```bash
git clone https://github.com/armbian/build armbian-build
cd armbian-build
```

### The Script Logic
The `01-build-armbian.sh` script automatically:
1. Locates the `linux-uefi-arm64-edge.config` file in the Armbian userpatches directory.
2. Strips out unstable kernel symbols (e.g., `CONFIG_DRM_PANFROST` to disable the GPU, preventing SError panics).
3. Injects required symbols for headless operation, networking (2.5GbE adapters), and USB hub power routing.

### Execution
Run the provided script from the repository root before initializing the Armbian compiler:
```bash
./scripts/01-build-armbian.sh
```
Once the patches are injected, compile the system natively using the Armbian framework:
```bash
./compile.sh BOARD=hikey960 BRANCH=edge RELEASE=trixie BUILD_MINIMAL=yes BUILD_DESKTOP=no KERNEL_ONLY=no KERNEL_CONFIGURE=no
```
The output will be a raw `.img` file and a `.img.xz` archive containing the rootfs, which we will extract and prepare for fastboot flashing in the next step.
