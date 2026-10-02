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
  :demand t
  :hook (window-setup . (lambda () (neo-ime-mode 1)))
  :config
  (when (and (not noninteractive) after-init-time)
    (neo-ime-mode 1)))
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
  :demand t
  :hook (window-setup . (lambda () (neo-ime-mode 1)))
  :config
  (when (and (not noninteractive) after-init-time)
    (neo-ime-mode 1)))
```

tar形式で導入する場合は `build-ime.ps1` が生成する `var/neo-ime-0.1.3.tar` を
`M-x package-install-file` で選び、上の宣言の `:vc` / `:load-path` を省いてください。
このリポジトリはMELPAなどのパッケージアーカイブには登録していません。

`M-x neo-ime-mode` で有効・無効を切り替えられます。
通常の編集・移動コマンドを実行すると表示中の未確定文字を取り消します。
IMEから届く確定文字の挿入では、次の未確定文節を取り消しません。
IME情報を取得できない場合は、その変換中だけWindows標準表示へ戻します。

表示は `neo-ime-preedit` / `neo-ime-target` のfaceで調整できます。
候補窓は親フレームのフォント・配色を使い、選択行は `highlight` faceで表示します。
候補窓は入力フォーカスを取らず、確定・取消・フォーカス解除で閉じます。
`neo-ime-poll-interval` は初期値0.02秒です。変更後はmodeを再起動してください。

## ビルド

EmacsとMinGWのgccをPATHに用意し、リポジトリのルートで実行します。

```powershell
.\build-ime.ps1
```

DLLと、DLL・Lisp・nativeソース・手順・ライセンスを含む `var/neo-ime-0.1.3.tar` を生成します。
Emacsヘッダーの場所はEmacs自身から検出します。
検出を上書きする場合は `-EmacsRoot 'C:/path/to/emacs'` を指定してください。
DLLをロード済みなら、再ビルド前にそのEmacsを終了してください。

## 検証

```powershell
emacs.exe -Q --batch -l check-ime.el
gcc -std=c11 -Wall -Wextra -Werror -o var/check-ime-native.exe native/check-ime-native.c -limm32 -lcomctl32
.\var\check-ime-native.exe
emacs.exe -Q -l "$PWD/check-ime-gui.el"
```

`var/` とtarを用意するため、GUI検証の前にビルドを実行してください。
GUI検証は専用のEmacsプロセスを起動して自動終了します。
結果は `var/check-ime-gui.log` に書き込みます。

確認する内容:

- 未確定文字のバッファ挿入、更新・取消時のUndoと変更フラグの保持、確定後のUndo/Redo。
- 背景色の継承、UTF-16の位置変換、カーソル更新時の文字列の保持、保存前の一時文字除去。
- 実Windowsウィンドウの別スレッドへのattach/detach、フォーカス解除、破棄。
- 合成したIMMデータでの部分確定・取得失敗時の標準表示復帰。
- 候補データのオフセット・終端・件数の検証、ページ表示・選択行、候補子フレームの再利用と解除。
- 新しい一時環境へのパッケージ導入、DLL読み込み、GUIフレームでの有効化・無効化・再有効化。

Windows x64 / Emacs 31.1で上記の検証を実行しています。
Google日本語入力による実入力・変換、白い未確定文字窓の抑止、自前候補一覧を確認済みです。
100件の候補取得・次ページの表示・選択行の更新・Enter確定・確定後の一回のUndoを実GUIで検証しました。
IMM32互換の入力経路を使い、TSF専用の統合は実装していません。
候補はキーボードで選択します。候補のマウス選択は実装していません。

## ライセンス

`neo-ime.el`、`native/neo-ime-native.c`、およびそのDLLはGPL-3.0-or-laterです。
原文は [COPYING](COPYING) にあります。
その他のファイルは、既存の [MIT LICENSE](LICENSE) に従います。
配布tar内の本体にもGPL表記とソースを同梱します。

参考: [Microsoft WM_IME_COMPOSITION](https://learn.microsoft.com/en-us/windows/win32/intl/processing-the-wm-ime-composition-message)、
[SetWindowSubclass](https://learn.microsoft.com/en-us/windows/win32/api/commctrl/nf-commctrl-setwindowsubclass)。
