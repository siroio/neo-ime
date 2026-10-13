# neo-ime

Windows版Emacsで、Windows IMEの未確定文字をカーソル位置にインライン表示するパッケージです。
`neo-ime.el` が未確定文字をバッファへ一時挿入し、
`neo-ime-native.dll` がIMM32のイベントを受け取ります。Emacs本体のパッチは不要です。

- 白背景の未確定文字ウィンドウを抑止し、下線付きの文字列へ置き換えます。
- 変換対象の文節を太字にし、IMEから取得した候補をEmacsの子フレームで表示します。
- 候補の選択行・ページ・件数を表示します。Space・上下キー・PageUp/Down・数字・EnterはIMEが処理します。
- 未確定文字の更新・除去はUndo履歴と変更フラグを保持します。
- 確定直前に一時文字を除去し、確定文字をEmacs標準の挿入経路でUndoに記録します。
- 取消・保存・自動保存・無効化時には一時文字を除去します。
- 無効化するとnative subclass・タイマー・overlayを解除します。複数フレームにも対応します。

## 導入

Windows GUI版Emacs 29.1以降とダイナミックモジュール対応が必要です。
同梱のDLLはWindows x64版Emacs 31.1でビルド・読み込みを確認しています。
別アーキテクチャでは同じアーキテクチャのMinGWで再ビルドしてください。

`use-package :vc` が使えるEmacsでは、次の設定でGitHubから導入できます。
DLLを同梱しているため、導入時のCコンパイラは不要です。

```elisp
(use-package neo-ime
  :vc (:url "https://github.com/siroio/neo-ime" :rev :newest)
  :hook (window-setup . neo-ime-mode))
```

Windows側でGoogle日本語入力などのIMEを選び、半角/全角キーで入力します。
IMEの選択や `default-input-method` は変更しません。SKKや `mozc.el` は不要です。
Emacs内部の入力メソッドは同時に有効にしないでください。

Gitで取得して使う場合:

```powershell
git clone https://github.com/siroio/neo-ime.git
```

```elisp
(use-package neo-ime
  :load-path "C:/path/to/neo-ime"
  :hook (window-setup . neo-ime-mode))
```

tar形式で導入する場合は `build-ime.ps1` が生成する `var/neo-ime-0.1.4.tar` を
`M-x package-install-file` で選び、上の宣言の `:vc` / `:load-path` を省いてください。
このリポジトリはMELPAなどのパッケージアーカイブには登録していません。

`M-x neo-ime-mode` で有効・無効を切り替えられます。
通常の編集・移動コマンドを実行すると表示中の未確定文字を取り消します。
IMEから届く確定文字の挿入では、次の未確定文節を取り消しません。
IME情報を取得できない場合は、その変換中だけWindows標準表示へ戻します。

表示は `neo-ime-preedit` / `neo-ime-target` のfaceで調整できます。
候補窓は親フレームのフォント・配色を使い、選択行は `highlight` faceで表示します。
候補窓は入力フォーカスを取らず、確定・取消・フォーカス解除で閉じます。
候補の表示先は通常バッファとミニバッファで独立して選べます。

| 設定 | 対象 | 選択肢 |
| --- | --- | --- |
| `neo-ime-candidate-backend` | 通常バッファ | `corfu` / `child-frame` |
| `neo-ime-minibuffer-candidate-backend` | ミニバッファ | `vertico` / `child-frame` |

どちらも単体パッケージの既定値は `child-frame` です。
`M-x customize-group RET neo-ime RET` で変更・保存できます。
CorfuとVerticoを導入済みなら、以下で通常バッファにCorfu、ミニバッファにVerticoを使えます。

```elisp
(use-package neo-ime
  :custom ((neo-ime-candidate-backend 'corfu)
           (neo-ime-minibuffer-candidate-backend 'vertico))
  :hook (window-setup . neo-ime-mode))
```

両方を `child-frame` にすればCorfu・Verticoへの依存はありません。
Corfu・Verticoの自動導入は行いません。選択した描画パッケージが未導入なら、
有効化時にエラーを表示します。`corfu-mode` の有効化は不要です。
`vertico` は既に有効なVerticoの補完セッションの候補領域を使用します。
M-xなどでIME候補を表示している間はコマンド候補の再描画を抑え、
候補が閉じたときや変換の確定・取消後に通常の補完表示へ戻します。
`read-string` などVerticoのセッションがないミニバッファでは `child-frame` を使います。
表示先にかかわらず、候補順・選択・ページ切り替え・確定はWindows IMEが処理します。
Corfu・Verticoの内部描画APIを使うため、更新で互換性が変わる可能性があります。
候補子フレームにはタブバー・タブ行を表示しません。
`neo-ime-poll-interval` は初期値0.02秒です。変更後はmodeを再起動してください。
未変更のスナップショットはネイティブ側でコピーを省き、カーソル・候補だけの更新では
未確定文字のfaceを付け直しません。LispとDLLを更新した後はEmacsを再起動してください。

## ビルド

EmacsとMinGWのg++をPATHに用意し、リポジトリのルートで実行します。

```powershell
.\build-ime.ps1
```

DLLと、DLL・Lisp・nativeソース・手順・ライセンスを含む `var/neo-ime-0.1.4.tar` を生成します。
Emacsヘッダーの場所はEmacs自身から検出します。
検出を上書きする場合は `-EmacsRoot 'C:/path/to/emacs'` を指定してください。
DLLをロード済みなら、再ビルド前にそのEmacsを終了してください。

native実装はC++20です。[Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html)
に基づき、`.clang-format` で整形します。Win32ヘッダーには依存順序があるため、
`windows.h` が `commctrl.h`・`imm.h` より先になるよう設定しています。
ロック・IMEコンテキスト・フック・バッファをRAIIで解放し、可変長候補の
ヘッダー・オフセット・終端を検証します。共有するウィンドウ寿命フラグはatomicです。
GUIスレッドではEmacs APIを呼ばず、Lisp値の生成はロックを解放してから行います。
例外とRTTIは無効にし、確保失敗は戻り値で処理します。
C++ランタイムは静的リンクするため、利用側に追加のMinGW DLLは不要です。

## 検証

```powershell
emacs.exe -Q --batch -l check-ime.el
emacs.exe -Q --batch -l check-ime-performance.el
g++ -std=c++20 -O2 -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror -fno-exceptions -fno-rtti -static -o var/check-ime-native.exe native/check_ime_native.cc -limm32 -lcomctl32
.\var\check-ime-native.exe
clang-format --dry-run --Werror native/neo_ime_native.cc native/neo_ime_native.h native/check_ime_native.cc
emacs.exe -Q -l "$PWD/check-ime-gui.el"
emacs.exe -Q -L C:/path/to/corfu -L C:/path/to/vertico -l "$PWD/check-ime-minibuffer.el"
```

`var/` とtarを用意するため、GUI検証の前にビルドを実行してください。
GUI検証は専用のEmacsプロセスを起動して自動終了します。
結果は `var/check-ime-gui.log` に書き込みます。
ミニバッファ検証には導入済みCorfu・Verticoをload-pathに追加してください。
結果は `var/check-ime-minibuffer.log` に書き込みます。

確認する内容:

- 未確定文字のバッファ挿入、更新・取消時のUndoと変更フラグの保持、確定後のUndo/Redo。
- 背景色の継承、UTF-16の位置変換、カーソル更新時の文字列の保持、保存前の一時文字除去。
- カーソル・候補更新時のバッファ変更番号の保持、属性変更時のハイライト更新。
- 未変更スナップショットの省略、候補座標の整数オーバーフローの拒否。
- 実Windowsウィンドウの別スレッドへのattach/detach、フォーカス解除、破棄。
- 合成したIMMデータでの部分確定・取得失敗時の標準表示復帰。
- 属性取得のエラー・長さ超過、バッファ確保失敗、候補コピー失敗の処理。
- 空の未確定文字で属性データがない場合の継続処理。
- 候補データのオフセット・終端・件数の検証、ページ表示・選択行、候補子フレームの再利用と解除。
- 明示的なCorfu切り替え、候補順・選択行・解除とUndoの保持。
- M-xのVertico描画・再描画抑制・通常補完への復帰、通常バッファとは独立した表示選択。
- タブバー・タブ行を有効にした環境での候補子フレームのタブ抑制。
- 新しい一時環境へのパッケージ導入、DLL読み込み、GUIフレームでの有効化・無効化・再有効化。

Windows x64 / Emacs 31.1で上記の検証を実行しています。
Google日本語入力による実入力・変換、白い未確定文字窓の抑止、自前候補一覧を確認済みです。
100件の候補取得・次ページの表示・選択行の更新・Enter確定・確定後の一回のUndoを実GUIで検証しました。
0.1.4では導入済みCorfuの実描画APIによる候補表示・選択行・非表示もGUI検証しています。
CorfuのGUI検証は `NEO_IME_CHECK_CORFU=1` を設定し、Corfuをload-pathに追加して `check-ime-gui.el` を実行します。

処理単体の計測は `emacs.exe -Q -l "$PWD/benchmark-ime.el"` で実行できます。
実DLLの未変更スナップショット取得10万回と、同じ未確定文字の更新1万回を各5回測定します。
`NEO_IME_BENCH_FAST=1` で変更番号を渡す取得経路を測定し、結果は
`var/bench-before.log` / `var/bench-after.log` に秒数・GC回数・GC時間として保存します。
コード変更の前後を比較するときは各版で実行してください。
通常の実入力や画面表示全体の遅延を測るものではありません。

IMM32互換の入力経路を使い、TSF専用の統合は実装していません。
候補はキーボードで選択します。候補のマウス選択は実装していません。

## ライセンス

`neo-ime.el`、`native/` 内のC++ソース・ヘッダー、およびそのDLLはGPL-3.0-or-laterです。
原文は [COPYING](COPYING) にあります。
その他のファイルは、既存の [MIT LICENSE](LICENSE) に従います。
配布tar内の本体にもGPL表記とソースを同梱します。

参考: [Microsoft WM_IME_COMPOSITION](https://learn.microsoft.com/en-us/windows/win32/intl/processing-the-wm-ime-composition-message)、
[SetWindowSubclass](https://learn.microsoft.com/en-us/windows/win32/api/commctrl/nf-commctrl-setwindowsubclass)。
