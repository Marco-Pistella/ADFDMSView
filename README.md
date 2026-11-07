# ADFDMSView

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
![Far Manager 3](https://img.shields.io/badge/Far%20Manager-3.0%2B-blue)
![Platform](https://img.shields.io/badge/Windows-x86%20%7C%20x64-lightgrey)

**A Far Manager 3 plug-in that opens Amiga ADF disk images and DMS archives as if they were folders, and keeps working when the disk is damaged.**

An ADF image (OFS or FFS) or a DMS disk archive shows up as a virtual panel in which you can browse the directory tree, view files with <kbd>F3</kbd> and copy files or whole directories with <kbd>F5</kbd>. The plug-in is **read-only**: nothing is ever written to the image or to the archive.

What sets it apart is that it does not give up on bad material. Old floppy disks and old archives are often truncated, bit-rotted or missing a few bytes. ADFDMSView checks what it reads, repairs what can be repaired, brings back files that the directory tree no longer reaches, and tells you, item by item, what is complete, what is partial and what is lost.

Current version: **2.3.0**. The DLL is small (about 50 KB before UPX compression) and comes in 32 and 64-bit builds.

## Features

- **ADF images**: OFS and FFS, bootblocks `DOS\0` to `DOS\7`, DD and HD disks (and any image of at least 8 blocks, up to 512 MiB), files of any size, any number of entries per directory.
- **DMS archives**: every compression mode (no compression, simple, quick, medium, deep, heavy 1, heavy 2). The archive is unpacked in memory and also offered as an ordinary `<name>.adf` that you can copy out for an emulator.
- **Repair of damaged DMS archives**: every track is verified (CRC-16 of the packed data and 16-bit checksum of the unpacked track); damaged track headers are repaired; damaged packed data are repaired when the damage is a burst of up to 16 bits, two flipped bits, or one byte lost or added. A repair is accepted only if the track checksum is then right.
- **Recovery on damaged ADF disks**: truncated images, non-standard bootblocks, bad root block checksums, damaged block tables, broken hash chains. Files and data blocks that the directory tree does not reach are collected in a `_RECOVERED` folder.
- **A written report**: `_RECOVERY_REPORT.txt` lists what was found and repaired, one line per finding.
- **Clear status in Far**: the Description column flags unreadable and incomplete files; the information panel (<kbd>Ctrl</kbd>+<kbd>L</kbd>) shows bad sectors, bad tracks, root block state and DMS track statistics.
- **Safe copy**: a file that cannot be read completely is still copied, with the unreadable blocks replaced by zeros.
- **Original dates**: files and directories keep their Amiga date and time when copied out (the clock value is taken as local time), and the panel shows them and can sort by date.
- **Windows-safe names**: Amiga (Latin-1) names are converted to valid Windows names; reserved names and duplicates are handled.
- **Translatable**: every text comes from a Far `.lng` file; no recompilation is needed to add a language.

## Requirements and installation

- Far Manager 3.0 (build 3000 or later), 32 or 64-bit. Use the DLL of the same bitness as Far.
- Copy `ADFDMSView.dll` and `ADFDMSViewEng.lng` together into a folder of their own under the Far plug-ins folder, for example:

  ```
  %FARHOME%\Plugins\ADFDMSView\
  ```

  and restart Far. Far looks for language files in the folder of the DLL, so the folder must not be shared with another plug-in.

- Put the cursor on a `.adf` or `.dms` file and press <kbd>Ctrl</kbd>+<kbd>PgDn</kbd> (<kbd>Enter</kbd> may do the same, depending on your file associations).

Pre-built DLLs, when available, are on the Releases page of this repository.

## Using the panel

| Key | Action |
| --- | --- |
| <kbd>Enter</kbd> / `..` | Enter a directory / go back up to the root of the disk |
| <kbd>F3</kbd> | View a file (extracted to a temporary folder first) |
| <kbd>F5</kbd> | Copy the selected items, directories with all their subdirectories, with a copy dialog |
| <kbd>Ctrl</kbd>+<kbd>L</kbd> (other panel) | Information panel for the disk |
| <kbd>Ctrl</kbd>+<kbd>6</kbd> | Descriptions panel mode (shows the status of each item) |

Write operations (<kbd>F5</kbd> or <kbd>F6</kbd> into the panel, <kbd>F7</kbd>, <kbd>F8</kbd>, <kbd>Del</kbd>, saving a file edited with <kbd>F4</kbd>, moving items out with <kbd>F6</kbd>) show a box saying that the panel is read-only. To take items out, copy them with <kbd>F5</kbd>.

### Special items in the root of the panel

| Item | When | What it is |
| --- | --- | --- |
| `<name>.adf` | DMS archives only | The disk image unpacked from the archive |
| `_RECOVERED` | If something was recovered | Files and data blocks that the directory tree does not reach |
| `_RECOVERY_REPORT.txt` | If damage was found | Report of what was found and repaired (`ERROR:` lines are problems, `note:` lines are repairs and remarks) |

## Supported formats

| Format | Notes |
| --- | --- |
| ADF (`.adf`) | OFS and FFS, including international and directory-cache variants (read through the normal hash tables) |
| DMS (`.dms`) | Floppy disk archives, standard 80-cylinder DD and HD disks and the 160 half-cylinder layout |

Not opened (a box explains why): encrypted DMS archives, DMS archives that do not hold a disk image, archives in which no disk track can be found, files shorter than 56 bytes or larger than 512 MiB. Only the Amiga OFS and FFS file systems are read (not PFS, SFS and others).

## Damaged DMS archives

Each track of the disk ends up in one of these states:

| State | Meaning |
| --- | --- |
| right | CRC and checksum are right |
| repaired | Header or packed data were damaged and were repaired; CRC and checksum are right afterwards |
| not verified | Packed data are intact but the track checksum is not the expected one; the track is kept |
| best effort | Data are damaged and could not be repaired; what was unpacked is kept |
| lost | The track is in the archive but could not be recovered; written as zeros |
| absent | The track is not in the archive; written as zeros |

The search for repairs is bounded (6 seconds and 150,000 trial unpacks per archive, set by `DMS_REPAIR_SECONDS` and `DMS_REPAIR_BUDGET` in the source), so opening a damaged archive never takes forever; the report says when a limit stopped a search.

## Limits

- Read-only: nothing can be written, deleted or created inside an image or an archive.
- The whole image is held in memory (up to 512 MiB, set by `MAX_FILE_SIZE`); a DMS archive is also unpacked in memory. In the 32-bit version a very large image may not fit in Far's address space.
- Only the last modification date is read; protection bits and file comments are not shown, and hard and soft links are not listed.
- Items without an Amiga date (the unpacked `<name>.adf`, `_RECOVERY_REPORT.txt`, `_RECOVERED`, `DATA_<n>.bin`) and items whose header has an impossible date get the date of the copy.
- Directories nested more than about 60 levels deep are not entered.
- `_RECOVERED` cannot tell a lost file from a deleted one: a deleted file whose blocks are still on the disk can show up there.

## Languages

English is built into the DLL and shipped as `ADFDMSViewEng.lng`. Far loads the `.lng` file that matches its interface language and falls back to English.

To add a language, copy `ADFDMSViewEng.lng` to `ADFDMSView<Language>.lng`, change the language name on the first line, translate the messages between the double quotes, keep every message in the same order and keep the placeholders (`%u`, `%d`, `%ls`), and save the file as UTF-8 with BOM. The details are in section 8 of [ADFDMSView.txt](ADFDMSView.txt). Translations are welcome as pull requests.

The recovery report is always in English.

## Building from source

The plug-in is a single C file, [`adfdmsview.c`](adfdmsview.c), plus the version resource [`adfdmsview.rc`](adfdmsview.rc).

**Prerequisites**

- [MSYS2](https://www.msys2.org/) (default `C:\msys64`, or set `MSYS2_ROOT`). The compilers `mingw-w64-x86_64-gcc` and `mingw-w64-i686-gcc`, and UPX, are installed with `pacman` automatically if missing.
- `plugin.hpp` from the Far Manager 3 plug-in SDK, in `.\PluginSDK` or in the folder named by the `FARSDK` variable (the `Headers.c` subfolder and the SDK of an installed Far are also tried). It is part of Far Manager, which has its own license, so it is not included in this repository.

**Build**

From the Windows command prompt:

```
build.bat            build both versions, compressed with UPX
build.bat x64        build .\x64\ADFDMSView.dll (64 bit)
build.bat x86        build .\x86\ADFDMSView.dll (32 bit)
build.bat noupx      do not compress with UPX (can be combined with x64 or x86)
build.bat zip        also create ADFDMSView-<version>.zip, for example ADFDMSView-2.3.0.zip (needs tar.exe, Windows 10 or later)
```

The language files are copied next to each DLL, and the zip contains the built folders, `ADFDMSView.txt` and `LICENSE`. The build stops if the version in `adfdmsview.rc` differs from the one in `GetGlobalInfoW`, and it checks with `objdump` that all the plug-in entry points are exported with their plain names. Some antivirus programs are suspicious of UPX-packed files: use `noupx` for an unpacked DLL.

The portable core (the DMS unpacker and the ADF reader, with no Win32 and no Far headers) can be built alone with `gcc -DADF_CORE_ONLY ...`.

## Repository contents

| File | Purpose |
| --- | --- |
| `adfdmsview.c` | Plug-in source: portable core and Far plug-in |
| `adfdmsview.rc` | Version resource of the DLL |
| `ADFDMSViewEng.lng` | English language file |
| `ADFDMSView.txt` | Full user manual |
| `build.bat` | Build script (MSYS2 / MinGW-w64) |
| `LICENSE` | MIT License |

## Contributing

Bug reports and pull requests are welcome. When you report a problem with a specific disk, please include the contents of `_RECOVERY_REPORT.txt` if the panel shows one. By contributing you agree that your contribution is released under the MIT License.

## License

ADFDMSView is released under the [MIT License](LICENSE).

Copyright (c) 2026 Marco Pistella
