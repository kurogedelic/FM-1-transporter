# AGENTS.md

For agents operating an FM-1 Transporter on the user's behalf. Not a development guide.

The Transporter is a board wired to an M-VAVE FM-1's USB lines. You talk to it through `tools/fm1t.py` (needs pyserial). Details: `README.md`, `docs/PROTOCOL.md`.

## Before anything

- Ask the user to confirm the wiring is done and the FM-1 is the target. You cannot check either.
- You need `wl82loader.bin` (jl-uboot-tool). Pass it as `--loader PATH` or `FM1T_LOADER`. Never commit or publish it.
- Run `fm1t.py status` first. Fields: `uboot` (FM-1 in boot mode), `v15` (stock V15 running), `loader` (loader uploaded).

## Getting into boot mode

- Stock V15 running: `fm1t.py uboot` (automatic for `info`/`dump`/`write`).
- No working firmware: `fm1t.py rekey`, then ask the user to power the FM-1 off and on. You cannot do this.
- After the work: `fm1t.py runapp`, or ask the user to power-cycle the FM-1.

## Commands

```bash
python3 tools/fm1t.py --loader L info                # expect key=980F, id=856014; stop otherwise
python3 tools/fm1t.py --loader L dump backup.bin     # full 1 MiB read; keep this file
python3 tools/fm1t.py --loader L write --package P.fwsc --ref backup.bin          # dry run
python3 tools/fm1t.py --loader L write --package P.fwsc --ref backup.bin --write  # writes
```

## Rules

- Always `dump` before any write, and keep the dump.
- Always run the dry run first and show the user its result. Run `--write` only after the user says so for that package.
- Writes cover `0x4000`–`0x93000` only; the firmware refuses anything else. Do not try to work around it.
- If a write or verify fails, do not power-cycle or retry blindly. Report the exact output and tell the user not to power the FM-1 off.
- `write` accepts only the stock V15 `FM-1.fwsc` (pinned SHA-256). It is not redistributed; get it from M-VAVE's download page.
- If `info` does not return `key=980F` and `id=856014`, it is not an FM-1. Stop.
