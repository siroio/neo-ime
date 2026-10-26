# neo-ime

Windows版Emacsで、Windows IMEの未確定文字をカーソル位置にインライン表示するパッケージです。
`neo-ime.el` が未確定文字をバッファへ一時挿入し、
`neo-ime-native.dll` がTSFまたはIMM32の入力を受け取ります。Emacs本体のパッチは不要です。

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

tar形式で導入する場合は `build-ime.ps1` が生成する `var/neo-ime-0.2.1.tar` を
`M-x package-install-file` で選び、上の宣言の `:vc` / `:load-path` を省いてください。
このリポジトリはMELPAなどのパッケージアーカイブには登録していません。

`M-x neo-ime-mode` で有効・無効を切り替えられます。
通常の編集・移動コマンドを実行すると表示中の未確定文字を取り消します。
IMEから届く確定文字の挿入では、次の未確定文節を取り消しません。
IME情報を取得できない場合は、その変換中だけWindows標準表示へ戻します。

表示は `neo-ime-preedit` / `neo-ime-target` のfaceで調整できます。
候補窓は親フレームのフォント・配色を使い、選択行は `highlight` faceで表示します。
候補窓は入力フォーカスを取らず、確定・取消・フォーカス解除で閉じます。
候補の描画方法は `neo-ime-candidate-backend` だけで設定します。
通常バッファ・ミニバッファともに同じ方法を使います。

- `corfu`: 入力位置の近くに枠付きポップアップを表示します。候補文字列だけを並べ、選択行を強調します。
- `child-frame`: neo-ime独自の番号・件数付き候補窓を表示します。単体パッケージの既定値です。

```elisp
(use-package neo-ime
  :custom (neo-ime-candidate-backend 'corfu)
  :hook (window-setup . neo-ime-mode))
```

`M-x customize-group RET neo-ime RET` でも変更・保存できます。
Corfuは別途導入してください。`corfu-mode` の有効化は不要です。
`child-frame` はCorfu・Verticoを必要としません。
Corfuでの変換中は通常のVertico候補欄を一時的に隠し、候補を閉じると元の表示へ戻します。
`read-string` などVerticoを使わない入力欄でも、選んだ描画方法を使います。
旧 `neo-ime-minibuffer-candidate-backend` 設定は削除してください。現在は使用しません。
表示先にかかわらず、候補順・選択・ページ切り替え・確定はWindows IMEが処理します。
Corfu・Verticoの内部描画APIを使うため、更新で互換性が変わる可能性があります。
候補子フレームにはタブバー・タブ行を表示しません。
`neo-ime-poll-interval` は初期値0.02秒です。変更後はmodeを再起動してください。
未変更のスナップショットはネイティブ側でコピーを省き、カーソル・候補だけの更新では
未確定文字のfaceを付け直しません。LispとDLLを更新した後はEmacsを再起動してください。

## IME状態とカーソル色

IMEがONのときはカーソルを赤くし、OFFのときは元の色を使います。
色はフレームごとに保持し、状態が取得不能になったときやmodeを無効化したときに復元します。
ON/OFFの自動切替や組み込みのmode-line表示は行いません。明示的な制御コマンドは利用できます。

```elisp
(setq neo-ime-cursor-color-on "red"
      neo-ime-cursor-color-off nil) ; nilは元のカーソル色

(neo-ime-status)                 ; 選択中のフレーム: on / off / unknown
(neo-ime-status (selected-frame)) ; フレームを明示して取得
```

両方の色を`nil`にするとカーソル色を変更せず、状態APIとフックだけを使えます。
設定変更は次のポーリング時に反映します。
`unknown`は未接続・無効化中・IMEコンテキストを取得できない場合を表し、OFFとは区別します。
状態はWindows IMEのON/OFFであり、ひらがな・カタカナ・英数の変換モードではありません。

`neo-ime-status`はキャッシュを読むだけなので、ユーザーのmode-line表示からも利用できます。
Windows側の通知を既存タイマーで取り込み、通常は`neo-ime-poll-interval`の間隔で反映します。
ポーリング間に起きた複数回の切り替えは、最後に観測した状態へまとめられます。

状態が変わると`neo-ime-status-change-hook`をフレームと新しい状態の2引数で呼びます。
初期状態は`unknown`で、有効化時に取得できた状態や、無効化時の`unknown`への変化も通知します。
フック内で`neo-ime-status`を呼ぶと更新後の状態を取得できます。

```elisp
(defun my-ime-status-changed (_frame _status)
  (force-mode-line-update t))
(add-hook 'neo-ime-status-change-hook #'my-ime-status-changed)
```

この機能を使うにはLispと同梱DLLを両方更新し、Emacsを再起動してください。

## ビルド

EmacsとMinGWのg++をPATHに用意し、リポジトリのルートで実行します。

```powershell
.\build-ime.ps1
```

DLLと、DLL・Lisp・nativeソース・手順・ライセンスを含む `var/neo-ime-0.2.1.tar` を生成します。
Emacsヘッダーの場所はEmacs自身から検出します。
検出を上書きする場合は `-EmacsRoot 'C:/path/to/emacs'` を指定してください。
DLLをロード済みなら、再ビルド前にそのEmacsを終了してください。

native実装はC++20です。[Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html)
に基づき、`.clang-format` で整形します。Win32ヘッダーには依存順序があるため、
`windows.h` が `commctrl.h`・`imm.h` より先になるよう設定しています。
ロック・IMEコンテキスト・フック・バッファをRAIIで解放し、可変長候補の
ヘッダー・オフセット・終端を検証します。共有するウィンドウ寿命フラグはatomicです。
GUIスレッドではEmacs APIを呼ばず、Lisp値の生成はロックを解放してから行います。
RTTIは無効です。TSFの標準コンテナでの確保失敗はCOM・Emacs APIの境界で捕捉し、
HRESULTまたはLispエラーへ変換します。
C++ランタイムは静的リンクするため、利用側に追加のMinGW DLLは不要です。

## 検証

```powershell
emacs.exe -Q --batch --eval '(setq load-prefer-newer t)' -l check-ime.el
emacs.exe -Q --batch --eval '(setq load-prefer-newer t)' -l check-ime-performance.el
emacs.exe -Q --batch --eval '(setq load-prefer-newer t)' -l check-ime-status.el
emacs.exe -Q --batch --eval '(setq load-prefer-newer t)' -l check-ime-tsf.el
g++ -std=c++20 -O2 -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror -fno-rtti -static -o var/check-ime-native.exe native/check_ime_native.cc native/tsf_text_store.cc native/tsf_backend.cc -limm32 -lcomctl32 -lole32 -loleaut32 -luuid
.\var\check-ime-native.exe
g++ -std=c++20 -O2 -Wall -Wextra -Werror -static -o var/check-tsf-store.exe native/check_tsf_store.cc native/tsf_text_store.cc -lole32 -luuid
.\var\check-tsf-store.exe
$nativeFiles = Get-ChildItem native -File | Where-Object { $_.Extension -in '.cc','.h' } | ForEach-Object FullName
clang-format --dry-run --Werror @nativeFiles
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
- IME状態の3値、変更時だけのフック通知、色設定の変更・無効化・取得不能時の色復元。
- 複数GUIフレームでの独立したカーソル色と再有効化、native側の状態通知と取得不能。
- 実Windowsウィンドウの別スレッドへのattach/detach、フォーカス解除、破棄。
- 合成したIMMデータでの部分確定・取得失敗時の標準表示復帰。
- 属性取得のエラー・長さ超過、バッファ確保失敗、候補コピー失敗の処理。
- 空の未確定文字で属性データがない場合の継続処理。
- 候補データのオフセット・終端・件数の検証、ページ表示・選択行、候補子フレームの再利用と解除。
- 明示的なCorfu切り替え、候補順・選択行・解除とUndoの保持。
- M-xのVertico描画・再描画抑制・通常補完への復帰、通常バッファとは独立した表示選択。
- タブバー・タブ行を有効にした環境での候補子フレームのタブ抑制。
- 新しい一時環境へのパッケージ導入、DLL読み込み、GUIフレームでの有効化・無効化・再有効化。

従来のIMM32経路はWindows x64 / Emacs 31.1で上記の検証を実行しています。
IMM32でのGoogle日本語入力による実入力・変換、白い未確定文字窓の抑止、自前候補一覧を確認済みです。
100件の候補取得・次ページの表示・選択行の更新・Enter確定・確定後の一回のUndoを実GUIで検証しました。
0.1.4では導入済みCorfuの実描画APIによる候補表示・選択行・非表示もGUI検証しています。
CorfuのGUI検証は `NEO_IME_CHECK_CORFU=1` を設定し、Corfuをload-pathに追加して `check-ime-gui.el` を実行します。

処理単体の計測は `emacs.exe -Q -l "$PWD/benchmark-ime.el"` で実行できます。
実DLLの未変更スナップショット取得10万回と、同じ未確定文字の更新1万回を各5回測定します。
`NEO_IME_BENCH_FAST=1` で変更番号を渡す取得経路を測定し、結果は
`var/bench-before.log` / `var/bench-after.log` に秒数・GC回数・GC時間として保存します。
コード変更の前後を比較するときは各版で実行してください。
通常の実入力や画面表示全体の遅延を測るものではありません。

候補はキーボードで選択します。候補のマウス選択は実装していません。

## TSFとIME制御（0.2.0）

`neo-ime-input-backend` は `auto`（既定）、`tsf`、`imm32` から選びます。
`auto` はTSF初期化を試し、初期化できないフレームではIMM32を使います。
`tsf` は初期化失敗をエラーにします。設定変更後はmodeを再起動してください。
入力方式と候補の描画方式（`neo-ime-candidate-backend`）は別の設定です。

TSFは入力セッションまたは選択範囲を文書として公開します。バッファ全体の同期は行いません。
文節属性・候補を取得できるIMEでは既存の描画を使い、候補取得に失敗すると標準UIを戻します。
変換中の非同期バッファ書き換えはエラーにして、一時文字列の破損を防ぎます。
入力位置の矩形はセッションのカーソル位置を使い、文字ごとの座標やマウスによる文節操作は扱いません。

| コマンド/API | 動作 |
| --- | --- |
| `M-x neo-ime-reconvert` | 選択範囲をTSFで再変換。IMEが対応していなければエラー。取消時は原文を保持 |
| `neo-ime-open` / `neo-ime-close` | IMEのON/OFFを要求し、取得できた実際の状態を返す |
| `neo-ime-toggle` | 現在の状態を取得して切替。取得不能時には切替しない |
| `neo-ime-conversion-mode` | 変換モードのシンボルまたは `unknown` を返す |
| `neo-ime-set-conversion-mode` | 変換モードを要求して再取得。他の変換フラグ・文節モードは保持 |

モードは `hiragana`、`full-katakana`、`half-katakana`、`full-alphanumeric`、
`half-alphanumeric` です。ON/OFFとは独立しており、モード指定だけではIMEをONにしません。
IMEが設定変更を拒否・遅延する場合があるため、戻り値は要求値ではなく取得値です。

```elisp
(neo-ime-open)
(neo-ime-set-conversion-mode 'hiragana)
(neo-ime-conversion-mode)
(neo-ime-close)
```

### Evilでの自動OFF

`neo-ime-mode` が有効なら、EvilのNormal・Visual・Motion・Operator状態に入ると
Windows IMEを自動でOFFにします。操作状態のバッファへの切替時にも同期します。
未確定文字は取り消し、確定済みの文字は保持します。
Insert・Replace・Emacs状態やミニバッファでは日本語入力を許可します。
Insertへ戻ったときの自動ONは行いません。必要なときに通常のIMEキーでONにしてください。
Evilを必須依存にはせず、`neo-ime-mode` を無効化すると連携フックも解除します。

Evil連携の検証は、インストール済みのEvilとgoto-chgを指定して実行できます。
GUI検証は専用プロセスでIME状態を変更し、自動終了します。実キーの変換テストとは別です。

```powershell
emacs.exe -Q --batch -L C:/path/to/evil -L C:/path/to/goto-chg -l check-ime-evil.el
emacs.exe -Q -L C:/path/to/evil -L C:/path/to/goto-chg -l check-ime-evil-gui.el
```

`native/tsf_text_store.*` が文書とロック、`native/tsf_backend.*` がGUIスレッドの
TSF寿命・キー・候補・制御を担当します。`native/tsf_compat.h` はMinGWに不足する
Windows SDKのABI宣言だけを補います。`neo-ime-tsf.el` が世代付きセッション・Undo・公開コマンドです。
Lisp/DLL/`neo-ime-tsf.el` は一緒に更新し、Emacsを再起動してください。

TSFの単体テストと専用Emacsでの初期化・解除、複数フレーム、ミニバッファへの入力先切替を検証しています。
今回の自動実行環境では最前面ウィンドウを取得できず、実キー入力によるTSF変換・再変換は未検証です。
従来のGoogle日本語入力の確認結果はIMM32経路の結果で、TSFの実入力成功を示すものではありません。
TSF専用GUIテストは同梱DLLを読みます。ビルド後、
`emacs.exe -Q -l check-ime-tsf-gui.el` で実行できます。

### 0.2.1の修正

TSFはUI-lessモードで有効化し、候補UI通知の購読が失敗した場合は初期化失敗として扱います。
Windows標準の入力窓・候補窓を抑止し、別フレームの通知で表示判断を上書きしません。
IME状態のポーリングはGUIスレッドで現在値を取得し、状態変更通知が届かない場合もカーソル色へ反映します。
次の回帰テストはUI-less有効化と表示判断の保持を検証します。

```powershell
g++ -std=c++20 -O2 -Wall -Wextra -Werror -static -o var/check-tsf-backend.exe native/check_tsf_backend.cc native/tsf_text_store.cc native/tsf_backend.cc -limm32 -lole32 -loleaut32 -luuid
.\var\check-tsf-backend.exe
```

`check_ime_native.cc` は通知なしの状態変更とTSF時の標準UI抑止も検証します。
`check-ime-evil-gui.el` は実DLLでIMEのON/OFFとカーソル色の連携を検証します。

## ライセンス

`neo-ime.el`、`native/` 内のC++ソース・ヘッダー、およびそのDLLはGPL-3.0-or-laterです。
原文は [COPYING](COPYING) にあります。
その他のファイルは、既存の [MIT LICENSE](LICENSE) に従います。
配布tar内の本体にもGPL表記とソースを同梱します。

参考: [Microsoft WM_IME_COMPOSITION](https://learn.microsoft.com/en-us/windows/win32/intl/processing-the-wm-ime-composition-message)、
[SetWindowSubclass](https://learn.microsoft.com/en-us/windows/win32/api/commctrl/nf-commctrl-setwindowsubclass)。
