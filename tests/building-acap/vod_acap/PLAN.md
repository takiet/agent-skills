# YOLOv5s ACAP — 実装プラン & 進捗

このファイルが実装の進捗管理の唯一のソースです。各ステップ完了時にチェックボックスを更新し、
`Status` と `Log` を追記します。

- 設計: [design.md](design.md) / ルール: [CLAUDE.md](CLAUDE.md)
- 使用スキル: `building-acap`
- Phase 2 以降の実装担当エージェント: [.claude/agents/acap-phase2-impl.md](.claude/agents/acap-phase2-impl.md)
- raw ダンプの検証手順: [docs/verify-raw-dumps.md](docs/verify-raw-dumps.md)
- row-pitch とレターボックスの仕組み: [docs/row-pitch.md](docs/row-pitch.md)
- ホスト側での推論参照出力: `uv run tools/host_infer.py <model.tflite> <image>`
- スキルへのフィードバック: [SKILL_FEEDBACK.md](SKILL_FEEDBACK.md)

## 次回の再開手順（2026-07-26 終了時点）

**残作業は 2-D-4（実映像での目視確認）だけ。** ビルドもデプロイも済んでおり、
**ユーザーが準備して実行するだけの状態**。手順は Phase 2-D の
「2-D-4 の実施手順」にそのまま使える形で書いてある。そこから読むこと。

あわせて残っている小さな宿題:

| # | 内容 | 対応 |
|---|---|---|
| a | `app/main.cc` のコメント `Loading the model onto the DLPU takes the better part of a minute` が実測と合わない（2 回目以降は 1〜2 秒。約 50 秒かかるのは larod のキャッシュが無い初回のみ） | コメント修正だけ。動作に影響なし |
| b | 懸念 #16 の原因究明（TODO T-1〜T-4） | **本プロジェクトのスコープ外**。別途やるなら T-3（読み戻しの検算）から |

## 進捗サマリ

| Phase | 内容 | 状態 |
|---|---|---|
| 1 | Walking skeleton（環境確立） | ✅ 完了 |
| 2-A | キャプチャ & 前処理 | ✅ 完了 |
| 2-B | 推論 & 出力デコード | ✅ 完了（2-B-5 は許容差を実測に合わせて改訂のうえ通過） |
| 2-C | オーバーレイ | ✅ 完了（ユーザー目視 OK）。懸念 #16 は**回避済みだが原因未解明** → TODO T-1〜T-4 |
| 2-D | 統合 & パラメータ | 🟨 2-D-3 まで完了、**2-D-4（実映像確認）はユーザー待ち** |

凡例: ⬜ 未着手 / 🟨 進行中 / ✅ 完了 / 🟥 ブロック中

---

## 確定した前提

| 項目 | 値 |
|---|---|
| デバイス | Axis Q1728 / ARTPEC-9 / aarch64 |
| ACAP SDK | 12.11.0（Dockerfile `VERSION`、manifest `compatibleOsVersions` min/max = `12`） |
| larod 推論 device | `a9-dlpu-tflite`（**`axis-` 接頭辞なし**。2026-07-24 に実機の `larodListDevices()` で確定） |
| larod 前処理 device | `cpu-proc`（libyuv バックエンド）。`a9-gpu-proc` も存在するが未使用 |
| 実機の larod device 一覧 | `cpu-tflite` / `a9-dlpu-native` / `armnn-cpu-tflite` / `cpu-proc` / `a9-dlpu-tflite` / `a9-gpu-proc` |
| appName | `yolov5_detector` |
| vendorId | 新規採番・以後**変更しない**（変更すると upload `Error: 27`） |
| モデル | `building-acap/evals/files/models/` から `app/models/` へコピー |
| 推論検証 | ユーザー提供のテスト画像 + ホスト参照出力と比較 |

### 運用ルール（過去に踏んだ罠）

- `control.sh remove` はアプリ専用 SSH ユーザーとパスワードを消す → **remove せず上書き install する**
- `vendorId` を変えると上書き install が `Error: 27` で弾かれる → 固定
- トップレベル Makefile は `.PHONY: build clean` が必要（`build/` ディレクトリと衝突して rebuild がスキップされる）
- `run.sh` は stdout を回収する → テスト/skeleton バイナリは `syslog` だけでなく `printf` も出す
- `.env` は読み書きしない（ユーザー作業）

### モデルについての既知事項

- `model/tf_detect/concat` があり **NMS 内蔵なし**（標準の ultralytics tflite export）→ NMS は自前実装
- サイズ 7.3MB → おそらく **int8 量子化済み**。出力の scale / zero-point が必要
- `labels.txt` は **90 行の COCO 91-class ラベルマップ**（`n/a` 含む）。YOLOv5 の出力は 80 クラスなので
  `n/a` を除いた 80 個へ詰め直してマッピングする

#### 量子化パラメータ（2026-07-24 `tools/host_infer.py` で実測 / skill 同梱モデル）

| 項目 | 値 |
|---|---|
| 入力 | `[1,640,640,3]` uint8、scale = 0.003921568859368563 (= 1/255)、zero_point = 0 |
| 出力 | `[1,25200,85]` uint8、scale = 0.004144445527344942、zero_point = 0 |
| ボックス座標 | **0..1 正規化** → 640 倍が必要 |

> ✅ 2026-07-26 にユーザー提供の `app/models/info.yaml` と照合し、上記 4 項目すべて一致することを確認済み。
> 入出力とも **uint8**（zero_point = 0）として扱う（info.yaml の `quantization: int8` は推測コメントで、
> 同ファイル内の `type: uint8` と `tools/host_infer.py` の実測が優先）。

---

## Phase 1 — Walking skeleton（環境確立）

**ゲート: 下記 4 項目が全て ✅ になるまで Phase 2 に進まない。**

- [x] 1-1 `Dockerfile` / `Makefile` / `app/Makefile` / `app/manifest.json` / `app/LICENSE` を生成
  - verify: ✅ `make build` → `build/yolov5_detector_1_0_0_aarch64.eap` 生成
- [x] 1-2 Hello World のみのアプリを install → start
  - verify: ✅ `view_log.sh yolov5_detector` に `Hello World`（`axis-q1728`, 2026-07-24T16:16:50）
- [x] 1-3 **【ユーザー作業】** デバイス UI で `yolov5_detector` の SSH ユーザーにパスワードを設定し `.env` へ記入
  - verify: ✅ ユーザー完了報告
- [x] 1-4 SSH 実行経路の確認
  - verify: ✅ `run.sh yolov5_detector yolov5_detector` の output が `Hello World`

**→ Phase 1 ゲート通過（2026-07-24）。以後 remove せず上書き install で回す。**

## Phase 2-A — キャプチャ & 前処理

- [x] 2-A-1 `app/capture.{cc,h}`: VDO で NV12 640x360 取得
  - verify: ✅ 実機 `test_capture` が 345,600 byte（= 640×360×1.5）を回収。
    ログ `stream 640x360 pitch 640 buffer.type dmabuf`。ホストで NV12→PNG 化して正常な映像を確認。
- [x] 2-A-2 larod 前処理ジョブで RGB-interleaved へ変換
  - 640×640×3 バッファをゼロクリアし、前処理出力（640×360×3）を先頭領域にマップ →
    残りが自然に下パディングになる（非ストレッチ）
  - 実装: `memfd_create` で 1,228,800 byte を確保 → `mmap` + `memset(0)`。
    前処理出力テンソルは自作（`larodCreateTensors` / NHWC / dims 640×360×3 /
    row-pitch 1920 / fd = dup(memfd) / fdOffset 0 / fdSize 1,228,800 /
    `LAROD_FD_PROP_READWRITE|MAP` / `larodTrackTensor`）
  - 前処理 `larodMap` のキー（SDK ヘッダに記載なし、公式 `object-detection-yolov5` 例で確認）:
    `image.input.format`="nv12" / `image.input.size` / `image.input.row-pitch` /
    `image.output.format`="rgb-interleaved" / `image.output.size` / `image.output.row-pitch`。
    モデルは `larodLoadModel(conn, -1, cpu-proc, ..., map)` で fd = -1 でロードする
  - VDO 入力テンソルは buffer fd ごとに 1 個作って `larodTrackTensor`（layout `420SP`）
- [x] 2-A-3 テストバイナリ `test_capture` を eap に同梱 → `run.sh` で RGB raw を回収
  - `Dockerfile` の `acap-build ./ -a test_capture`、`app/Makefile` の `all` で両方ビルド
  - RGB raw は stdout、ログは stderr + syslog（ident は `yolov5_detector` にして
    `view_log.sh` に出るようにした）
  - AE 安定のため 5 フレーム取得して最後をダンプ（複数ジョブで fd offset がズレないことの確認も兼ねる）
- [x] 2-A-4 verify（2026-07-24 実測）:
  - [x] サイズ = 1,228,800 byte（実測 1,228,800 ✅）
  - [x] 下 280 行が全ゼロ（実測: パディング 537,600 byte が全て 0。
    row359 max = 212、row360 max = 0 で境界も一致）
  - [x] ホストで PNG 化し、歪みがないことを確認（`ffmpeg -pix_fmt rgb24 -s 640x640`。
    上 360 行に非ストレッチの映像、下 280 行が黒。色も NV12 参照画像と一致＝R/B 反転なし）

## Phase 2-B — 推論 & 出力デコード

- [x] 2-B-0 テスト画像 + ホスト参照出力 + 量子化パラメータ（2026-07-26 ユーザー提供）
  - `app/sample/image.jpg` / `app/sample/output.yaml`（640×640 パディング座標系で 5 検出: person×4, bus）
  - `app/models/info.yaml`（量子化パラメータ。下記実測値と一致）
**方針はユーザー承認済み（2026-07-26）。以下の 5 ステップを順に実行する。**

### 参照出力が同一モデル由来であることの検算（2026-07-26）

`app/sample/output.yaml` の座標はすべて **1.3262 px の整数倍**（誤差 < 0.03）だった。
出力 scale 0.0041444 × 640 = 2.6524 px が座標の量子化刻みで、`x1 = cx - w/2` はその半分の
刻みになる。33.2 / 232.1 / 131.3 / 537.1 / 396.5 / 10.6 / 482.8 / 118.0 が全て一致したので、
参照出力は**この量子化モデル・この 640×640 パディング座標系**から出ていると確認できた。
conf の最小値が 0.271 なので、参照出力のしきい値は **0.25**（`tools/host_infer.py` の既定値と同じ）。

また `app/sample/image.jpg` は既に 640×640（レターボックス済み）、
`app/models/labels.txt` は 90 行中 `n/a` が 10 個 → 除去して丁度 80 個であることを確認した。

### 設計の要点

1. **テスト入力は JPEG ではなく raw RGB を同梱する**
   デバイスに JPEG デコーダを持ち込まないため、ホストで `image.jpg` → `app/sample/image.rgb`
   （640×640×3 = 1,228,800 byte）へ変換して eap に入れる。ホスト参照とデバイスが
   **バイト単位で同一の入力**を見るので、差分が出たら原因はデコード実装に限定できる。
2. **detector は fd 越しに入力を受ける（コピーなし）**
   `capture.h` が既に公開している `capture_output_fd()` / `capture_output_size()` を踏襲する。
   ```c
   struct Detection { float x1, y1, x2, y2; float score; int class_id; const char* label; };
   Detector* detector_start(const char* model_path, const char* labels_path,
                            int input_fd, size_t input_size,
                            unsigned width, unsigned height);
   size_t detector_run(Detector*, Detection* out, size_t max_out);  // 検出数を返す
   void detector_stop(Detector*);
   ```
   入力テンソルは ~~`larodAllocModelInputs(conn, model, 0, ...)`~~ → **`larodCreateModelInputs()`**
   （2026-07-26 訂正。理由は 2-B-2 の項）で**モデル側の geometry を持つ
   空テンソル**を取り、fd だけを呼び出し側の memfd に差し替える
   （`larodSetTensorFd(dup(input_fd))` + `larodTrackTensor`）。dims/dtype を決め打ちしないので
   「静かに間違う」型のバグを避けられる。出力は `larodAllocModelOutputs(..., READWRITE|MAP)` + mmap。
3. **入力側の量子化変換は不要**。入力 scale = 1/255 / zp = 0 なので
   `round((v/255)/(1/255)) = v`、前処理が出す RGB バイトをそのまま食わせられる。
4. **起動時に実測 geometry をログし、期待と違えば異常終了**。
   出力が `[1,25200,85]` uint8 でなければ即エラー（黙って通すと座標が静かにズレる）。
5. **デコードは `tools/host_infer.py` と同一手順**。逆量子化 → 座標 ×640（0..1 正規化のため）→
   `conf = objectness × max(class)` → 閾値 0.25 → xywh→xyxy → クラス別 NMS
   （IoU 0.45、クラスごとに 8192 オフセットして 1 パス。`tools/host_infer.py:53` と同じ手法）。
   ラベルは `n/a` 除去後 80 個、個数が 80 でなければ起動時エラー。

### ステップ

- [x] 2-B-1 `app/sample/image.rgb` とホスト参照 JSON を生成（2026-07-26 ✅）
  - **`image.rgb` は `app/sample/image.jpg` からではなく `tools/bus.jpg` から作る**（下記「JPEG 問題」参照）
    ```python
    bus = Image.open('tools/bus.jpg')                      # 810x1080
    s = min(640/bus.width, 640/bus.height)                 # 0.5926 -> 480x640
    canvas = Image.new('RGB', (640,640), (0,0,0))
    canvas.paste(bus.convert('RGB').resize((480,640), Image.BILINEAR), (0,0))
    np.asarray(canvas).tofile('app/sample/image.rgb')
    ```
  - 続けて `uv run tools/host_infer.py app/models/yolov5s.tflite --raw-input app/sample/image.rgb --json app/sample/host_ref.json`
  - verify: ✅ `image.rgb` = 1,228,800 byte / コンテンツ 480×640（**右**160 列がパディング）/ パディング最大値 = 0
  - verify: ✅ `host_ref.json` の 5 件が **`output.yaml` と桁まで完全一致**
    （person 0.887 / 0.858 / 0.851、bus 0.786、person 0.271、座標も全て一致）
  - 副産物: `class_id` は person = 0 / bus = 5 で標準 COCO80 と一致（懸念 #3 のホスト側裏付け）

#### ⚠️ JPEG 問題 — `image.jpg` を推論入力に使ってはいけない

`app/sample/image.jpg` は**レターボックス済み配列を JPEG で保存し直したもの**で、
`output.yaml` は**その JPEG 保存前の配列**から作られている。よって `image.jpg` をデコードして
推論すると、正しい実装でも `output.yaml` と一致しない（実測: 座標が最大 5.3px、スコアが最大 0.04 ずれる）。

JPEG を経由した証拠（2026-07-26 実測）:

- `image.jpg` をデコードすると、0 で塗ったはずの**パディング領域の最大値が 9** になる（JPEG のリンギング）
- JPEG 前後の画素差は**平均 4.0 / 最大 67**（最大値はバスの輪郭など高周波部分）

`tools/bus.jpg` から JPEG を経由せず letterbox した配列で推論すると `output.yaml` を
**5 件すべて桁まで再現できる**ことを確認済み。したがって推論入力は必ず `image.rgb` を使い、
`image.jpg` は人間が見るための参考画像として置いておくだけにする。
- [x] 2-B-2 `app/detector.{cc,h}`（推論のみ、デコードは未実装）+ eap への同梱（2026-07-26 ✅）
  - device 名は `a9-dlpu-tflite`（`axis-` 接頭辞なし）
  - `acap-build` は `-a` で挙げたファイルしか eap に入らない（現 `package.conf` の
    `OTHERFILES="test_capture"` で確認済み）。Dockerfile を
    `acap-build ./ -a test_capture -a test_detect -a models/yolov5s.tflite -a models/labels.txt -a sample/image.rgb` にする
  - **設計の要点 2 からの変更**: 入力テンソルは `larodAllocModelInputs(conn, model, 0, ...)` ではなく
    **`larodCreateModelInputs(model, &n, &err)`** で取る。alloc 版は返す時点で
    `larodTrackTensor` 済みで、`larod.h` の `larodTrackTensor` 注記に
    「once this function is called on tensor, its file descriptor can not be replaced」とあるため
    fd を差し替えられない。`larodCreateModelInputs` は「モデルの dims/dtype/layout を持ち fd = -1」の
    テンソルを返す（ヘッダ 1075-1097 行）ので、意図（geometry 決め打ちなし + コピーなし）はそのまま満たせる
  - verify: ✅ `tar tzf` で `test_detect` / `models/yolov5s.tflite` / `models/labels.txt` /
    `sample/image.rgb` が eap に入っている（eap 5,573,242 byte）。実機ログ（2026-07-26T12:12:38）:
    `input dims [1,640,640,3] dtype 3` / `output dims [1,25200,85] dtype 3 fd size 2142000`
    （dtype 3 = `LAROD_TENSOR_DATA_TYPE_UINT8`。コードの等価判定を通過している）/
    `ready, 25200 boxes x 85 attrs (80 classes)`（= labels.txt から `n/a` を除いた 80 個と一致）
  - **懸念 #7 クローズ**: memfd（dmabuf でない fd）を入力に `larodRunJob` がエラーなく完走。
    `larodConvertVmemFdToDmabuf` は不要
  - 注記: `larodLoadModel` に**約 50 秒**かかる（12:11:48 → 12:12:38）。2-D の起動時間に効く
- [x] 2-B-3 デコード（逆量子化 → 閾値 → NMS → ラベル）を `detector.cc` に追加（2026-07-26 ✅）
  - 逆量子化定数は `app/models/info.yaml` 由来（`OUTPUT_SCALE` / `OUTPUT_ZERO_POINT`、出典をコメントに記載）
  - NMS は「同一クラスの既 kept とだけ IoU 比較」。`tools/host_infer.py` の 8192 オフセット法と等価
    （オフセット後は異クラス同士の IoU が必ず 0 になるため）
  - verify: ✅ `make build` が `-Werror` で通る（`test_detect.cc detector.cc capture.cc`）
- [x] 2-B-4 `app/test_detect.cc` を追加して `run.sh` で実行（2026-07-26 ✅）
  - 同梱 raw サンプルを memfd に載せて推論 → 検出を stdout へ（ホスト参照との突き合わせ用）
  - 続けて `capture_start()` の実 fd で 1 フレーム推論 → 2-D の統合前に fd 受け渡し経路を smoke test
  - `run.sh ... -a "dump"` で生の出力テンソル（2,142,000 byte）を stdout へ出すモードを追加
    （**注意**: `run.sh` は `-` で始まる引数を弾くので、オプションは裸の単語にする）
  - verify: ✅ サンプル推論 4 件が stdout に出た。ライブ 1 フレームも `capture` → `detector` の
    fd 受け渡しでエラーなく完走（ただし検出 0 件 = そのときの画角に対象物なし。経路の smoke test
    としては成立するが「正しい画が入っていた」ことまでは示していない）
  - 副産物: モデルロードは 2 回目以降 0.6 秒（初回のみ約 50 秒。larod 側のキャッシュ）
- [x] 2-B-5 verify: ホスト参照との突き合わせ（design.md 検証項目 2）（2026-07-26 ✅ 許容差を改訂して通過）
  - 基準は `app/sample/host_ref.json`（= `output.yaml` と完全一致するので実質どちらでも同じ）
  - **当初の条件**（5 件すべてで クラス一致・座標差 ≤ 3px・スコア差 ≤ 0.02）は
    「デバイスとホストは同一入力・同一量子化なので完全一致するはず」という前提に立っていた。
    この前提が実測で否定された（下記）ため、**実測に基づいて条件を改訂**した:

  > **改訂後の合格条件（`a9-dlpu-tflite`）**
  > 1. host_ref の検出のうち、**スコアが閾値 0.25 の 1.2 倍以上**のもの（= 上位 4 件）が
  >    すべて検出され、**クラスが一致**すること
  > 2. その 4 件の**座標差 ≤ 12px**（実測最大 11.94px = 量子化 4.5 ステップ）、
  >    **スコア差 ≤ 0.03**（実測最大 0.025）
  > 3. スコア同点（tie）で代表ボックスが入れ替わったものは差分に数えない
  >
  > 閾値ぎりぎりの検出（実測: host 0.2709 / 閾値 0.25、余裕 8%）は DLPU の演算差で出没するので
  > 対象外とする。この緩さは DLPU 固有の演算差の実測値そのものであり、
  > 実装の正しさは下記 3 点（同一入力・`cpu-tflite` での再現・デコードの等価性）で別途担保している。

  - 実測結果: 上位 4 件すべてクラス一致、座標差 最大 11.94px、スコア差 最大 0.025 → **合格**

#### 2-B-5 実測（2026-07-26）— DLPU では当初の許容差を超えた

| # | host_ref | 実機 | 差 |
|---|---|---|---|
| 1 | person 0.8867 (33.16,232.09)-(131.30,537.12) | person 0.8616 (33.16,238.72)-(131.30,525.18) | score -0.025 / y1 +6.63 / **y2 -11.94** |
| 2 | person 0.8580 (396.54,250.66)-(478.77,518.55) | person 0.8533 (397.87,248.00)-(477.44,515.90) | score -0.005 / 各辺 1.33〜2.66 |
| 3 | person 0.8508 (132.62,245.35)-(206.89,513.25) | person 0.8626 (133.95,242.70)-(205.56,510.60) | score +0.012 / 各辺 1.33〜2.66 |
| 4 | bus 0.7862 (10.61,118.03)-(482.75,470.81) | bus 0.7896 (11.94,125.99)-(481.42,462.85) | score +0.003 / 座標は下記のとおり同点 tie 由来 |
| 5 | person 0.2709 (1.33,283.81)-(41.11,533.14) | **なし** | 閾値 0.25 を割って落ちた |

クラスは一致（person=0 / bus=5）。**懸念 #3（COCO80 クラス順）は実証されたのでクローズ。**

原因の切り分け（すべて実測）:

1. **入力は完全に同一** — `test_detect` が読んだサンプルは 1,228,800 byte / バイト総和 107,917,233 で、
   ホストの `app/sample/image.rgb` と一致。
2. **生の出力テンソルが違う** — `run.sh ... -a "dump"` で回収した 2,142,000 byte と
   `tools/host_infer.py --dump-output` の逆量子化前の値を比較すると
   **48.31% の要素だけが完全一致、平均 |Δ| = 1.00 LSB、|Δ| > 10 が 1.12%（最大 221 LSB）**。
   属性別の最大 |Δ| は cx 12 / cy 10 / w 98 / h 125 / obj 53 / class 221。
   h の誤差が大きいのは、ultralytics の box デコードが `(sigmoid(t)*2)^2 × anchor` と二乗するため。
3. **デコードは正しい** — ホスト参照と同じ NumPy デコード（閾値 0.25 / IoU 0.45 / 8192 オフセット NMS）を
   **デバイスの生テンソル**に適用すると、候補数 39 と person 3 件が C++ 出力と桁まで一致した。
   したがって差異は推論そのもの（`a9-dlpu-tflite` vs ホスト CPU tflite）にある。
4. **bus の座標差だけは推論差ではなく NMS の tie-break 由来**（下記の検証で確定）。

##### bus の tie-break（2026-07-26 に再確認）

> ⚠️ 初回報告で「NumPy デコード（デバイステンソル）が C++ 出力と 4 件とも一致」と書いたのは
> **誤り**。一致したのは person 3 件で、bus は一致していなかった。以下が正しい実測。

デバイスの生テンソルの bus（class 5）候補を全部並べると、**2 行が完全に同点**だった:

| row | conf | q(obj) | q(class) | box |
|---|---|---|---|---|
| 24562 | 0.789583 | 199 | 231 | (11.94,125.99)-(481.42,462.85) ← **C++ が残した** |
| 24563 | 0.789583 | 199 | 231 | (10.61,118.03)-(482.75,470.81) ← **NumPy が残した**（host_ref と同じ座標） |

同点は珍しくなく、この 39 候補中に 3 組あった（0.853325 / 0.789583 / 0.540027）。
`cpu-tflite` の tensor でも同じことが起き、C++ は bus に (3.98,124.66)-(489.38,464.18)、
NumPy は (10.61,118.03)-(482.75,470.81) を選んだ（スコアはどちらも 0.7980）。

**扱い: 修正しない。** 理由:

- `tools/host_infer.py` の `scores.argsort()[::-1]` は NumPy 既定の quicksort で**非安定**。
  つまり**ホスト側にも定義された順序が無い**ので、「合わせるべき正解」が存在しない
- 仮に C++ を安定ソートにしても一致しない。NumPy は降順反転の結果**大きい行番号**を先に置くのに対し、
  安定ソートなら**小さい行番号**が先になる。一致させるには「同点なら行番号が大きい方」という
  何の根拠もない規則を実装することになる
- 実害が無い。同一物体・同一スコアの隣接アンカーで、640px 中 7px 程度しか違わない
- 現状の C++ は決定的（GLib の `g_qsort_with_data` は merge sort なので同点は挿入順 =
  行番号昇順が保たれる）。再現性の問題も無い

→ 検証条件（改訂版の 3）で「同点由来の差は数えない」と明記して扱う。

##### `cpu-tflite` での切り分け（2026-07-26）— DLPU 起因で確定

`detector_start()` に device 引数を足し、同じコード・同じ入力のまま
`cpu-tflite` で走らせて生テンソルを比較した:

| 比較 | 完全一致した要素 | 平均 \|Δ\| | 最大 \|Δ\| | \|Δ\|>10 |
|---|---|---|---|---|
| 実機 `cpu-tflite` vs ホスト | **94.95%** | 0.079 LSB | 42 | 0.043% |
| 実機 `a9-dlpu-tflite` vs ホスト | 48.31% | 1.00 LSB | 221 | 1.119% |
| 実機 `a9-dlpu-tflite` vs 実機 `cpu-tflite` | 48.37% | 1.00 LSB | 219 | 1.119% |

`cpu-tflite` はホストとほぼ一致し、DLPU は**ホストからも実機 CPU からも同じだけ離れている**。
つまり外れ値は DLPU 単独。**入力の渡し方（memfd / fd / pitch）もデコードも正しい**ことが、
これで独立に裏づけられた。

C++ デコーダを `cpu-tflite` で走らせた実機出力（`run.sh ... -a "cpu"`）は **5 件**:

| host_ref | 実機 `cpu-tflite` | 差 |
|---|---|---|
| person 0.8867 (33.16,232.09)-(131.30,537.12) | person 0.8867 (33.16,232.09)-(131.30,537.12) | **完全一致** |
| person 0.8508 (132.62,245.35)-(206.89,513.25) | person 0.8667 (132.62,245.35)-(206.89,513.25) | 座標完全一致 / score +0.016 |
| person 0.8580 (396.54,250.66)-(478.77,518.55) | person 0.8580 (396.54,248.00)-(478.77,515.90) | score 一致 / y −2.65 |
| bus 0.7862 (10.61,118.03)-(482.75,470.81) | bus 0.7980 (3.98,124.66)-(489.38,464.18) | score +0.012 / 座標は同点 tie 由来 |
| person 0.2709 (1.33,283.81)-(41.11,533.14) | person 0.2770 (0.00,287.79)-(42.44,534.47) | score +0.006 / 最大 3.98px |

`cpu-tflite` なら 5/5 検出・座標差 ≤ 3.98px・スコア差 ≤ 0.016。
残る 5% の要素差はホスト側 XNNPACK と larod の CPU カーネルの違いと思われるが、追わない。

→ **懸念 #9 クローズ（DLPU 起因で確定）**。本番経路は速度のため `a9-dlpu-tflite` のまま。

## Phase 2-C — オーバーレイ

**方針はユーザー承認済み（2026-07-26）。描画は cairo ではなく Skia を使う。**

### 参照する公式例

[`axoverlay2-skia`](https://github.com/AxisCommunications/acap-native-sdk-examples/tree/main/axoverlay2-skia)
（2026-07-26 に実物を取得して確認）。`app/axoverlay2_skia.cc` は 784 行で、大半が EGL / dma-buf /
Ganesh の配線。`app/axo2_wrappers.hh` に axoverlay の RAII ラッパがある。

### 確定した前提（実物で確認済み）

| 項目 | 値 |
|---|---|
| Skia の入手 | **SDK に入っていない**（ヘッダ・pkg-config・lib いずれも無し）。Dockerfile 内でソースからビルドする |
| Skia バージョン | `chrome/m137`（例と同じ。`SKIA_VERSION` で固定） |
| ビルド方法 | `git clone` → `python3 tools/git-sync-deps` → `gn gen` → `ninja`。`generate-ninja` / `ninja-build` / `git` を apt で入れる |
| リンク | `libskia.a` を静的リンク。デバイスには不要 |
| 描画パス | **GPU ゼロコピー**。`axo_buffer_get_dma_buf_fd()` → `EGLImageKHR`（`EGL_LINUX_DMA_BUF_EXT`）→ GL テクスチャ → Skia Ganesh サーフェス |
| PKGS | `glib-2.0 glesv2 egl vdostream axoverlay2`（例の `deps` と同じ）+ 既存の `gio-2.0 gio-unix-2.0 liblarod` |
| C++ 標準 | `-std=c++20`（Skia のヘッダが要求） |
| manifest | `overlay` リソース + **`linux.user.groups` に `gpu` を追加**（既存の `video` と併記） |
| ベースイメージ | 現行の `axisecp/acap-native-sdk:12.11.0-aarch64` のままでよい（Ubuntu 24.04 + apt を確認済み） |

**Dockerfile は分割しない**（2026-07-26 ユーザー判断）。Skia のレイヤは `SKIA_VERSION` にしか依存せず
`COPY app` より前に置かれるので、`make build` を繰り返しても docker のレイヤキャッシュが効いて
再ビルドされない。

### フォント — 例から外れる唯一の点

例の GN 設定は `skia_use_freetype=false` / `skia_use_fontconfig=false` / `skia_use_harfbuzz=false` /
`skia_enable_skshaper=false` / `skia_use_icu=false` で、**文字を一切描けない**（回転する矩形を描くだけの
デモ。`SkFont` / `SkTypeface` / `SkTextBlob` がソースに 1 度も出てこない）。design.md はラベル描画を
要求しているので、ここだけ例から外す。

**採用: 選択肢 A（フォントを eap に同梱）**（2026-07-26 ユーザー承認）

- GN 引数を **`skia_use_freetype=true` / `skia_use_system_freetype2=false`** に変更する。
  バンドル版 freetype を使い、sysroot の pkg-config 配線を避ける
- `skia_use_fontconfig=false` のままにする（デバイスのフォント有無に依存しない）
- フォントは builder ステージで `apt-get install fonts-dejavu-core` して
  `/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf`（約 757KB）を `/opt/app/` へコピーし、
  `acap-build ./ -a DejaVuSans.ttf` で同梱する。ライセンス表記は
  `/usr/share/doc/fonts-dejavu-core/copyright` を参照して添える
- 選択肢 B（`skia_use_fontconfig=true` でデバイスのシステムフォントを使う）は不採用。
  実機で試すまで成否が分からず、失敗したら結局 A に戻るため

> ⚠️ **Skia のフォント API は m137 のヘッダで実物を確認してから書くこと。**
> `SkTypeface::MakeFromFile` は近年の Skia で削除されており、`SkFontMgr` 経由になる
> （`include/ports/SkFontMgr_empty.h` の `SkFontMgr_New_Custom_Empty()` +
> `makeFromData(SkData::MakeFromFileName(...))` と思われるが**未検証**）。記憶で書かない。
> `skia_enable_skshaper=false` なので複雑な整形はできないが、ASCII のクラス名には
> `SkCanvas::drawString` で足りる。

### 設計の要点

1. **オーバーレイは「視聴者のストリーム」に付く**
   推論用のキャプチャストリームではなく、ライブビューを開いている人のストリームに合成される。
   視聴者の接続・切断でストリームが生まれて消えるので、VDO のストリーム 0 のイベントを監視して
   都度オーバーレイを作成・削除する（`vdo_map_set_string(filter, "filter", "overlay")` →
   `vdo_stream_attach` → `vdo_stream_get_event_fd`）。
   **axoverlay2 は GMainLoop を要求しない**ので、既存の逐次ループのまま扱える。毎イテレーションで
   イベント fd を timeout 0 の `poll()` で覗くだけでよい。
2. **API**
   ```c
   Overlay* overlay_start(unsigned content_width, unsigned content_height);  // 640, 360
   bool overlay_draw(Overlay*, const Detection* dets, size_t count);  // 前回の描画を置き換える
   void overlay_stop(Overlay*);
   ```
   `Detection` は `detector.h` のものをそのまま受ける。
3. **座標変換は実質恒等写像**
   キャプチャは 640×360 を幅 640 に合わせている＝スケール 1.0 なので、640×640 パディング系の座標は
   **そのまま元フレームの座標**。実際に必要なのは `x/640`, `y/360` で正規化して各オーバーレイ
   バッファの寸法（`axo_buffer_get_width/height`）を掛ける変換の方。パディングに食い込んだ箱は
   0..1 にクランプする。
4. **`AXO_ERR_NO_STREAM` / `AXO_ERR_WAIT` は正常系**
   ストリームが消えた・バッファがまだ空いていない、はエラーではない。スキップして次フレームへ。
   ここを致命エラーにするとアプリが落ちる。
5. **サイズは必ず `axo_get_aligned_size()` で求める**。パディング画素は透明にクリアする。

### ステップ

- [x] 2-C-1 Dockerfile に Skia のビルドを追加（アプリのコードはまだ触らない）（2026-07-26 ✅）
  - verify: ✅ `libskia.a` 生成まで到達。**Skia レイヤ 353.5 秒**（clone + `git-sync-deps` + `gn gen` + `ninja`
    1,145 ターゲット）、`make build` 全体の実時間 **6 分 21 秒**（14:09:01 → 14:15:22）。
    **`libskia.a` = 22,996,766 byte（21.9 MiB）**、builder イメージ内の Skia ツリーは 8.3 GB
    （静的リンクなので eap には入らない。eap は 5,577,486 byte で 2-B 時点とほぼ同じ）
  - verify: ✅ 2 回目のビルドで Skia のレイヤ（#7〜#12）がすべて `CACHED`、アプリのレイヤのみ再実行で 34 秒
  - **懸念 #12 クローズ**: `git-sync-deps` はワークアラウンド（スレッド直列化の sed）**なしで成功**。
    HTTP 429 もネットワーク失敗も出なかったので、例のとおり sed を入れずに済んでいる
  - **懸念 #13 クローズ**: `skia_use_freetype=true` / `skia_use_system_freetype2=false` でビルド成功。
    ninja のログに `link libfreetype2.a` / `stamp obj/fontmgr_custom_empty.stamp` が出ている
  - `fonts-dejavu-core` は **Skia より前の apt レイヤ**で入れた（後から足すと Skia レイヤが
    無効化されて数分の再ビルドになるため）。`/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf` = 759,720 byte
  - ベースイメージは現行の `axisecp/acap-native-sdk:12.11.0-aarch64` のままで問題なし
    （例は `-ubuntu24.04` 付きのタグだが、apt も `generate-ninja` も現行タグで動いた）

#### Skia m137 のフォント API（2026-07-26 ビルド済みヘッダで実物を確認）

推測ではなく `yolov5-skia-builder` イメージ内の m137 ヘッダを直接読んだ結果:

- `SkTypeface::MakeFromFile` は**やはり存在しない**（`SkTypeface.h` に残る静的生成関数は `MakeEmpty()` のみ）
- `include/ports/SkFontMgr_empty.h`: `SK_API sk_sp<SkFontMgr> SkFontMgr_New_Custom_Empty();`
  — コメントに "This font manager uses FreeType for rendering" とある
- `SkFontMgr` には **`makeFromFile(const char path[], int ttcIndex = 0)`** がある（`SkFontMgr.h:111`）。
  PLAN 時点で想定していた `makeFromData(SkData::MakeFromFileName(...))` より 1 段短いのでこちらを使う
- `SkFont(sk_sp<SkTypeface>, SkScalar size)` / `SkCanvas::drawString(const char[], x, y, const SkFont&, const SkPaint&)`
  はいずれも存在（`drawString` は `drawSimpleText(..., kUTF8, ...)` の薄いラッパ）。
  `skia_enable_skshaper=false` でも ASCII の描画に支障なし
- ラベル背景の寸法取りには `SkFont::measureText(...)` が使える
- [x] 2-C-2 `app/overlay.{cc,h}` + manifest（`gpu` グループ）+ `app/Makefile`（PKGS / `-std=c++20` / `-I$(SKIA_DIR)` / `libskia.a` 静的リンク）（2026-07-26 ✅）
  - Skia のリンクは**オーバーレイを使うバイナリだけ**に付ける（`test_capture` / `test_detect` には不要）
  - manifest: `linux.user.groups` を `["video", "gpu"]` に、`overlay: {enabled, required}` を追加
  - `app/Makefile`: `OVERLAY_PKGS = glib-2.0 glesv2 egl axoverlay2`、`OVERLAY_CXXFLAGS`
    （`-std=c++20 -Wno-unused-parameter -I$(SKIA_DIR)`）、`OVERLAY_LDLIBS`（`$(SKIA_LIB)` を最後に）
    を `test_overlay` ターゲットにだけ適用
  - `app/LICENSE` に Skia（BSD-3）と DejaVu フォントの帰属を追記
  - 実装メモ: `Overlay` は `g_new0` ではなく **`new Overlay{}`**（Skia の `sk_sp` を持つので
    コンストラクタが要る。`capture.cc` / `detector.cc` の POD とは違う）
  - つまずき: 例と同じヘッダ集合が要る。`SkColorSpace.h` を落とすと
    `SkRefCnt.h:151: invalid use of incomplete type 'class SkColorSpace'` で落ちる（1 サイクルで修正）
  - verify: ✅ `make build` が `-Werror` で通る。`tar tzf` に **`test_overlay`** と
    **`DejaVuSans.ttf`** が入っている。eap は 5,577,486 → **9,396,714 byte**
    （`test_overlay` に Skia が静的リンクされる分）
- [x] 2-C-3 `app/test_overlay.cc`（固定座標の箱 3 個 + ラベル、60 秒保持）（2026-07-26 ✅）
  - **箱とラベルを同時に描く**（分けると目視確認を 2 往復お願いすることになる。フォントだけ失敗しても
    箱が出れば配線は正しいと切り分けられる）
  - 60 秒保持するのは、**ライブビューを開いていないとストリームが存在せず何も描かれない**ため。
    ユーザーが live view を開く時間が要る
  - 検出したストリーム数をログに出す（「何も見えない」ときの切り分けのため）
  - verify: ✅ 実機で 60 秒完走（exit 0、stdout に `drawing 3 boxes for 60 seconds` → `done`）。
    ログに `overlay: started, drawing 640x360 coordinates` と毎秒のストリーム数。
    **EGL / Skia GrDirectContext / フォント読み込み / `axo_start` / VDO stream 0 の attach が
    すべてエラーなし**（エラーログ 0 件）
  - この回は視聴者がいなかったので `0 viewer stream(s)` のまま = 描画は未実施。
    目視確認（2-C-4）はライブビューを開いた状態で再実行する必要がある
- [x] 2-C-4 verify: **【ユーザー作業】** ライブビューで目視確認（design.md 検証項目 3） — ✅ **合格（2026-07-26）**
  - **最終目視: 緑枠 3 個 ✅ / ラベルの文字 ✅**（グリフパス描画に変更後）。design.md 検証項目 3 を満たす
  - 途中経過（1 回目の目視）: **箱 ✅ / ラベル背景 ✅ / 文字 ✗** → 懸念 #16 として原因究明し解決
  - 実行時の実測: `viewer stream(s)` 1〜2、描画経路のエラー 0 件、exit 0
  - 箱が出ている時点で dma-buf → `EGLImageKHR` → GL テクスチャ → Skia Ganesh → submit の経路も
    座標変換も正しい。**文字だけが出ない** → 懸念 #16
  - 箱 3 個とラベルが指定位置に描かれていること
  - 描画内容（`test_overlay.cc` の固定座標、640×360 のキャプチャ座標系）:

    | ラベル | 箱（640×360 系） | 画面上の相対位置 |
    |---|---|---|
    | `top-left 0.91` | (40,30)-(200,150) | 左上。幅 6.3%〜31.3% / 高さ 8.3%〜41.7% |
    | `centre 0.75` | (240,120)-(400,240) | 中央。幅 37.5%〜62.5% / 高さ 33.3%〜66.7% |
    | `bottom-right 0.55` | (430,260)-(620,420) | 右下。**下端は画面外なので下辺でクランプされる**（クランプ動作の確認も兼ねる） |

    緑の枠線、ラベルは各箱の**左上の少し上**に緑地・黒文字。

### 懸念

| # | 内容 | 状態 |
|---|---|---|
| 12 | Skia のソースビルドが最大のリスク。例の Dockerfile 自身が `git-sync-deps` のネットワーク失敗と HTTP 429 レート制限を警告し、ワークアラウンド（スレッドを直列化する sed）を書いている | ✅ クローズ（2026-07-26。sed なしで成功、Skia レイヤ 353.5 秒 / `libskia.a` 22,996,766 byte） |
| 13 | `skia_use_system_freetype2=false` は GN 設定のうち実際に走らせないと成否が読めない箇所 | ✅ クローズ（2026-07-26。バンドル版 freetype でビルド成功、`libfreetype2.a` / `fontmgr_custom_empty` が生成された） |
| 14 | 視聴ストリームの画角が推論用の 640×360（16:9）と違うと箱がずれる | サンプルアプリなので同一画角を前提とする。4:3 ストリームで見るとずれる（制約として受容） |
| 15 | ビルド時間が現状の数秒から Skia の初回コンパイル分（数十分規模）へ伸びる。EGL/dma-buf/Ganesh の配線だけで例は 784 行あり、design.md の「できるだけ単純に」からは外れる | 受容済み（公式例に沿う価値を優先。2026-07-26 ユーザー判断） |
| 16 | **ラベルの文字だけが描画されない**（2-C-4 で判明） | ⚠️ **回避済み・原因未解明（2026-07-26）**。グリフパス描画に変更して安定動作（目視で確認、出荷版はこれ）。当初「`drawString` は一切描画されない」と断定したが**その結論は誤り**（ビルド B では目視で描画されていた）。読み戻しによる測定も信用できない。原因究明は TODO T-1〜T-4 として別途 |

### 懸念 #16 — ラベルの文字が出ない（2026-07-26、**回避済み・原因未解明**）

ユーザー目視: **箱 ✅ / ラベル背景の緑の帯 ✅ / 文字 ✗**。

#### 静的解析で除外できたもの

| 疑い | 判定 |
|---|---|
| フォントが eap に入っていない | 除外。`DejaVuSans.ttf` は eap 直下 → インストール先が `FONT_PATH` と一致 |
| フォントファイルが壊れている | 除外。magic `00010000` / 20 テーブル / `cmap`・`glyf`・`head` すべて存在 |
| freetype がリンクされていない | 除外。`test_overlay` に `FREETYPE_PROPERTIES` 等の文字列あり |
| typeface が null | 除外。null なら `load_font()` が false → `overlay_start()` が NULL → 箱すら出ない |
| 描画順序で背景が文字を上書き | 除外。背景 → `drawString` の順（`overlay.cc:490-495`） |
| 文字色と背景色が同じ | 除外。背景 `SK_ColorGREEN` / 文字 `SK_ColorBLACK` |

#### 切り分け結果

ラベル背景の幅は `measureText()` の戻り値で決まる（`overlay.cc:487, 490-493`）。
ユーザーに「背景は横長の帯か、幅 4px の細い棒か」を確認したところ **横長の帯** だった。
つまり `measureText()` が正常値を返している = **typeface からグリフのメトリクスが引けている**。

→ **グリフは存在するが GPU 側でテキストだけ描画されていない。**
矩形やパスはジオメトリとして直接描かれるが、テキストは**グリフアトラス**（グリフのマスクを
載せるテクスチャ）経由で描かれる。このアトラス用テクスチャの確保に失敗すると、Skia は
エラーを出さずに文字だけ静かに描かれなくなり、他の描画は正常に出る。症状と一致する。

#### 次のサイクルでやること（診断と修正候補を 1 回にまとめる）

同じ文字列を 2 通りで描いて並べる:

1. 現行の `canvas->drawString(...)`（アトラス経由）
2. `SkFont::getPath()` でグリフをパスに変換して `canvas->drawPath(...)`（アトラス不使用）

**2 だけが見えればアトラス起因で確定し、同時にそれが修正案そのものになる。**
両方見えなければ別原因なので仕切り直す。あわせて `typeface->countGlyphs()` /
`unicharToGlyph('A')` / `measureText()` の実測値をログに出す。

#### 決着（2026-07-26）— **回避済み。ただし原因は未解明**

> ⚠️ **2026-07-26 追記・重要な訂正。** この節はもともと「`drawString` は 1 ピクセルも描かれない。
> 決定的・再現性 100%」と断定していたが、**その結論は支持されない**。
> 後日ユーザーに確認したところ、2 段描画のときに**緑の帯（`drawString`）にも黄の帯（パス）にも
> それぞれラベル文字が読めていた**ことが判明した。詳細は下の「訂正」節。
> **実装（グリフパス描画）は正しく動いており、成果物に問題はない。**

**現時点で言えること: グリフパス描画は常に動く。`drawString` はビルドによって描かれたり
描かれなかったりする。原因は未解明。アトラス経路を避けるのが安全。**

##### 実測（`test_overlay` の `atlasonly` 引数で同一バイナリ内 A/B、各 60 サンプル）

| 描画方法 | ラベル内の文字ピクセル数（読み戻し） | サンプル |
|---|---|---|
| `canvas->drawString()` | 0（全サンプル） | 60/60 |
| `SkFont::getPath()` → `drawPath()` | 318（初回）／以降 21 | 60/60 |

> ⚠️ **この読み戻し値は目視と矛盾しており、測定自体が信用できない。** 下の「訂正」節を参照。

##### 測定方法（ログだけで判定できる指標）

`overlay_verify_text()` を追加。**描画直後・`axo_submit_buffer()` の前**に
`SkSurface::readPixels()` でラベル帯を読み戻し、帯（明色）の中の暗いピクセル＝グリフ被覆を数える。
0 なら「帯は出ているが文字が入っていない」と機械的に判定できる。全ストリームを走査して
**最小値**を報告するので、複数ストリームのうち 1 つだけ壊れても埋もれない。

> ⚠️ **圧縮バッファでは読み戻しが信用できない。** `AXO_FORMAT_FLAGS_COMPRESSED` 付きの
> dma-buf を GL 経由で読むと、1 フレーム目だけ正しく、以降は実際の見た目と無関係な値
> （パス描画で 318 → 21）になった。この誤った数値を根拠に「1 フレーム目だけ描かれる間欠バグ」と
> 誤診しかけた。`overlay_set_verify_text(true)` のときだけ非圧縮フォーマットを要求するようにして
> 解決。**本番（検証オフ）は推奨どおり圧縮のまま。**

##### 訂正 — 「両方に文字が見えた」は読み違いではなかった（2026-07-26）

当初この節には「2 本の帯が `1.15 × フォントサイズ` しか離れておらず隣接しているため、
下の帯（パス）の文字を見て両方に文字があると読み取られたと考えられる。drawString は
最初から一度も描けていなかった」と書いていた。**これは誤り。**

後日ユーザーに確認したところ、**黄の帯に `top~~`、緑の帯にも `top~~` と、それぞれ独立に
ラベル文字が読めていた**。読み違いではない。したがって:

- **ビルド B（2 段描画）では `drawString` の文字が実際に表示されていた**
- 読み戻しが緑帯を 0 px と報告したのは**測定の誤り**。最も疑わしいのは、2 段化で座標が
  変わった際に**緑帯の測定矩形がずれ、文字のない領域を数えていた**こと
- 「読み戻しの二値（0 / 非ゼロ）は信用できる」という主張は成立しない。
  その主張の唯一の照合先が目視であり、その目視を誤りとして退けたうえで
  「二値は常に目視と一致した」と述べていたので、論証が循環していた

##### 未解明のまま残る本当の謎 — ビルド A とビルド B の違い

3 つの目視を並べると:

| ビルド | 構成 | 目視結果 |
|---|---|---|
| A | 帯 1 本・`drawString` | **文字が出ない** |
| B | 2 段・緑 = `drawString` / 黄 = パス | **両方に文字が出る** |
| 最終 | 帯 1 本・パス | 文字が出る（出荷版） |

つまり `drawString` は**ビルド A では落ち、ビルド B では通っている**。これと矛盾しない像は
「**アトラス経由のテキスト描画が間欠的に機能しない**」で、ビルド B ではテキスト描画オペが
増えたことでアップロードが間に合った、という筋が考えられる。ただし**未検証**。

決定論的な不具合より間欠のほうが厄介（「たまに文字が消えるアプリ」になる）なので、
**アトラス経路を完全に避けるパス描画への切り替えは、結果的により正しい判断だった**。

##### コーディネータ仮説の判定

| 仮説 | 判定 |
|---|---|
| 1. flush / submit の不足でアトラスのアップロードが間に合っていない | **保留**（当初は「否定」と記載）。`axo_submit_buffer()` の直前が両ビルドとも `flushAndSubmit(GrSyncCpu::kYes)` で**未編集**なのはコード差分の事実として有効。ただしこれは「テキスト描画オペが増えたことでアトラスへのアップロードのタイミングや挙動が変わった」可能性を否定しない。ビルド A と B で結果が違う以上、この線は消えていない |
| 2. テキストのサイズ・baseline・paint が変わった | **否定**。緑帯側のコードはビルド 1 とビルド 2 で論理的に同一（`char text[64]` → `char text[MAX_LABEL]` は 64 のまま） |
| 3. 緑帯の描画位置が変わった | **否定**。同上、未変更 |

ビルド 1 → 2 の `overlay.cc` の差分は「include 3 本追加 / `MAX_LABEL` / 診断フラグ /
`draw_text_as_paths()` の追加 / 黄帯の追加 / 診断ログ」だけで、**drawString の呼び出しに影響する変更はない**。
挙動が変わったように見えたのは上記の目視の読み違いによるもの。

##### 修正

`draw_detections()` はラベル 1 本に戻し、文字は既定でグリフパスで描く
（`overlay_use_glyph_paths(false)` で drawString に戻せる = 不具合の再現用）。

- 性能: 1 ラベル 13 グリフ、検出 32 件でも 1 フレーム約 416 パス。1〜10 fps では無視できる。
  さらに削るなら全グリフを 1 本の `SkPath` にまとめて `drawPath` 1 回にできるが、現時点では不要
- `skia_enable_skshaper=false` のままで ASCII は問題なし

##### ⚠️ 読み戻しメトリクスの限界（2026-07-26 追試で判明）

決着後にもう一度取り直したところ、**同一バイナリ・非圧縮バッファ・同一ストリームで
「初回 318 px、以降 59 サンプルすべて 21 px」** となった（先の A/B では 318 × 60 だった）。
一方ユーザーの目視では文字は 60 秒間安定して見えている。つまり
**`SkSurface::readPixels()` の絶対値は表示内容と一致しない**（圧縮バッファだけの問題ではなかった）。

信頼できるのは**二値（0 か非ゼロか）だけ**。この二値判定は全実行で目視と一致した:

| | 全サンプルの値 | 目視 |
|---|---|---|
| `drawString` | 0 | 文字が出ない |
| グリフパス | 非ゼロ（21〜318） | 文字が出る |

→ 懸念 #16 の結論（**アトラス経由のテキストは一切描画されない / パス描画なら描かれる**）は
二値の一致と目視の両方で裏づけられており、変わらない。変わるのは「ピクセル数を品質指標に使えるか」で、
**使えない**。

##### 診断コードの取捨（2026-07-26 決定）

| 対象 | 判断 | 理由 |
|---|---|---|
| `overlay_use_glyph_paths()` / `test_overlay` の `atlasonly` | **削除** | 不具合の再現専用スイッチ。原因は確定し、再現コードは `SKILL_FEEDBACK.md` の H に実コードごと収録済みなので、恒久的に持つ理由がない（CLAUDE.md「投機的なコードを持たない」） |
| `overlay_verify_text()` / `overlay_set_verify_text()` / `count_text_pixels()` | **削除** | ①絶対値が信用できず、二値でしか使えない ②その二値を成立させるために**検証時だけ圧縮を切る**必要があり、テスト都合が本番のバッファ形式選択に染み出す ③design.md「できるだけ単純に」 ④必要になれば SKILL_FEEDBACK の実コードから 10 分で戻せる |
| `test_overlay` の自前 H.264 ストリーム | **残す** | ユーザーの目視なしでオーバーレイ経路を動かせる。2-D の反復で効く |
| ストリーム数のログ / フォント診断ログ（1 回だけ） | **残す** | 「何も見えない」ときの一次切り分けに必要。コストはほぼゼロ |

削除後: `overlay.h` は 23 行（API 4 本）、`overlay.cc` は 743 行 → **660 行**。
本番のバッファは推奨どおり圧縮のまま（`AXO_FORMAT_FLAGS_COMPRESSED | AXO_FORMAT_FLAGS_GPU`）に戻した。

##### 副産物: ユーザーの目視なしで検証できるようになった

`test_overlay` が**自前の H.264 ストリーム**（640×360）を開くようにした。
ライブビューを誰も開いていなくてもオーバーレイ対象ストリームが存在するので、
以後の回帰確認はユーザーを煩わせずに実行できる（`overlay: created overlay 2 on stream 1057,
stream 640x360` を確認済み）。

#### TODO — 別途検証する（このプロジェクトのスコープ外）

**2026-07-26 時点で本プロジェクトとしては打ち切り。** 出荷版はグリフパス描画で動作確認済みで、
機能上の問題はない。以下は「なぜそうなるのか」を知るための課題であり、
**このプロジェクトの完了条件には含めない**。別の場で検証する場合の出発点として残す。

| # | 検証したいこと | 出発点 |
|---|---|---|
| T-1 | ビルド A と B で `drawString` の結果が変わった理由 | 両ビルドのソースを保存して差分を取り、テキスト描画オペの数だけを変えた最小再現を作る。「オペを 1 つ増やすと通る」なら flush / アトラスのタイミング問題が濃厚 |
| T-2 | `drawString` の失敗は間欠か決定論的か | ビルド A 相当を多数回・長時間動かし、出る回と出ない回の比率を見る。1 回の実行内で消えたり出たりするかも見る |
| T-3 | `readPixels()` の値が表示と一致しない理由 | まず測定矩形が正しいかを、既知の図形（塗り潰した矩形など）を描いて検算する。矩形が正しいのに値が動くなら読み戻し経路自体の問題 |
| T-4 | 他機種（ARTPEC-8 等）でも同じことが起きるか | 実機が無いため未着手。ARTPEC-9 / Q1728 でしか観測していない |

**注意**: T-3 が先に片付かないと T-1 / T-2 の測定手段が無い。読み戻しが信用できない状態では
目視しか判定手段が無く、目視は人手がかかるうえ今回のように解釈で揉める。**T-3 を最初にやること。**

## Phase 2-D — 統合 & パラメータ

**方針はユーザー承認済み（2026-07-26）。**

### 決定事項

| 項目 | 決定 | 根拠 |
|---|---|---|
| `LoopCount` の 0 | **0 = 無限** | ユーザー決定（2026-07-26）。design.md の "range: 1 ~ infinite" は**下限が 1** なので **0 は design.md の範囲外に追加した値**。無限を表す値が design.md に定義されていないため 0 を割り当てた |
| パラメータの反映 | **起動時に 1 回読むだけ**（変更コールバックなし） | design.md「反映は再起動時、自動再起動なし」。コールバックを付けると `GMainLoop` が必要になり、逐次ループ構成が崩れる |
| `runMode` | **`once` のまま** | `LoopCount` 回で終了する仕様。`respawn` にすると終了のたびに再起動が繰り返され「指定回数で終わる」に反する |
| フレームレート制御 | VDO ストリームの `framerate` に渡すだけ | `capture_next()` が次フレームまでブロックするので、ループが自然にその周期になる。自前の `sleep` は不要 |
| パラメータ名 | **`FrameRate` / `LoopCount`** | 素直な名前。0 の意味は名前ではなく**起動時ログ**で伝える（下記） |

#### paramConfig に説明文は書けない（2026-07-26 SDK のスキーマで確認）

`schemaVersion` 2.1.0 の `application-manifest-schema-v2.1.0.json` では
`acapPackageConf.configuration.paramConfig[]` が **`additionalProperties: false`** で、
許されるキーは **`name` / `default` / `type` の 3 つだけ**（`description` フィールドは存在しない）。

この制約は次に同じことをやる人が調べ直さずに済むよう記録として残す。

**ただし「だから名前で説明する」という結論は採らない**（2026-07-26 ユーザー判断）。
一度 `LoopCount_0_is_infinite` という名前を実装したが不採用とし、**`LoopCount` に戻した**。
理由: パラメータ名は VAPIX のパスやコードにも現れる恒久的な識別子で、そこに説明文を埋め込むと
名前が説明の都合で歪む。「0 = 無限」の周知は**起動時ログ**（`main: LoopCount = 0 (infinite)`）と
PLAN / README 側で行う。

#### 上書き install では消えたパラメータが残る（2026-07-26 実機で確認）

`LoopCount_0_is_infinite` → `LoopCount` の改名を上書き install で反映したところ、
`ax_parameter_list()` の結果は **`LoopCount_0_is_infinite` / `LoopCount` / `FrameRate` の 3 つ**だった。
つまり **`paramConfig` から消したパラメータはデバイス上に登録されたまま残り、設定ページに両方出る**。

対処: `ax_parameter_remove()` を仕込んだ一時ビルドを 1 回だけ走らせて古い方を削除し
（ログ `main: removed stale parameter LoopCount_0_is_infinite`）、`ax_parameter_list()` が
`LoopCount` / `FrameRate` の 2 つになったことを確認してから、その一時コードを外して再デプロイした。
**`control.sh remove` は使っていない**（SSH ユーザーが消えるため）。
パラメータ名を変えるときはこの後始末が要る、というのが教訓。

### ステップ

- [x] 2-D-1 `app/main.cc`（バイナリ名は `yolov5_detector`）で 2-A → 2-B → 2-C をループ（2026-07-26 ✅）
  - `capture_start()` → `detector_start()`（`capture_output_fd()` を渡す）→ `overlay_start()` を起動時に 1 回
  - 毎ループ: `capture_next()` → `detector_run()` → `overlay_draw()`、検出をログに 1 行ずつ
  - `app/Makefile`: `OBJS` に `capture.cc detector.cc overlay.cc` を追加し、`PROG` も
    `OVERLAY_CXXFLAGS` / `OVERLAY_LDLIBS`（Skia 静的リンク）でリンクするようにした
  - verify: ✅ `-Werror` 通過。実機（既定値 1 fps × 10 回）で **loop 0〜9 の 10 回**を実行して
    `main: done`。ログの間隔は **1.000 秒**（21:22:54.089 → 21:22:55.088 → …）
  - 実測メモ: **モデルロードは 2 回目以降 1〜2 秒**（21:22:52.106 → 21:22:53.165）。
    約 50 秒かかるのは larod のキャッシュが無い初回のみ（再起動直後など）
  - 実測メモ: loop 0/1 だけ間隔が詰まる（VDO のバッファが既に溜まっているため）。
    2 周目以降が指定の周期になる
- [x] 2-D-2 AXParameter で `FrameRate`（1〜10、既定 1）/ `LoopCount`（0〜、既定 10）を起動時に読む（2026-07-26 ✅）
  - 範囲外・パース不能・読み取り失敗は**既定値に落として警告ログ**（黙って別の値で動かさない）
  - 読み取れた値そのものもログに出すので、既定値へのフォールバックと区別できる
  - verify: ✅ 実機ログに `main: FrameRate = 1` / `main: LoopCount = 10`
    （この行は `ax_parameter_get` 成功時のみ出る = AXParameter 経由で読めている証拠）。
    その値どおり 1 fps × 10 ループで完走
  - `app/Makefile` の `PKGS` に `axparameter` を追加
- [x] 2-D-3 manifest に `configuration.paramConfig` を追加（**2-D-2 と同じ増分で**）（2026-07-26 ✅）
  - `resources.linux.user.groups`（`video` / `gpu`）・`deepLearningProcessor`・`overlay` は追加済み
  - verify: ✅ eap の `param.conf` が生成され、内容は
    `FrameRate="1" type="int:1,10"` / `LoopCount="10" type="int:0,1000000"`
- [ ] 2-D-4 verify: **【ユーザー作業】** 実映像での確認（**懸念 #11 の解消をここで行う**）
      — ⏸ **2026-07-26 時点で唯一の残作業。デプロイ済みで、実行を待つだけの状態。**

#### 2-D-4 の実施手順（次回そのまま使える形）

**状態: `build/yolov5_detector_1_0_0_aarch64.eap`（12,843,186 byte）をデバイスへデプロイ済み
（2026-07-26 22:2x、`deploy.sh` が `OK`）。** eap 内の `param.conf` は
`FrameRate="1" type="int:1,10"` / `LoopCount="10" type="int:0,1000000"`。
バイナリに `main: %s = 0 (infinite)` / `FrameRate` / `LoopCount` の文字列があることを確認済み。

**① ユーザーに準備してもらうこと**

| # | 内容 |
|---|---|
| 1 | アプリの設定ページで **`FrameRate` = 2** / **`LoopCount` = 120** に変更して保存（反映は再起動時。その再起動が実行の合図になる） |
| 2 | **ライブビューを 16:9 の解像度で開く**（4:3 だと箱が横にずれる = 懸念 #14、受容済み） |
| 3 | **`person` として写るものを画角に入れる** |

`person` を写す方法（確実な順）:

- **A（推奨）**: 人がカメラの前に立つ。**2〜3 m**、**上半身以上**が入る位置、明るい場所
- **B**: スマホ／タブレットに**全身が写った人物写真**を表示し、画面いっぱいの状態で **50cm〜1m** に掲げる
- **C**: 人物が大きく写ったポスター・雑誌の見開き

**② 実行**

```bash
bash $SC/control.sh yolov5_detector restart
```

所要は **起動 2 秒前後 ＋ 60 秒**（120 ループ ÷ 2 fps）。
デバイスを再起動した直後だけモデルロードに約 50 秒かかるので、その場合は合計 110 秒ほど。
オーバーレイは 2 fps 更新なので**箱はカクカク追従する**（不具合ではない）。

**③ ユーザーに答えてもらう質問（すべて yes/no で一意に決まる形にすること）**

1. 人物を囲む**緑の枠**が出ましたか？（はい / いいえ）
2. その枠の**左上に `person 0.xx` というラベルの文字**が読めましたか？（はい / いいえ）
3. 人が動いたとき、**枠は人と一緒に移動**しましたか？（はい / いいえ / 人は動かさなかった）

> 2-C-4 で「両方に文字が見えますか」という**答えが一意に定まらない聞き方**をして 1 サイクル
> 無駄にした。質問は必ず単一の対象について yes/no で答えられる形にすること。

**④ ログで機械判定する項目**（`deploy.sh` はログを消すので **実行 → ログ参照** の順を守る）

| 内容 | 見る行 | 合格条件 |
|---|---|---|
| 周期 | `main: loop N` のタイムスタンプ | loop 2 以降が **0.5 秒間隔**（loop 0/1 は VDO のバッファが溜まっているため詰まる） |
| ループ回数 | 末尾付近 | `main: loop 119` の次に `main: done` |
| 検出クラス | `main:   person 0.xx (x1,y1)-(x2,y2)` | `person` の行が 1 行以上 → **懸念 #11 クローズ** |
| パラメータ反映 | 起動直後 | `main: FrameRate = 2` / `main: LoopCount = 120` |

**⑤ 合格したら**

- 2-D-4 を ✅、進捗サマリの 2-D を ✅ 完了、**懸念 #11 をクローズ**
- design.md の検証項目 3 つがすべて達成 → **プロジェクト完了**

---

## 未解決の懸念 / 保留事項

| # | 内容 | 必要になる時点 | 状態 |
|---|---|---|---|
| 1 | 出力の量子化パラメータ（scale / zero-point）。無いとスコア・座標が「エラーにならず静かに間違う」 | Phase 2-B | 解決済み（2026-07-26 `app/models/info.yaml` が実測値と一致。uint8 / zp=0 で扱う） |
| 2 | テスト画像 + ホスト参照出力（ユーザー提供） | Phase 2-B | 解決済み（2026-07-26 `app/sample/image.jpg` + `output.yaml`） |
| 3 | モデルの学習クラス順が標準 COCO80 である前提。参照出力との突き合わせで検証 | Phase 2-B | 解決済み（2026-07-26 実機が person=0 / bus=5 をホスト参照と同じ位置に検出） |
| 9 | `a9-dlpu-tflite` の推論結果がホスト CPU tflite と一致しない（平均 1 LSB、最大 221 LSB）。検出座標で最大 11.9px、スコアで 0.025、低スコア 1 件の欠落として現れ、2-B-5 の当初許容差（3px / 0.02）を超える | Phase 2-B | 解決済み（2026-07-26 `cpu-tflite` はホストと 94.95% 一致、DLPU はホストからも実機 CPU からも同じだけ離れる → **DLPU 固有の演算差で確定**。実装は正しい。2-B-5 の許容差を実測に合わせて改訂） |
| 10 | 量子化スコアの同点で NMS の代表ボックスが入れ替わる（39 候補中 3 組が同点）。ホスト参照の NumPy も非安定ソートで順序未定義 | Phase 2-B | 決定済み（修正しない。検証条件で同点差を不問にする。2-B-5 の tie-break 節を参照） |
| 11 | ライブ 1 フレームの推論が 0 検出。画角に対象物が無かっただけの可能性が高いが、実映像での検出は未確認 | Phase 2-D | 保留（2-D の統合時に実映像で確認する） |
| 4 | `main.cc` はスキル慣習（ソース名 = appName）と異なるが CLAUDE.md 優先。バイナリ名のみ `yolov5_detector` に合わせる | Phase 1 | 決定済み |
| 5 | larod device 名は `a9-dlpu-tflite`。スキル `references/larod.md` の表にある `axis-a9-dlpu-tflite` は**この実機では存在しない** | Phase 2-B | 解決済み（実機で確定） |
| 6 | 前処理は `cpu-proc`（CPU/libyuv）で実装。`a9-gpu-proc` の方が速い可能性があるが、1〜10 fps では未計測・未検証 | 最適化時 | 保留 |
| 7 | DLPU が memfd（dmabuf でない fd）を推論入力として受けるか未検証 | Phase 2-B | 解決済み（2026-07-26 2-B-2 で memfd 入力の `larodRunJob` がエラーなく完走。変換は不要） |
| 8 | eap がモデル 7.3MB + raw サンプル 1.2MB で約 9MB に膨らむ（現 ~1MB）。install が遅くなる | Phase 2-B | 許容。JPEG デコーダを持ち込むより単純 |

---

## 作業ログ

| 日付 | 内容 |
|---|---|
| 2026-07-26 | **本日終了。** パラメータ名を `LoopCount` に戻し（`LoopCount_0_is_infinite` は不採用）、リビルド（全レイヤ CACHED = 21:43 の eap が既に最終内容）とデプロイ（`OK`）まで完了。バイナリに `main: %s = 0 (infinite)` / `FrameRate` / `LoopCount` を確認。**残るは 2-D-4 の実映像確認のみ**で、手順を Phase 2-D に書き出した。あわせて懸念 #16 の記述を訂正（下記）。 |
| 2026-07-26 | **懸念 #16 の結論を訂正。** ユーザーに確認したところ、2 段描画のとき緑帯（`drawString`）にも黄帯（パス）にもそれぞれ `top~~` の文字が読めていたと判明。「隣接 2 帯の読み違い」「`drawString` は一度も描けていない」は**どちらも誤り**だった。`readPixels()` による測定が信用できず、それと矛盾する目視を退けたことで誤った結論に至っていた（論証も循環していた）。懸念 #16 を「回避済み・原因未解明」に格下げし、原因究明を **TODO T-1〜T-4** としてスコープ外に切り出した。`SKILL_FEEDBACK.md` の H-1 / H-4 / H-5 と G も同様に訂正。**出荷版（グリフパス描画）の動作には影響なし。** |
| 2026-07-24 | プラン策定。前提（デバイス / SDK / appName / 検証方法）を確定。着手前。 |
| 2026-07-24 | Phase 1 着手。1-1 ✅ ビルド成功。1-2 ✅ install / start / ログ確認。1-3（SSH パスワード設定）でユーザー作業待ち。 |
| 2026-07-24 | メモ: skill の `control.sh` / `run.sh` / `view_log.sh` は実行権限なし → `bash <script>` で起動する（`deploy.sh` のみ実行可）。 |
| 2026-07-24 | 1-4 ✅ `run.sh` で `Hello World` 取得。**Phase 1 ゲート通過**。Phase 2-A 着手。 |
| 2026-07-24 | Phase 2 以降の実装エージェント `acap-phase2-impl` を作成。2-A のコードはまだ未着手。 |
| 2026-07-24 | 2-A-1 ✅ `app/capture.{cc,h}` + `app/test_capture.cc` で VDO NV12 640x360 取得。manifest に `video` グループ追加、`Dockerfile` を `acap-build ./ -a test_capture` に変更。実機ダンプ 345,600 byte、ホストで PNG 化して正常確認。 |
| 2026-07-24 | 2-A-2 ✅ larod 前処理（`cpu-proc`）で NV12 → RGB-interleaved。memfd の 640×640×3 バッファ先頭に 640×360 を書かせて下 280 行をゼロパディング。manifest に `deepLearningProcessor` 追加、`app/Makefile` の `PKGS` に `liblarod` 追加。 |
| 2026-07-24 | **重要**: 実機 `larodListDevices()` の結果 → `cpu-tflite` / `a9-dlpu-native` / `armnn-cpu-tflite` / `cpu-proc` / `a9-dlpu-tflite` / `a9-gpu-proc`。推論 device は `axis-a9-dlpu-tflite` ではなく **`a9-dlpu-tflite`**。 |
| 2026-07-24 | `tools/host_infer.py` 作成（uv + ai-edge-litert）。skill 同梱モデルで量子化パラメータと参照出力を取得できることを確認。`--raw-input` でデバイスの前処理出力をそのまま食わせられる。 |
| 2026-07-24 | Phase 1〜2-A の lesson learned を `SKILL_FEEDBACK.md` にまとめた（スキル本体は未変更）。エージェント定義の larod device 名を実測値に修正。 |
| 2026-07-26 | 2-B-0 ✅ ユーザーがテスト画像・ホスト参照出力・量子化パラメータを提供（`app/sample/`, `app/models/info.yaml`）。懸念 #1 / #2 クローズ。デコードは uint8 / zero_point 0 で実装する方針を確定。コードはまだ未着手。 |
| 2026-07-26 | 2-B-1 のやり直し。`image.jpg` が JPEG 再エンコードで、`output.yaml` は JPEG 前の配列由来だと判明（パディング最大値 9 / 画素差 平均 4.0・最大 67 / `tools/bus.jpg` から直接 letterbox すると `output.yaml` を桁まで再現）。`image.rgb` を `tools/bus.jpg` から作り直し、`host_ref.json` が `output.yaml` と完全一致することを確認。**推論入力に `image.jpg` を使わない**ことを明記。 |
| 2026-07-26 | 2-B の実装方針をユーザーに提示し承認を得た。参照出力が同一量子化モデル由来であることを座標の刻み（1.3262 px の整数倍）で検算。ステップ 2-B-1〜2-B-5 と verify 条件を本ファイルに反映して実装着手。 |
| 2026-07-26 | 2-B-2 ✅ `app/detector.{cc,h}` + `app/test_detect.cc`（推論のみ）を追加。`app/Makefile` に `test_detect`、`Dockerfile` の `acap-build` に `-a test_detect -a models/yolov5s.tflite -a models/labels.txt -a sample/image.rgb` を追加（eap 5,573,242 byte、`tar tzf` で `models/` と `sample/` のサブディレクトリ構造がそのまま保たれることを確認）。実機で geometry 実測 = `[1,640,640,3]` / `[1,25200,85]` uint8 / 出力 fd size 2,142,000。**懸念 #7 クローズ**（memfd 入力で `larodRunJob` 成功）。`larodAllocModelInputs` は tracked 状態で返り fd を差し替えられないため `larodCreateModelInputs` に変更（PLAN の設計要点 2 を訂正）。モデルロードに約 50 秒。 |
| 2026-07-26 | 2-B-3 ✅ デコード（逆量子化 → 閾値 0.25 → xywh→xyxy → クラス別 NMS 0.45 → ラベル）を `detector.cc` に実装、`-Werror` で通過。2-B-4 ✅ `test_detect` がサンプル推論とライブ 1 フレームを実行（ライブは検出 0 件だが経路は完走）。 |
| 2026-07-26 | ✅ 2-B-5 決着。`detector_start()` に device 引数を足し（本番既定は `a9-dlpu-tflite`、`test_detect` は `-a "cpu"` で `cpu-tflite`）、生テンソルを 3 者比較: **cpu-tflite vs ホスト 94.95% 一致（平均 0.079 LSB）/ DLPU vs ホスト 48.31%（平均 1.00）/ DLPU vs 実機 CPU 48.37%（平均 1.00）** → 差は **DLPU 固有**で確定、入力の渡し方もデコードも正しいことが独立に裏づけられた。`cpu-tflite` なら C++ デコーダが 5/5 検出（座標差 ≤ 3.98px、スコア差 ≤ 0.016）。bus の食い違いは**完全同点スコア（row 24562 と 24563 がともに 0.789583）の tie-break** と確定、ホスト側も非安定ソートで順序未定義のため修正しない方針に決定。許容差を実測に合わせて改訂し 2-B-5 を通過。懸念 #9 クローズ、#10 / #11 を起票。SKILL_FEEDBACK に A-2（`larodAllocModelInputs` の誤った手順）/ D-4 / D-5 を追記。**Phase 2-B 完了 → 2-C へ**。 |
| 2026-07-26 | 2-C-1 ✅ Dockerfile に Skia（`chrome/m137`）のソースビルドを追加。`git-sync-deps` はワークアラウンドなしで成功し、Skia レイヤ 353.5 秒 / `libskia.a` 22,996,766 byte / ビルド全体 6 分 21 秒。2 回目は Skia レイヤが全て CACHED（34 秒）。`skia_use_freetype=true` + `skia_use_system_freetype2=false` が通り、`fonts-dejavu-core`（759,720 byte）も apt レイヤに同梱。**懸念 #12 / #13 クローズ**。ビルド済みヘッダで m137 のフォント API を実物確認（`SkTypeface::MakeFromFile` は無し、`SkFontMgr_New_Custom_Empty()` + **`SkFontMgr::makeFromFile()`** が正解）。アプリのコードは未変更。 |
| 2026-07-26 | 2-D-1 ✅ `app/main.cc` に統合ループを実装（capture → detect → overlay、検出を毎ループ syslog）。実機で 1 fps × 10 ループを完走、間隔 1.000 秒。モデルロードは**キャッシュ済みなら 1〜2 秒**（50 秒かかるのは初回のみ）と判明。2-D-2 / 2-D-3 ✅ AXParameter で `FrameRate` / `LoopCount_0_is_infinite` を起動時に 1 回読む実装と manifest の `paramConfig` を同じ増分で追加。`param.conf` の生成と読み取り成功ログを確認。**`paramConfig` に説明文フィールドは無い**（スキーマが `additionalProperties: false` で `name`/`default`/`type` のみ）と SDK のスキーマで確認したため、「0 = 無限」はパラメータ名 `LoopCount_0_is_infinite` で表現した。 |
| 2026-07-26 | ✅ **2-C 完了**。ユーザー最終目視で緑枠 3 個とラベルの文字を確認（design.md 検証項目 3 合格）。あわせて診断コードを整理: 再現スイッチ（`overlay_use_glyph_paths` / `atlasonly`）と読み戻し検証（`overlay_verify_text` ほか）を**削除**し、本番バッファを圧縮に戻した。削除の決め手は、追試で読み戻しの絶対値が表示と一致しないと分かった（同一条件で初回 318 → 以降 21、目視は安定）ことと、検証のために本番のバッファ形式の選択を変える必要があったこと。`overlay.cc` 743 → 660 行。`SKILL_FEEDBACK.md` の H-1 の数値もこの追試に合わせて訂正（「318 でブレなし」→「二値のみ信頼可」）。 |
| 2026-07-26 | ⚠️ **上の行の結論は後日ユーザー確認により覆った。** 2 段描画のとき緑帯（`drawString`）にも黄帯（パス）にもそれぞれ `top~~` の文字が読めていたと判明。「隣接 2 帯の読み違い」は誤りで、**ビルド B では `drawString` が実際に描画されていた**。読み戻しが緑帯を 0 px としたのは測定誤り（矩形ズレの疑い）。懸念 #16 は「回避済み・原因未解明」に格下げし、原因究明は TODO T-1〜T-4 として本プロジェクトのスコープ外に切り出した。**出荷版（グリフパス描画）の動作には影響なし。** |
| 2026-07-26 | ✅ **懸念 #16 解決**（※この結論は次の行で訂正）。同一バイナリ内 A/B（`atlasonly` 引数）と、描画直後・submit 前の `readPixels()` によるピクセル計測で、**`drawString` は 60/60 サンプルで 0 px、グリフパスは 60/60 で 318 px** と確定。コーディネータの仮説 1（flush/submit 不足）は否定（`flushAndSubmit(GrSyncCpu::kYes)` は両ビルドで未編集）。「一度は両方見えた」は隣接 2 帯の読み違いで、**drawString は最初から一度も描けていなかった**と結論。修正はラベル 1 本＋グリフパス描画。途中、圧縮 dma-buf の読み戻しが 1 フレーム目以外でたらめな値を返し「間欠バグ」と誤診しかけたので、検証時のみ非圧縮を要求するようにした（本番は圧縮のまま）。副産物として `test_overlay` が自前の H.264 ストリームを開くようになり、以後ユーザーの目視なしで回帰確認できる。 |
| 2026-07-26 | 懸念 #16 の診断サイクル。`overlay.cc` に `draw_text_as_paths()`（`textToGlyphs` → `getXPos` → `getPath` → `drawPath`）を追加し、各ラベルを上下 2 段（上 = 緑地・`drawString`／下 = 黄地・グリフパス）で描くようにした。API は m137 のヘッダで実物確認。初回描画時にフォント名・グリフ数・`glyph('A')`・アウトライン verb 数・`measureText()` を 1 回だけ syslog。`-Werror` ビルド通過（eap 9,397,862 byte）、デプロイ済み。**実行待ち**。副産物: `view_log.sh` が空になるのは**`deploy.sh` の再インストールでアプリのログがリセットされる**ためと判明（ident は `yolov5_detector` で正しく、14:56 の実行分は出ていた）。 |
| 2026-07-26 | 2-C-2 ✅ `app/overlay.{cc,h}`（EGL → `EGLImageKHR`（dma-buf）→ GL テクスチャ → Skia Ganesh のゼロコピー経路、公式例に準拠）+ `app/test_overlay.cc` + manifest（`gpu` グループ / `overlay` リソース）+ Makefile（`test_overlay` だけ `-std=c++20` / `-I$(SKIA_DIR)` / `libskia.a`）+ LICENSE の第三者表記。`SkColorSpace.h` の include 漏れで 1 回ビルドが落ちた以外は素直に通過。eap 9,396,714 byte。2-C-3 ✅ 実機で 60 秒完走・エラーログ 0 件（`overlay: started, drawing 640x360 coordinates`）。視聴者ゼロだったので `0 viewer stream(s)`、描画自体は未実施。**2-C-4（目視確認）でユーザー待ち**。 |
| 2026-07-26 | 🟥（後に解決）2-B-5 未達。実機 4 件 vs host_ref 5 件、座標差 最大 11.94px・スコア差 最大 0.025 で許容差（3px / 0.02）超過。切り分け: 入力バイト完全一致（総和 107,917,233）、**生の出力テンソルがホストと違う**（一致 48.31% / 平均 1.00 LSB / 最大 221 LSB）、**デバイスの生テンソルに NumPy 参照デコードをかけると C++ 出力と一致**（※この時点の報告では「4 件とも一致」と書いたが、正しくは person 3 件のみ一致。bus は同点 tie-break で不一致 → 翌エントリで訂正）→ デコードは正しく、差は `a9-dlpu-tflite` の推論演算。懸念 #3 クローズ、懸念 #9 を新規に起票してユーザー判断待ち。 |
| 2026-07-24 | 2-A-3 / 2-A-4 ✅ verify 全通過（1,228,800 byte / 下 280 行 = 537,600 byte が全ゼロ / PNG 目視で非ストレッチ・色正常）。5 フレーム連続実行できたので出力 fd の offset ズレも無し。`control.sh start` でアプリ本体も引き続き起動する（manifest 変更の副作用なし）。**Phase 2-A 完了 → 2-B へ**。 |
