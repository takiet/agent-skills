# PLAN — cpu-proc vs a9-gpu-proc 前処理ベンチマーク

対象デバイス: Axis Q1728 (ARTPEC-9, aarch64, OS 12.11.x) / appName **`ppcomp`** / vendorId `0123456ABC`（**固定**）

## Phase 1 — `ppcomp` でやり直す

skeleton は `testapp` で通っているが、**appName は専用 SSH ユーザー名に含まれる**ので、
`ppcomp` に改名した時点で `testapp` の SSH ユーザーは使えない。よって Phase 1 を
`ppcomp` として一通りやり直し、Setup Verification を全部埋めてから Phase 2 に入る。

### Setup Verification — **全項目クリア（2026-07-31）**

- [x] `make build` → `build/ppcomp_1_0_0_aarch64.eap` 生成
- [x] `deploy.sh` でインストール成功（`OK`）
- [x] `control.sh ppcomp start` → `view_log.sh ppcomp` に `Hello World`（syslog 経路）
- [x] `setup_ssh.sh` 成功 → `run.sh ppcomp ppcomp` → `output` に `Hello World`（stdout 経路）

vendorId は `0123456ABC` のまま据え置く。以降 `ppcomp` を remove せず上書きインストールで回す。
古い `testapp` は放置してよい。

スクリプトはプロジェクト直下には無いので、プロジェクトルートから skill のパスで叩く:
`bash /Users/taki/Arbete/agent-skills/skills/building-acap/scripts/deploy.sh build/ppcomp_1_0_0_aarch64.eap`
（プロジェクトに `scripts/` をコピーするかは要相談）

## 方針

- 「性能値が取れれば十分」という前提なので、エラー処理は **失敗したらログを出して次のシナリオへ進む**
  レベルに留める。リトライ・グレースフル復帰は作らない。
- 計測は **`larodRunJob()` の前後のみ** を `clock_gettime(CLOCK_MONOTONIC)` で挟む。
  テンソル生成・モデルロード・VDO バッファ取得は計測外。これがデバイス間の唯一フェアな比較点。
- フレームは **起動時に 1 枚だけ** 1920x1080 NV12 で取得し、50 回すべてで使い回す。
  （CLAUDE.md の記述どおり。キャプチャ揺らぎが計測に混ざらない）
- ウォームアップ 3 回を実行して統計から除外する。GPU は初回だけ桁違いに遅く、
  それを含めると平均も標準偏差も壊れるため。ウォームアップ値も参考として syslog に別途出す。
- 結果は syslog と stdout の両方へ。syslog は `view_log.sh` 用、stdout は `run.sh` の `output` 用。

## ファイル構成

```
app/
├── manifest.json      <- resources.linux.user.groups: ["video"] を追加
├── Makefile           <- PKGS = gio-2.0 gio-unix-2.0 liblarod vdostream / all に test_preproc 追加
├── ppcomp.c           <- main: 4シナリオ × 2デバイスを回して統計出力
├── capture.c/.h       <- VDO NV12 1920x1080 を 1 枚取得、fd/offset/capacity/pitch を返す
├── preproc.c/.h       <- larod 前処理ジョブの構築・実行（シナリオとデバイスをパラメータ化）
└── test_preproc.c     <- 正当性検証用: 指定シナリオの出力を raw で stdout へダンプ
```

`deepLearningProcessor` は宣言しない（cpu-proc / a9-gpu-proc しか使わないため）。

## 増分

### 2-A: デバイス列挙 + キャプチャ — **完了（2026-07-31）**

実機実測で確定した値（2-B 以降はこれを前提にしてよい）:

```
larod: 6 device(s) found
  cpu-tflite / a9-dlpu-native / armnn-cpu-tflite / cpu-proc / a9-dlpu-tflite / a9-gpu-proc
capture: width=1920 height=1080 pitch=1920 subformat=NV12 buffer.type=dmabuf capacity=3110400
```

- **pitch = 1920**（width と一致、パディング無し）。ただし実装は `vdo_stream_get_info()` の
  実測値を使う形のままにしておく。
- **buffer.type = `dmabuf`** → `larodConvertVmemFdToDmabuf()` は**不要**。`vdo_buffer_get_fd()` の
  fd を `dup()` して `larodSetTensorFd()` に直接渡す。リスク表の該当項目はこれで解消。
  `CapturedFrame.is_dmabuf` フラグがあるので分岐は残す。
- `buffer.type` は `vdo-stream.h` の info テーブルに載っていない**未文書化キー**。実機で値が
  返ることは確認済み。
- `manifest.json` の `resources` は `acapPackageConf` の**外側**、トップレベルに置く。
  中に入れると `acap-build` がスキーマ検証で落ちる（SDK 同梱の
  `application-manifest-schema-v2.1.0.json` で確認済み）。


- manifest に `video` グループ追加、app/Makefile に PKGS 追加
- `capture.c`: `vdo_stream_snapshot()` もしくは短命ストリームで NV12 1920x1080 を 1 枚。
  `vdo_stream_get_info()` から **実際の pitch と buffer.type** を取得してログに出す
  （pitch は 1920 とは限らない。buffer.type が `vmem` なら `larodConvertVmemFdToDmabuf()` が必要）
- `ppcomp.c`: `larodConnect()` → `larodListDevices()` で全デバイス名をログ出力

**ゲート**: ログに `cpu-proc` と `a9-gpu-proc` が出る／解像度・pitch・buffer.type が出る。
（スキルのデバイス名表は実機と食い違うので、必ず実測値を正とする）

### 2-B: 前処理の実装と**画像による正当性検証**（CLAUDE.md の必須チェック項目）

**完了（2026-07-31）— ユーザーが 5 枚すべて目視確認し「問題ない」と承認済み**

確定事項:

- **crop は job params で通った**（`cpu-proc` で確認）。`larodSetJobRequestParams()` +
  `larodRunJob()` だけで済み、**モデルのロードは 1 回**。2-C のループはこの前提でよい。
  モデル再ロードのフォールバックも実装済みだが**未テスト**。`a9-gpu-proc` が job params を
  受けるかは 2-C で初めて分かる。
- **`rgb-planar` の綴りはそのままで正しい**（`cpu-proc` が受理。エラー無し）。
  チャネル順は R,G,B（NCHW の C 次元）。
- 4 シナリオとも raw のバイト数がテーブルの期待値と完全一致。
- `memfd_create()` には `#define _GNU_SOURCE` が要る（glibc が `__USE_GNU` で隠している）。
- **2-A で `capture.c` に入れた `printf()` を削除した。** `ppcomp` では無害だったが、
  `test_preproc` の stdout raw ダンプに 141 バイト混入して画像が壊れた。共有ソースに
  stdout 出力を置かない。ログは syslog へ、stdout は呼び出し側が決める。

`verify/` に PNG と raw を保存。各 PNG は **別々の実行＝別フレーム**なので、
シーンの内容（車・歩行者の位置）はシナリオ間で少し違う。これは異常ではない。

`preproc.c` に 4 シナリオ共通の構築ロジック:

| シナリオ | input | output format | output size | row-pitch (bytes) | 出力バイト数 |
|---|---|---|---|---|---|
| crop    | nv12 1920x1080 | nv12             | 300x300   | 300      | 135000  |
| scale   | nv12 1920x1080 | nv12             | 300x300   | 300      | 135000  |
| rgb-i   | nv12 1920x1080 | rgb-interleaved  | 1920x1080 | 5760     | 6220800 |
| rgb-p   | nv12 1920x1080 | rgb-planar       | 1920x1080 | 1920     | 6220800 |

- 出力は各シナリオごとに `memfd_create()` で 1 つ確保して使い回す
- crop 位置は `larodCreateJobRequest()` の params マップに `image.input.crop`
  (IntArr4: x, y, w, h) で渡し、**モデルは 1 回ロードするだけ**にする。
  もし job params で受け付けられなければ、crop をモデルの map に入れて毎回ロードし直す
  （ロードは計測外なので数値の妥当性は保たれる）
- 出力フォーマット名 `rgb-planar` は未確認。拒否されたら larod のエラーメッセージをそのまま
  ログに出して正しい綴りを確定する

`test_preproc.c`（`run.sh` の `-a` で `<device> <scenario>` を受ける）:
- 元の NV12 と、各シナリオの処理後バッファを raw で stdout にダンプ
- ホスト側で ffmpeg で PNG 化し、**ユーザーに 4 シナリオ分すべて目視確認してもらう**

**ゲート**: ~~ユーザーが「元画像・処理後画像とも正しい」と確認するまで 2-C に進まない。~~
**2026-07-31 クリア。**

### 2-C: ベンチマークループ + 統計 — **完了（2026-07-31）**

実機結果（Q1728 / ARTPEC-9、warmup 3 除外、n=50）:

```
crop   cpu-proc    mean=0.997 ms  sd=0.074 ms  (n=50)
crop   a9-gpu-proc mean=3.870 ms  sd=0.172 ms  (n=50)
scale  cpu-proc    mean=9.408 ms  sd=0.103 ms  (n=50)
scale  a9-gpu-proc mean=4.602 ms  sd=0.151 ms  (n=50)
rgb-i  cpu-proc    mean=7.595 ms  sd=0.050 ms  (n=50)
rgb-i  a9-gpu-proc mean=9.036 ms  sd=0.103 ms  (n=50)
rgb-p  cpu-proc    mean=22.084 ms  sd=0.071 ms  (n=50)
rgb-p  a9-gpu-proc N/A (Could not run job: This backend only supports
                        color conversion from NV12 to RGB interleaved)
```

- **`a9-gpu-proc` も job params の crop を受理した。**モデル再ロードのフォールバックは
  実装済みだが**一度も発火していない＝未テストのまま**。
- **GPU の memfd 出力拒否は起きなかった。**dma-buf への切り替えは不要。
- `a9-gpu-proc` は **NV12→RGB interleaved の色変換しか持たない**（larod のエラー文より）。
  色変換を伴わない crop / scale（nv12→nv12）は GPU でも通る。
- GPU が勝ったのは **scale のみ**。crop と rgb-i は CPU の方が速い。
- 計測を `larodRunJob()` だけに絞るため `preproc_run()` を
  `preproc_prepare()`（未計測）と `preproc_run()`（計測）に分割した。

- crop 位置 50 点は**固定シードの LCG で事前生成**し、cpu-proc と a9-gpu-proc で同一列を使う。
  x ∈ [0,1620], y ∈ [0,780] で **偶数に丸める**（NV12 のクロマは 2x2 サブサンプルなので
  奇数オフセットは拒否されるか色ズレする）
- 各 (シナリオ × デバイス) で ウォームアップ3 + 計測50 回、平均と標本標準偏差を算出
- 出力形式（syslog / stdout 共通）:
  ```
  crop   cpu-proc    mean=1.234 ms  sd=0.056 ms  (n=50)
  crop   a9-gpu-proc mean=0.789 ms  sd=0.123 ms  (n=50)
  ...
  ```
- あるデバイスがある変換をサポートしない場合はクラッシュさせず `N/A (<larod のエラー文>)` と
  出して次へ進む

**ゲート**: ~~`view_log.sh ppcomp` に 8 行そろって出る。~~ **2026-07-31 クリア**（syslog / stdout 両経路）。

### 2-D: 仕上げ — **完了（2026-07-31）**

- `docker build --no-cache` で全ソース再コンパイルし、コンパイラ警告 **0 件**を確認。
  `ppcomp` / `test_preproc` とも生成・eap に梱包済み。
- **再現性を 4 回の独立実行で確認**（実行ごとに別フレームをキャプチャ）。
  run 間の mean の振れは run 内 sd と同程度で、いずれも 3% 未満。
  結論（GPU が scale で 2.1 倍速く、crop で 3.8 倍遅い）は誤差より 1 桁大きく、頑健。

| シナリオ | cpu-proc 平均 | a9-gpu-proc 平均 | 比 |
|---|---|---|---|
| crop  | 1.015 ms  | 3.838 ms | GPU が **3.8 倍遅い** |
| scale | 9.406 ms  | 4.487 ms | GPU が **2.1 倍速い** |
| rgb-i | 7.539 ms  | 8.985 ms | GPU が 1.2 倍遅い |
| rgb-p | 22.118 ms | N/A      | GPU 非対応 |

**結論**: 前処理を GPU に逃がす価値があるのは **scale だけ**。crop は CPU が圧倒的で、
planar RGB は GPU に選択肢が無い。

## 残っている既知の未整理事項

- crop のモデル再ロード・フォールバック経路は**一度も発火しておらず未テスト**。
  両デバイスとも job params を受理したため不要だったが、コードとしては残っている。
- `verify/` に raw ダンプが約 40 MB 残っている。うち `dump_*.raw` 4 ファイル（約 25 MB）は
  2-B の中間生成物で、最終的な PNG の生成には使われていない。削除可否はユーザー判断。
- `runMode: once` のため、**deploy 直後にアプリが自動で 1 回走る**。ログを読むときは
  明示実行ぶんと混同しないよう PID で区別すること。

## リスクと未確認事項

| 項目 | 内容 | 対処 |
|---|---|---|
| ~~`rgb-planar` の綴り~~ | **解消**: 2-B で `cpu-proc` が受理。綴りは正しい | — |
| ~~`image.input.crop` を job params で渡せるか~~ | **解消**: 2-C で両デバイスとも受理。モデルは 1 回ロード | フォールバックのコードは残っているが未テスト |
| ~~a9-gpu-proc が memfd 出力を受けるか~~ | **解消**: 2-C で memfd 出力のまま成功 | — |
| ~~a9-gpu-proc の nv12→nv12 crop 対応~~ | **解消**: crop / scale とも GPU で動く。落ちたのは色変換の rgb-planar のみ | — |
| ~~VDO の buffer.type~~ | **解消**: 2-A で `dmabuf` と確定。入力 fd の変換は不要 | — |

## 運用上の注意（既知の罠）

- `deploy.sh` はアプリのログをリセットする。必ず **deploy → run → log 参照** の順。
- `control.sh remove` は SSH ユーザーとパスワードごと消す。増分ごとに **上書きインストール**する。
- `vendorId` は変えない。変えると upload が `Error: 27` で落ちる。
