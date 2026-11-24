# RetroArch でうまく動かなかった時に（TapeOS 編）

TapeOS（xmil コア）を RetroArch で動かそうとして上手くいかなかった時に、症状から原因と直し方を引くための文書です。2026-11-25 に素の Debian 環境で実際に踏んだ罠をもとに書いています。平常時の操作（起動、ゲームフォーカスの ON/OFF、全画面、終了など）は、別の文書「RetroArch 最低限の操作（TapeOS を動かすために）」にあります。

## 症状：メニューの日本語が豆腐（□）や「？」になる

- メニュー用フォントは **assets** の中にあります。Debian では `retroarch-assets` パッケージが必要です。
- 設定ファイルの `assets_directory` は、assets の**親フォルダ**を指します（例：`/usr/share/libretro/assets`）。`.../assets/ozone` のように1段深くすると、フォントが読めずドット絵フォントに化けます。
- `retroarch.cfg` の中で同じキーが2回出てきた場合、**先に書かれた方が勝ちます**。末尾に足しても効かないことがあります。
- root で起動すると root のホームの設定を読むため、自分の `~/.config/retroarch/retroarch.cfg` を編集しても反映されません。必ず自分のユーザーで起動してください。

## 症状：コアやファイルが見つからない／選べない

Debian のデフォルトは次のとおりです（環境により異なります。設定 → ディレクトリ、または `retroarch.cfg` の `libretro_directory` / `system_directory` で確認できます）。

- コア：`/usr/lib/x86_64-linux-gnu/libretro/` に `x1_libretro.so` と `x1_libretro.info` を置きます（sudo が必要）。
- システム：`~/.config/retroarch/system/xmil/` に、IPL ROM を **`IPLROM.X1` という名前で**、フォント `FNT0808.X1` を置きます。IPL ROM の実体は xmil リポジトリの `rom/X1_compatible_rom.bin` です（名前を変えて配置します）。フォントは本リポジトリに同梱されていません。meister68k 氏の X1 互換フォント配布に含まれているので、そちらから入手してください。

## 起動方法（ブラウザで選べない時の迂回路）

メニューからは「コアをロード → コンテンツをロード」で `shell.d88` を選びます。ファイルブラウザで名前が化けて選べない場合は、コマンドラインから直接指定できます。

```
retroarch -L /usr/lib/x86_64-linux-gnu/libretro/x1_libretro.so images/shell.d88
```

## 症状：文字入力がおかしい（スペースで ▶▶、F で画面が切り替わる）

X1 のようなキーボードを使うコアでは、**ゲームフォーカスを ON** にしてください（デフォルトの切替キーは Scroll Lock）。ON の間はホットキーが無効になり、キーボードがまるごとコアに渡ります。

OFF のままだと、次のようなホットキーが文字入力と衝突します。

- スペース ＝ 早送り（▶▶ が出て、入力が暴走します）
- F ＝ 全画面切替（FOR も IF も打てません）
- ESC 2回 ＝ 終了（実行中に押すとホストに戻ります）

起動時から ON にするには、設定 `input_auto_game_focus = "1"` を使います。起動スクリプトに `--appendconfig` で差し込むこともできます。

全画面から抜けるには、Scroll Lock でフォーカスを切ってから F（全画面切替）または F1（メニュー）を使います。

## その他の操作メモ

- 終了は ESC 2回です。
- 表示サイズ（640×400 など）の通知は RetroArch とコアの間の取り決めで、TapeOS 本体の動作とは無関係です。
