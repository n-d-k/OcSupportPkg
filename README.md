Additional features/changes implemented by this fork
============

[ Multi-Boot ]
         
         - ACPI patches are optional for non macOS with setting ACPI->Quirks->EnableForAll to yes (default is no).
         - Booter Quirtks, SMBIOS and Device Properties patches will only applied to macOS.
 
[ Hotkeys ]
 
         - Full functional Hotkeys [1-9] corresponding to Boot Entry's Index number and dedicated W (Windows) / X (macOS) keys can be used without seeing Boot Picker. While in boot picker, F10 can use to take a snapshot of the screen.
          
[ Ui Boot Picker ]
              
          - Bios Date/time, auto boot to the same OS or manual set to always boot one OS mode, and OC version are displayed in boot picker.
          - Auto boot to previous booted OS (if Misc->Security->AllowSetDefault is NO/false).
          - macOS Recovery/Tools Entries are hidden by default, use Spacebar in Boot Menu as a toggle on/off to show/hide hidden entries.
          
[ Custom Entries ]
 
          - Custom entries are now listed first in picker menu and by the orders they are appeared in Misc->Boot->Entries, before all other entries.
          - Ability to change entry name found by auto scanner by adding custom entry with the exact same device path, this will give users the option to complete change how all boot entries listed in Boot Picker.
    
[ Others ]

          - No verbose apfs.efi driver loading (if using apfs.efi instead of ApfsDriverLoader.efi).
          - ndk-macbuild.tool script are set to compile with latest edk2 (One can easily set to stable edk2 if prefer).
          - NvmExpressDxe driver build script are also available for system without native nvme support. (Compatible with OC and Clover).


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
based on [NetBSD implementation](https://blog.netbsd.org/tnf/entry/introduction_to_µubsan_a_clean).

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
