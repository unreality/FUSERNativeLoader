# Known game binaries

The FUSER shipping executables the loader supports (fue's `kBuilds`, host_hook's `kVerified`). They are kept here as
references for deriving addresses (`tools/derive_mp_build.py`, `tools/derive_fifth_deck_build.py`), disassembly and crash-dump triage. They are
copyrighted game files and are git-ignored (`.gitignore`: `binaries/**/*.exe`); only this README is tracked.

| Folder | Build | Engine CL | TimeDateStamp | SizeOfImage | SHA-256 |
|---|---|---|---|---|---|
| `egs-v1.4.2-cl1310155/FuserEOS-Win64-Shipping.exe` | EGS v1.4.2, the reference and test build | 1310155 | `6143E115` | `04138000` | `c6e1827d7f1e41051cd84b1e958cdf80078a01de75448d065b9ca891812baa3f` |
| `egs-cl1317808/FuserEOS-Win64-Shipping.exe` | EGS, January 2023 update ("EGS CL1317808") | 1317808 | `63C882A2` | `04138000` | `990c2035770f8ad39aa63e8bf5d62aae0d868a5547861396a9c92f4b44573079` |
| `steam-cl1317755/Fuser-Win64-Shipping.exe` | Steam, as shipped (SteamStub 3.1 x64; `.text` encrypted) | 1317755 | `63BCB6A2` | `041AE000` | `5531afb4318db09f948062d015c0c77b5813f2a71ceff937c6eb3c9d4baab43a` |
| `steam-cl1317755/unpacked/Fuser-Win64-Shipping.exe` | Steam, unpacked with Steamless 3.1.0.5 (`.bind` removed) | 1317755 | `63BCB6A2` | `0417B000` | `e271760728e56fadb2e19cdbe130c529bcb6c769a9ff56f35af61f219527eda3` |

## Notes

- **Sources:** EGS v1.4.2 comes from the EGS v1.4.2 install's `FUSER/Fuser/Binaries/Win64`. The EGS CL1317808 build and the Steam exe were both supplied on 2026-09-24.
- **Steam unpacked:** use this copy for disassembly. The packed exe's code is encrypted on disk.
  - Unpacking keeps every RVA, so the addresses in `kVerified` and in dumps apply to it directly.
  - It does not keep the packed identity. Its SizeOfImage differs, so cdb won't map it as a dump's module.
  - The loader identifies the Steam build by the packed identity.
  - To redo it: `Steamless.CLI.exe Fuser-Win64-Shipping.exe` writes `Fuser-Win64-Shipping.exe.unpacked.exe`.
- **Crash dumps:**
  - EGS v1.4.2 dumps: pass this folder's `egs-v1.4.2-cl1310155` as cdb's `-i` and `-y`.
  - Steam dumps: walk the stack by hand (`dps @rsp`), and disassemble the return addresses with
    `cdb -z steam-cl1317755\unpacked\Fuser-Win64-Shipping.exe`, then `ub <address>`.
