# Repository Guidelines

## Project Structure & Module Organization

- Core user-space tools live in `tools/`; the built `hvisor` binary is staged into `output/` after `make`.
- Kernel modules are under `driver/`, with headers shared through `include/`.
- Third-party dependencies such as `cJSON/` are managed as git submodules; run `make env` after cloning.
- Example VM and virtio configurations are collected in `examples/` to bootstrap new targets.

## Build, Test, and Development Commands

- `make all ARCH=arm64 KDIR=~/linux` builds both tools and kernel modules. Adjust `ARCH`, `LOG`, `LIBC`, and `VIRTIO_GPU` as needed.
- `make tools` or `make driver` limits the build to user-space or kernel deliverables; artifacts land in `output/`.
- `make clean KDIR=~/linux` removes generated objects (clean requires `KDIR` because `driver/Makefile` depends on it).
- `make fmt` enforces formatting before pushing; the CI workflow runs `clang-format --Werror` on the same paths.

## Coding Style & Naming Conventions

- Follow the repo’s `.clang-format` (LLVM base, 4-space indent, no tabs). Run `make fmt` or `clang-format -i` on changed C/C++ headers and sources.
- Source files use lowercase with underscores (e.g., `virtio_blk.c`). Public headers live in `include/` and mirror module names.
- Prefer descriptive enum and struct names that match existing patterns; keep logging macros (`LOG_INFO`, etc.) consistent with current usage.

## Testing Guidelines

- There is no automated unit test suite today; validate changes by cross-compiling and exercising sample configs (e.g., `examples/qemu-aarch64/**`).
- Before submitting, load the rebuilt `driver/hvisor.ko` on a zone0 kernel and verify VM lifecycle commands such as `./hvisor zone start` and `./hvisor zone list`.
- Capture any regressions or manual test notes in the pull request description so reviewers can reproduce.

## Commit & Pull Request Guidelines

- Match the existing history: short, imperative subject lines (e.g., `Fix style`, `Clean up ARM64 zone config comments`). Squash noisy fixups before pushing.
- Reference related issues or hardware targets in the body when relevant, and mention required build options (`ARCH`, `VIRTIO_GPU`) for the change.
- Pull requests should describe the motivation, outline manual validation steps, and include screenshots or logs when touching device bring-up paths.

## Security & Configuration Tips

- Keep `refactor.sh` and `trans_file.sh` scripts updated if you automate deployment to targets; they assume artifacts in `output/`.
- When enabling virtio-gpu, ensure `libdrm` is present on the build host and set `VIRTIO_GPU=y` to avoid runtime mismatches.
