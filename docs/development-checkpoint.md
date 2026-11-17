# Development checkpoint

This document records the current implementation state for maintainers taking over the project.

## Implemented work

- Native C++20 desktop interfaces for Windows and Linux.
- Video conversion using FFmpeg with H.264, H.265 and AV1.
- CPU, NVIDIA NVENC, Intel QSV and AMD AMF capability detection.
- CRF input, eight compression presets and configurable audio bitrate.
- MP4 output and source-container output modes.
- Batch input from individual files, directories, ZIP archives and PPTX presentations.
- Per-file and overall progress, queue controls, automatic output naming and repeat-processing markers.
- Resource-aware parallel scheduling.
- SSH configuration discovery and editing, including preservation of unrelated directives and aliases that share a Host block.
- Windows Credential Manager integration for saved SSH passwords.
- SSH key and agent authentication.
- Remote FFmpeg capability probing, upload, conversion, progress forwarding, download and temporary-directory cleanup.
- Native SSH server management windows on Windows and Linux.
- Developer-facing unit tests for SSH configuration parsing and credential storage behavior.

## Current verification

- Linux CI has successfully configured and built the application, run unit tests and run the FFmpeg/ZIP/PPTX self-test.
- The latest Windows CI run reached compilation and reported two type-deduction errors in the SSH manager window's centering calculation. The operands are Windows `LONG` values and C++ `int` values; convert the rectangle coordinates to a consistent integer type before calling `std::max`.
- The same Windows build reports several MSVC secure-CRT warnings for direct `getenv` use. Replace environment access with a small platform abstraction using `_dupenv_s` on Windows and `std::getenv` on POSIX, and ensure allocated buffers are released.
- Remote conversion has not yet been verified against a live SSH server. Use a disposable test server with FFmpeg installed and exercise key authentication, password authentication, upload, conversion, progress, cancellation, download integrity and cleanup.
- NVIDIA, Intel and AMD hardware encoding require physical devices and suitable drivers for final validation.

## Visual review notes

The captured Linux desktop screenshot confirms that the output-container selector and SSH server entry point are visible. The current settings card has a layout defect: the lower row of batch options overlaps the audio bitrate controls. Increase the settings-card height and reposition the batch options and queue using a single vertical layout calculation.

The SSH manager screenshot shows the expected server fields and actions. Its dialog should be centered over the main window, use a clearer visual separation between server list and editor, and show or hide the password field according to the selected authentication method. Passwords must remain masked while editing.

## Code quality requirements

- Apply the repository's formatter consistently. Use a checked-in `.clang-format` file based on LLVM, four-space indentation, a 100-column limit, attached braces, and no single-line control-flow bodies.
- Run clang-format locally on all C++ headers and source files before committing.
- Run clang-tidy locally with the repository compile database. Prioritize readability, lifetime safety, error handling, portability and concurrency correctness. Do not move development lint checks into GitHub Actions.
- Enable and resolve compiler warnings locally for both GCC/Clang and MSVC. Do not silence warnings globally.
- Add concise comments where behavior or invariants are non-obvious, especially SSH config rewriting, credential handling, remote process cancellation and archive replacement.
- Keep platform-specific behavior behind small interfaces. Avoid duplicating business logic between the Windows and Linux UI layers.
- Every functional change must include tests and a local build. Update the developer handoff and decision log when architecture or persistent behavior changes.

## Remote-processing invariants

- The remote host is a compute target only. Users continue to select local input and output paths.
- Never pass passwords in command-line arguments, SSH URLs, logs or the SSH config file.
- Quote every remote shell argument. Never concatenate user-controlled paths into unquoted shell commands.
- Use a unique private temporary directory for each task and remove it on success, failure and cancellation.
- Download into a local temporary file, verify its size and media streams, then atomically replace the final destination.
- A lost SSH connection must leave the local source intact and must not report a successful conversion.
- Preserve source-container selection across direct video, ZIP and PPTX tasks.
