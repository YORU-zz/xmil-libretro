# パッチ版 xmil（TapeOS 用）— 暫定版 2026-11-25

これは、OUROBO/ROS 再現プロジェクト（P2）の **TapeOS を動かすための、自作パッチを当てた
libretro 版 X1 エミュレータコア**（xmil-libretro のフォーク）です。
パッチはブランチ **`tapeos-patch-20261122`** にあります（基点は上流コミット
`3106aa54ffb244cca4019a6e95dacaa698781bc3`）。

> **暫定版の注記**：これは **暫定版 2026-11-25**（開発中スナップショット）です。
> 開発中につき内容は変更されうるもので、正本リポジトリではありません。
> マイルストーンごとに再生成される輸出形として公開しています。

## パッチ内容（上流との差分）

- **CMT テープ再生の実装**（`io/cmt.c`、`io/ppi.c`）：上流では `cmt_read()` がスタブで
  テープ入力が永遠に届かず、IPL のテープ起動が成立しませんでした。CPU クロック同期の
  ビットストリームプレイヤーを実装し、PPI ポート B bit1 を駆動するようにしました。
  テープイメージは環境変数 `XMIL_TAPE` で指定する `.cmt` 形式（パックドビット列）です。
- **WAV テープ対応**（`io/cmt.c`、2026-11-24 追加）：`XMIL_TAPE`（録音は
  `XMIL_TAPE_REC`）のパスが `.wav` なら、clean な PCM WAV（8/16 ビット、モノ／
  ステレオ、任意のサンプルレート）の再生／録音もできます。コア側の処理は固定
  しきい値でのデジタイズのみで、汚れた WAV の正規化はホスト側ツール
  `wavclean`（TapeOS リポジトリ側で配布）の担当です。録音は正規形式
  （48000Hz、16 ビット、モノ）で書き出します。`.cmt` 経路は従来どおり残ります。
- **EMM 1MB ＋ RAMDISK**（`io/memio.c`、`io/memio.h`、`io/iocore.c`）：24 ビット
  アドレスラッチ式 EMM のバックングストアを実装し、TapeOS の RAMDISK を載せました。
- **キーボード対応の修正**（`libretro/sdlkbd.c`）。
- **RS-232C ボード**（`io/usart.c`／`io/usart.h`、iocore に登録）。i8251
  （USART）のレジスタモデルを I/O ページ 0x0C（0x0C00 = データ、
  0x0C01 = コントロール／ステータス）に実装しました。モード／コマンド
  ワードの状態機械、TxRDY／RxRDY／TxEMPTY の実チップ準拠のステータス、
  名目ボーレート（ボードクロック 153.6kHz、×16 分周で 9600bps）に
  基づく送信タイミングを持ちます。ホスト側は環境変数 `XMIL_SERIAL` で
  指定したキャラクタデバイス（PTY 等）に橋渡しします。未設定時は
  ボードだけが存在する状態（送信は名目速度で排出、受信なし）となり、
  既存の動作は変わりません。

差分の実体は `patches/xmil-libretro-3106aa5.patch`（9 ファイル、+1181/−12）にも
同梱しています。起動シーケンス（IPL の FDD→拡張 ROM→CMT の試行順）には手を入れていません。

## ビルド手順

```sh
make -C libretro        # -> libretro/x1_libretro.so（Linux の場合）
```

生成されたコアを RetroArch のコアディレクトリに配置してください。

## TapeOS の起動手順

1. 上記のとおり本コアをビルドし、RetroArch から使えるようにします。
2. 同梱の互換 IPL ROM `rom/X1_compatible_rom.bin` を、RetroArch の system
   ディレクトリ内 `xmil/IPLROM.X1` として配置します（meister68k 氏作、CC0）。
   あわせて、同じ system/xmil フォルダに `FNT0808.X1`（8×8 フォント、
   meister68k 氏の X1 互換フォント配布／美咲フォント由来）を置いてください。
3. **TapeOS 本体（ディスクイメージ）は本リポジトリには含まれません。**
   別リポジトリ **[YORU-zz/tapeos](https://github.com/YORU-zz/tapeos)** で配布します
   （暫定版を公開中です）。そちらのイメージを本コアで起動すれば、TapeOS がそのまま起動します。

## ライセンスとクレジット

- コア本体（xmil）：**Yui 氏 © 2015**、BSD 様式ライセンス（`LICENSE` 参照）。
  本リポジトリは libretro/xmil-libretro のフォークです。
- 当プロジェクトのパッチ：**MIT**（asa2yoru、全文は `NOTICE` 参照）。
- 同梱 IPL ROM：**meister68k 氏**の X1 compatible ROM ver 0.7.3、**CC0 1.0**。
- 詳細は `NOTICE` を参照してください。

---

## 上流の README（原文）

以下は、上流 libretro ポートに付属していた README の原文です。帰属表示のため、
内容を変更せずに残しています（当プロジェクトのパッチ内容や現在の配布状況とは
関係ありません）。

Libretro port of X Millennium Sharp X1 emulator


svn: http://amethyst.yui.ne.jp/svn/x1/xmil/trunk/ 
Website: http://www.turboz.to/


WIP and basic port 

bios load from retroarch system directory in a folder named
xmil: ("IPLROM.X1" "IPLROM.X1T")

valid_extensions = "dx1|zip|2d|2hd|tfd|d88|88d|hdm|xdf|dup|cmd"

Know bugs:

- Opening disk from gui not working for now (endless loop & freeze)

