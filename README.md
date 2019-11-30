Additional features implemented by this fork
============

- Hotkey W to boot directly to first available Windows boot entry from either auto scanner or custom entries. (Hold down W to boot Windows OS directly).
- Auto default boot to last booted macOs or Windows.
- No verbose apfs.efi driver loading (if using apfs.efi instead of ApfsDriverLoader.efi).
- Avoid duplicated entry in boot menu, cusstom entry will not be added to boot menu if the same entry already found by auto scanner.
- Ability to change entry name found by auto scanner by adding custom entry with the exact same device path.
- Compile with latest edk2.
- NvmExpressDxe driver build script are also available for system without native nvme support. (Compatible with OC and Clover).
- ACPI patches is now optional for non macOS with setting ACPI->Quirks->EnableForAll to yes (default is no).

OcSupportPkg
============

Additional UEFI support common libraries shared by other projects in [Acidanthera](https://github.com/acidanthera). The primary purpose of the library set is to provide supplemental functionality for Apple-specific UEFI drivers.

Early history of the codebase could be found in [AppleSupportPkg](https://github.com/acidanthera/AppleSupportPkg) and PicoLib library set by The HermitCrabs Lab.

## Features

- Apple PE image signature verification
- CPU information gathering
- Cryptographic primitives (SHA-256, RSA, etc.)
- Helper code for ACPI reads and modifications
- Higher level abstractions for files, strings, timers, variables
- Overflow checking arithmetics
- PE image loading with no UEFI Secure Boot conflict
- Plist configuration format parsing
- PNG image loading

#### OcGuardLib

This library implements basic safety features recommended for the use within the project. It implements fast
safe integral arithmetics mapping on compiler builtins, type alignment checking, and UBSan runtime,
based on [NetBSD implementation](https://blog.netbsd.org/tnf/entry/introduction_to_�ubsan_a_clean).

The use of UBSan runtime requires the use of Clang compiler and `-fsanitize=undefined` argument. Refer to
[Clang documentation](https://releases.llvm.org/7.0.0/tools/clang/docs/UndefinedBehaviorSanitizer.html) for more
details.

## Credits

- The HermitCrabs Lab
- All projects providing third-party code (refer to file headers)
- [Download-Fritz](https://github.com/Download-Fritz)
- [Goldfish64](https://github.com/Goldfish64)
- [savvamitrofanov](https://github.com/savvamitrofanov)
- [vit9696](https://github.com/vit9696)
