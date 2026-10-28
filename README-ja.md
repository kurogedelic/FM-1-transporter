# FM-1 Transporter

[English](README.md)

![接続](docs/images/connection.png)

FM-1 の USB 信号線につないだ小型ボード（RP2040 / RP2350）を使い、PC から **M-VAVE FM-1**（JieLi AC791N / WL82）の Flash を読み書きするツールです。

## ハードウェア

配線は 3 本だけです。**VBUS は接続しないでください。** FM-1 は内蔵バッテリーで動作します。

| ボード | D+ | D- | 状態 |
|---|---|---|---|
| Seeed XIAO RP2040 | D6 (GP0) | D7 (GP1) | ✅ 推奨 |
| Raspberry Pi Debug Probe（UART コネクタ） | TX (GP4) | RX (GP6) | ✅ 動作確認済み |
| Raspberry Pi Pico | GP0 | GP1 | ⚠️ ビルドのみ、未検証 |
| Raspberry Pi Pico 2 / Pico 2 W | GP0 | GP1 | ⚠️ Pico 2 W は FM-1 1台で検証済み（キー入力、ダンプ、書き込み）。外付けプルダウンは不要でした。Pico 2 はビルドのみ、未検証 |
| Waveshare RP2350-USB-A / USB-C / USB-CM | GP12 | GP13 | ⚠️ 実験的・非推奨。基板の改造（D+ の 1.5 kΩ プルアップを外す、FM-1 に VBUS を出さない）とファームの移植が必要 |

どのボードも GND 同士をつなぎます。

## 導入

1. [Releases](../../releases) から使うボードの UF2 をダウンロードします。
2. BOOTSEL を押しながらボードを接続し、`RPI-RP2` ドライブに UF2 をコピーします。
3. [jl-uboot-tool](https://github.com/kagaimiq/jl-uboot-tool) から `wl82loader.bin` を入手します。このリポジトリには同梱していません。ツール使用時にボードの RAM へ送られます。

## 使い方

**ウェブアプリ**（Chrome / Edge）: `web/index.html` を開き、手順に従います。

**CLI**（pyserial が必要）:

```bash
python3 tools/fm1t.py --loader wl82loader.bin info
python3 tools/fm1t.py --loader wl82loader.bin dump backup.bin
python3 tools/fm1t.py --loader wl82loader.bin write --package FM-1_vNN.fwsc --ref backup.bin --write
```

`--write` なしの `write` は確認のみです。FM-1 のファームが壊れている場合は `fm1t.py rekey` を実行してから FM-1 の電源を入れます。その他は `fm1t.py --help` を参照してください。

## ビルド

pico-sdk 2.2.0、ARM GCC、CMake が必要です。

```bash
git submodule update --init
cmake -S . -B build -DPICO_SDK_PATH=$HOME/pico-sdk -DPICO_BOARD=seeed_xiao_rp2040
make -C build -j8 fm1_transporter
```

Pico は `-DPICO_BOARD=pico`、Pico 2 は `-DPICO_BOARD=pico2`（Pico 2 W は `pico2_w`）、Debug Probe は `-DFM1T_BOARD=debugprobe -DPICO_BOARD=pico` を指定します。

## 注意事項

- 自己責任で使用してください。書き込む前に必ずダンプを保存してください。
- 書き込むのは `0x4000`–`0x93000` だけです。
- 作業後は、USB で使う前に FM-1 の電源を入れ直してください。
- M-VAVE・JieLi とは無関係です。

[Architecture](docs/ARCHITECTURE.md) · [Protocol](docs/PROTOCOL.md) · [Dev log](docs/DEVLOG.md) · [AGENTS.md](AGENTS.md)

## ライセンス

MIT。[Pico-PIO-USB](https://github.com/sekigon-gonnoc/Pico-PIO-USB)（MIT）を含みます。一部のルーチンは jl-uboot-tool（MIT）から移植しています。
