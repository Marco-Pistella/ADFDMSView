# ADFDMSView

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
![Far Manager 3](https://img.shields.io/badge/Far%20Manager-3.0%2B-blue)
![Platform](https://img.shields.io/badge/Windows-x86%20%7C%20x64-lightgrey)

**A Far Manager 3 plug-in that opens Amiga ADF disk images and DMS archives as if they were folders, and keeps working when the disk is damaged.**

An ADF image (OFS or FFS) or a DMS disk archive shows up as a virtual panel in which you can browse the directory tree, view files with <kbd>F3</kbd> and copy files or whole directories with <kbd>F5</kbd>. The plug-in is **read-only**: nothing is ever written to the image or to the archive.

What sets it apart is that it does not give up on bad material. Old floppy disks and old archives are often truncated, bit-rotted or missing a few bytes. ADFDMSView checks what it reads, repairs what can be repaired, brings back files that the directory tree no longer reaches, and tells you, item by item, what is complete, what is partial and what is lost.

Current version: **2.4.3**. The DLL is small (a few tens of KB before UPX compression) and comes in 32 and 64-bit builds.

## Features

- **ADF images**: OFS and FFS, bootblocks `DOS\0` to `DOS\7`, DD and HD disks (and any image of at least 8 blocks, up to 512 MiB), files of any size, any number of entries per directory.
- **DMS archives**: every compression mode (no compression, simple, quick, medium, deep, heavy 1, heavy 2). The archive is unpacked in memory and also offered as an ordinary `<name>.adf` that you can copy out for an emulator.
- **Repair of damaged DMS archives**: every track is verified (CRC-16 of the packed data and 16-bit checksum of the unpacked track); damaged track headers are repaired; damaged packed data are repaired when one byte has a wrong value, was lost or was added (the length of the data says which, and the CRC-16 points at the candidates; the end of the file gives the length of the last track), or, when no single byte explains the CRC, when a burst of up to 16 bits or two bits are wrong (these corrections are solved from the CRC, tried the fewest bits first, and taken only if the unpacked data also end where the packed data end and no other candidate of the same kind fits). A repair is accepted only if the CRC of the packed data and the checksum of the unpacked track are then both right; when a correction of bits is found after a class of candidates with fewer wrong bits was too large to be tried, it is not completely verified and the track is shown as not verified. A track whose data are damaged only where the decoder does not look is kept without any search. A track that cannot be verified is kept as decoded instead of leaving a hole in the disk, and after a lost track the next ones are tried again from an empty dictionary.
- **Encrypted DMS archives**: they are opened without asking for the password; the key is found from the checksum of the first track (when more than one key fits it, or a key fits only weakly, that track is shown as not verified).
- **Recovery on damaged ADF disks**: truncated images, non-standard bootblocks, bad root block checksums, damaged block tables, broken hash chains, disks without a root block or even without any file header. Files and data blocks that the directory tree does not reach are collected in a `_RECOVERED` folder.
- **A written report**: `_RECOVERY_REPORT.txt` lists what was found and repaired, one line per finding.
- **Clear status in Far**: the Description column flags files that are unreadable, incomplete or not verified; the information panel (<kbd>Ctrl</kbd>+<kbd>L</kbd>) shows bad sectors, bad tracks, root block state and DMS track statistics.
- **Safe copy**: a file that cannot be read completely is still copied, with the unreadable blocks replaced by zeros, and a box at the end says how many items were copied incomplete or unverified and how many could not be copied. A block that belongs to another file is never copied into a file.
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
| DMS (`.dms`) | Floppy disk archives, also encrypted ones, standard 80-cylinder DD and HD disks and the 160 half-cylinder layout |

Not opened (a box explains why): DMS archives that do not hold a disk image, archives in which no disk track can be found (also encrypted ones whose key is not found and from which nothing can be decoded), files shorter than 56 bytes or larger than 512 MiB. Only the Amiga OFS and FFS file systems are read (not PFS, SFS and others).

## Damaged DMS archives

Each track of the disk ends up in one of these states:

| State | Meaning |
| --- | --- |
| right | CRC and checksum are right |
| repaired | Header or packed data were damaged and were repaired; CRC and checksum are right afterwards |
| not verified | Packed data are intact but the track checksum is not the expected one, or the key of the track (first track of an encrypted archive) or the repair of its bits is not completely verified; the track is kept |
| best effort | Data are damaged and could not be repaired; what could be unpacked is kept, the rest is zero; tracks that copy from it are kept the same way |
| lost | The track is in the archive but nothing could be recovered from it; written as zeros |
| absent | The track is not in the archive; written as zeros |

When the CRC of a track is wrong, the track is first unpacked as it is, and kept as right if the checksum of the unpacked track is right. Otherwise damaged packed data are repaired when one byte was lost, added or has a wrong value, or a few bits are wrong (a burst of up to 16 bits, or two bits). With so many candidates a 16-bit checksum could not tell a right repair from a wrong one by itself, so the corrections of bits are accepted only under extra rules (the unpacked data must end where the packed data end, the candidates that flip the fewest bits are tried first and one is taken only if it is the only one of its class that fits, and a damage that leaves the stream in step is tried only with short bursts). When a class of candidates is too large for the limit of a track it is skipped, and a correction found after that is shown as not verified. Damage in more than one place of a track, or a damaged sector, is not repaired and the track is kept as best effort. In the modes that keep a dictionary from track to track, a track that does not verify is unpacked a second time from an empty dictionary.

The key of an encrypted archive is found from the checksum of the first disk track (a short search for a normal DOS disk, the 65536 possible keys otherwise). A key that gives the right checksum but not the end of the packed data is shown as not verified: a search through all the keys finds about one such key by chance even when the right one is not among them.

The repair search is bounded by 6 seconds and 400,000 candidate decodes per archive (`DMS_REPAIR_SECONDS` and `DMS_REPAIR_BUDGET`). The search for wrong bits is limited to 40,000 candidate decodes and one second per track (`DMS_BITS_TRIALS` and `DMS_BITS_SECONDS`), and can use at most half of the archive-wide repair time and decode budget. The search for an encryption key is limited to 30 seconds (`DMS_KEY_SECONDS`). The report states when a limit stops a search.

## Limits

- Read-only: nothing can be written, deleted or created inside an image or an archive.
- The whole image is held in memory (up to 512 MiB, set by `MAX_FILE_SIZE`); a DMS archive is also unpacked in memory. In the 32-bit version a very large image may not fit in Far's address space.
- Only the last modification date is read; protection bits and file comments are not shown, and hard and soft links are not listed.
- Items without an Amiga date (the unpacked `<name>.adf`, `_RECOVERY_REPORT.txt`, `_RECOVERED`, `DATA_<n>.bin`) and items whose header has an impossible date get the date of the copy.
- Directories nested more than about 60 levels deep are not entered.
- `_RECOVERED` cannot tell a lost file from a deleted one: a deleted file whose blocks are still on the disk can show up there.
- On a damaged OFS disk two data blocks can claim the same position of a file: the one with a right checksum is used, else the one with the lowest block number, and the report says so.

## Languages

English is built into the DLL and shipped as `ADFDMSViewEng.lng`. Far loads the `.lng` file that matches its interface language and falls back to English.

To add a language, copy `ADFDMSViewEng.lng` to `ADFDMSView<Language>.lng`, change the language name on the first line, translate the messages between the double quotes, keep every message in the same order and keep the placeholders (`%u`, `%d`, `%ls`), and save the file as UTF-8 with BOM. The details are in section 8 of [ADFDMSView.txt](ADFDMSView.txt). Translations are welcome as pull requests.

The recovery report is always in English.

## Building from source

The plug-in is a single C file, [`adfdmsview.c`](adfdmsview.c), plus the version resource [`adfdmsview.rc`](adfdmsview.rc). Build it from the Windows Command Prompt, in the project folder:

```bat
build.bat              build both DLLs (x64 and x86)
build.bat x64          build only .\x64\ADFDMSView.dll
build.bat x86          build only .\x86\ADFDMSView.dll
build.bat noupx        build without UPX compression
build.bat max          use UPX --ultra-brute as well (slower; cannot be combined with noupx)
build.bat zip          create ADFDMSView-<version>.zip (needs tar.exe)
build.bat x64 max zip  combine options: build x64, use maximum UPX search, then package it
```

**MSYS2 and compilers.** The script searches for MSYS2 (default `C:\msys64`, or `MSYS2_ROOT`). If it is not found, it offers an unattended download and installation. This offer is available on x64 Windows 10 version 1809 (build 17763) or later when `curl.exe` is present and the target folder is missing or empty. Answer `1` to install, or `0` to leave the system unchanged and see the official MSYS2 website.
The current installer requires Windows 10 version 1809 (build 17763) or later. The installer is
downloaded from the MSYS2 installer project and MSYS2 is then updated with `pacman`. MSYS2
recommends a short ASCII-only installation path on an NTFS volume, without spaces or network drives;
the default `C:\msys64` is the safest choice.

The script checks for a GCC compiler and `windres.exe` for each target. The default x64
environment is **MINGW64 only**; this deliberately prefers the MSVCRT-based toolchain because it
produces substantially smaller DLLs for this plug-in. UCRT64 is not selected automatically. If
MINGW64 is missing, the installation menu offers MINGW64 and UCRT64 and requires an explicit
choice. To override the default, run `set "ENV64=ucrt64"` before `build.bat` to force UCRT64, or
`set "ENV64=mingw64 ucrt64"` to search MINGW64 first and then UCRT64. For x86, the default and only
supported environment here is MINGW32. MINGW64 and MINGW32 are legacy MSYS2 environments, and
their package availability may change as MSYS2 phases them out. Choose `0` in the installation
menu to skip that DLL. Package installation is never attempted without a choice.

**Far SDK.** A `plugin.hpp` in the project folder is used first. When `FARSDK` is set, that folder (or its `Headers.c` subfolder) is checked; otherwise the script checks `PluginSDK` and the `PluginSDK\Headers.c` folder of an installed Far Manager. If the header is still missing and `FARSDK` was not explicitly set, the script downloads it from the official Far Manager repository into `PluginSDK` (requires `curl.exe`). The header is part of the Far Manager SDK and is not included here.

**UPX.** By default the script searches `UPX_EXE`, `PATH` and the MSYS2 folders, preferring the MINGW64 UPX executable. If UPX is missing, it tries to install the native MINGW64 package `mingw-w64-x86_64-upx` with `pacman`; if that is not possible, the DLL is still built unpacked. Normal packing uses `--best --lzma`. When packing succeeds, the build log reports the DLL size before and after UPX. If UPX fails, the unpacked DLL is restored and the warning is shown. The `max` option adds `--ultra-brute`, which can take substantially longer; it asks UPX to try more variants and does not guarantee a smaller file. `noupx` skips packing entirely.

The script checks that `Info->Version` in `adfdmsview.c` agrees with the four version numbers in `adfdmsview.rc`, and that `VER_STRING` matches them. It also checks the DLL exports with `objdump` when that tool is present. Each output folder receives the language files next to the DLL. The `zip` option includes the folders built, the user documentation and `LICENSE` (or `LICENSE.TXT`); it requires `tar.exe` (included with current Windows versions).

Useful environment variables:

| Variable | Purpose | Default |
| --- | --- | --- |
| `MSYS2_ROOT` | MSYS2 installation folder | `C:\msys64` |
| `MSYS2URL` | MSYS2 installer URL used for automatic installation | current MSYS2 x86-64 installer |
| `ENV64` | x64 GCC environments to try, in order | `mingw64` |
| `ENV32` | x86 GCC environments to try, in order | `mingw32` |
| `FARSDK` | Folder containing `plugin.hpp` or its `Headers.c` subfolder | `PluginSDK` search and automatic download |
| `UPX_EXE` | Full path to `upx.exe` | search `PATH` and MSYS2 folders |

The portable core (the DMS unpacker and the ADF reader, without Win32 or Far headers) can also be built alone with `gcc -DADF_CORE_ONLY ...`.

## Repository contents

| File | Purpose |
| --- | --- |
| `adfdmsview.c` | Plug-in source: portable core and Far plug-in |
| `adfdmsview.rc` | Version resource of the DLL |
| `ADFDMSViewEng.lng` | English language file |
| `ADFDMSView.txt` | Full user manual |
| `build.bat` | Build script (MSYS2 / MinGW-w64) |
| `LICENSE` | MIT License |

## License

ADFDMSView is released under the [MIT License](LICENSE).

Copyright (c) 2026 Marco Pistella
