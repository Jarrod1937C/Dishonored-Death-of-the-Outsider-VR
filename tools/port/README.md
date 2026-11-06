# tools/port: moving the mod's addresses from Dishonored 2 to DOTO

These scripts ported every hook address and struct offset of the Dishonored 2 VR mod
(`Dishonored2.exe` 1.77.9.0) to Death of the Outsider (`Dishonored_DO.exe` 1.145.0.0, Steam,
link stamp `0x67AB7505`) by matching the code of the two executables. Run them again the
same way if either game is patched.

Requirements: Python 3 with numpy, GNU `objdump` (binutils, with the `pei-x86-64` target).
No disassembler library is needed. `tools/pe.py` must be importable
(`PYTHONPATH=<repo>/tools`). Set `D2_EXE` and `DOTO_EXE` to the two executables
(defaults: `~/mnt/DH2/Dishonored2.exe`, `~/mnt/Dishonored_DeathOfTheOutsider/Dishonored_DO.exe`).
Work in a scratch directory: the scripts write their indexes (`d2.npz`, `doto.npz`,
`state.pkl`, `maps3.pkl`, `al.npz`) to the current directory.

## Building the maps (once per exe pair)

1. `python extract.py $D2_EXE d2 0x1000 0x2000000` and the same for `$DOTO_EXE doto`:
   objdump's disassembly of `.text`, stored per instruction: address, length, the bytes with
   every rel32 / RIP-relative displacement masked, the call / jump / data references, and a
   "shape" hash (mnemonic + operands with all numbers removed). About 30 s each.
2. `python pipeline.py`: functions from `.pdata` plus call targets; unique exact matches of
   the masked bytes; then, repeatedly, instruction alignment of matched pairs by shape
   (difflib) whose aligned call / data references propose new pairs, and "gap filling"
   between ordered anchors (the linker keeps function order). Resumable (`state.pkl`);
   rerun until it reports no new pairs. Result: about 88% of Dishonored 2's functions paired.
3. `python buildal.py`: flattens the result into `al.npz` for fast lookups.

## Looking things up

| Script | What it answers |
|---|---|
| `q.py <d2 rva>...` | where a D2 address is in DOTO: function start, an aligned instruction, a byte inside one, or data (by the references of aligned code) |
| `callmap.py <d2 func>...` | the same for a function, voted by its callers' aligned call sites. **Always check function starts with it**: small functions (accessors, thunks) can be mis-paired by similarity |
| `ddiff.py <d2 func>...` | the aligned instructions of a function pair whose bytes differ: the struct offsets, vtable slots and constants that moved |
| `offmap.py <value>...` | a displacement / immediate followed across every aligned instruction that uses it (votes; different structs can share a value, so prefer `ddiff.py` on the function that owns the field) |
| `table.py <src file>...` | every address-like literal of a source file with its address and offset mappings |
| `dis.sh d2\|doto <rva> [len]` | objdump of a range |
| `iat.py a:<rva> b:<rva>` | names of import table slots |
| `find.py d2\|doto "48 8B ?? .."` | byte pattern search in `.text` |
| `apply.py <file> '{"0xOLD":"0xNEW"}'` / `edit.py <file>` | token-exact / text-exact source edits that keep CRLF line endings |

## What the port found (2026-11-05)

- Renderer code is almost all byte-identical; game code moved and many classes changed size.
- `renderView_t`: every field from the fov on is 0x10 lower (fov 0x8C/0x90, vieworg 0x98,
  viewaxis 0xBC, explicit projection 0xF0, its flag 0x130). The view object's own fields are unchanged.
- The player camera's 0x25xxx fields are +0x30; the player's first-person view is +0x310/+0x31C;
  `gameLocal`'s fields near +0x340xxx are +0x146C0.
- `idRenderSystemLocal` has five more virtuals before slot 0x200 (+0x28).
- DOTO's end-of-frame code makes no `ID3D11DeviceContext::Flush`, so there is nothing for
  `SkipFrameSyncFlush` to skip.
- DOTO's window procedure reads the window size with `GetWindowRect` (Dishonored 2: `GetClientRect`).
