# `building-acap` スキルへのフィードバック

YOLOv5s ACAP アプリ（`yolov5_detector`）の Phase 1 〜 Phase 2-C を実装して得られた知見。
**スキル本体のファイルは一切変更していない。** ここに提案としてまとめる。

対象スキル: `/Users/taki/Arbete/agent-skills/skills/building-acap/`
検証環境: Axis Q1728 / ARTPEC-9 / aarch64 / OS 12.11.x / ACAP SDK 12.11.0
記録日: 2026-07-24（Phase 1 〜 2-A）／ 2026-07-26 追記（Phase 2-B / 2-C）

---

## A. 誤りの修正（優先度: 高）

### A-1. larod デバイス名の表が実機と一致しない

`references/larod.md` の「Device names」表:

| Chip | Device name（スキルの記載） |
|---|---|
| ARTPEC-9 DLPU | `axis-a9-dlpu-tflite` |

**実機 Q1728 の `larodListDevices()` の結果:**

```
cpu-tflite / a9-dlpu-native / armnn-cpu-tflite / cpu-proc / a9-dlpu-tflite / a9-gpu-proc
```

正しくは **`a9-dlpu-tflite`**（`axis-` 接頭辞なし）。スキルの名前で `larodGetDevice()` を呼ぶと
失敗する。

- ARTPEC-8 の `axis-a8-dlpu-tflite` も同様に誤っている可能性が高いが、**手元に実機がなく未検証**。
  検証できるまでは断定しないほうがよい。
- 表に載っていないデバイスが実機には複数ある（`a9-dlpu-native`, `armnn-cpu-tflite`,
  `cpu-proc`, `a9-gpu-proc`）。特に**前処理用デバイス（`cpu-proc` / `a9-gpu-proc`）が
  表に一切載っていない**のが実害として大きい。前処理の節で名前を挙げずに
  「前処理用のモデルをロードする」とだけ書かれているため、名前を自力で探す必要があった。

**提案**: 表に「実機で必ず `larodListDevices()` して確認すること。以下は参考値」と明記し、
前処理用デバイスの行を追加する。今回の Q1728 の実測一覧をそのまま例として載せてもよい。

### A-2. `references/larod.md` の Workflow のサンプルコードが動かない手順になっている（2026-07-26 追記）

「Workflow」節の 3 → 6 は、**そのまま書くと必ず失敗する**組み合わせになっている:

```c
// 3. Allocate model input/output tensors
larodTensor** inputs = larodAllocModelInputs(conn, model, 0, &num_inputs, NULL, &error);
...
// 6. Per frame: point the input tensor at the VDO buffer's dma-buf, then run
larodSetTensorFd(inputs[0], dup(vdo_buffer_get_fd(buf)), &error);
larodTrackTensor(conn, inputs[0], &error);              // once per distinct buffer
```

`larod.h` の `larodAllocModelInputs()` の説明:

> In addition the service will automatically track each tensor, i.e. **as if larodTrackTensor()
> would have been called** on the newly created and allocated tensors

`larodTrackTensor()` の説明:

> Once this function is called on @p tensor, **its file descriptor can not be replaced**
> (c.f. larodSetTensorFd()). This also applies to the fd size, fd offset and fd props

つまり alloc 系が返したテンソルは既に track 済みなので、`larodSetTensorFd()` は通らない
（さらに `larodTrackTensor()` を二度呼ぶことにもなる。ヘッダに
「should only be called once per larodTensor」とある）。

**正しくは `larodCreateModelInputs(model, &numTensors, &error)`。** モデルの dims / dtype /
layout を持ち fd = -1 のテンソルが返るので、そこに自前バッファの fd を
`larodSetTensorFd` / `...FdOffset` / `...FdSize` / `...FdProps` で設定してから
`larodTrackTensor()` する。use-case で使い分けると:

| やりたいこと | 使う API |
|---|---|
| larod にバッファも確保させる（出力側で普通） | `larodAllocModelInputs/Outputs` → fd 差し替え不可 |
| 自前バッファ（VDO の dma-buf、memfd など）を渡す（入力側で普通） | **`larodCreateModelInputs/Outputs`** |

**実害**: 本プロジェクトの Phase 2-B で、この手順をそのまま計画に書き写したため
実装 1 サイクル分の手戻りが出た。ヘッダを読んで初めて気づける類の誤りで、
サンプルコードは「VDO のバッファを入力にする」という**最も典型的な用途**を示しているので
影響範囲が広い。

**提案**: Workflow 節の手順 3 を入出力で分け、入力は `larodCreateModelInputs`、
出力は `larodAllocModelOutputs(..., READWRITE|MAP)` にする。あわせて
「track したテンソルの fd / size / offset / props は以後変更できない」を注記する。

---

### A-3. `references/overlay.md` が cairo 前提で、公式例と GPU パスに触れていない（2026-07-26 追記）

3 点が抜けている。

**(1) 公式例 `axoverlay2-skia` の存在に触れていない。**
[acap-native-sdk-examples/axoverlay2-skia](https://github.com/AxisCommunications/acap-native-sdk-examples/tree/main/axoverlay2-skia)
が axoverlay2 の公式サンプルだが、スキルは cairo でのみ説明している。Skia は SDK に含まれず
（ヘッダ・pkg-config・lib のいずれも無い）、Dockerfile 内で `git clone` → `tools/git-sync-deps`
→ `gn gen` → `ninja` でソースからビルドする。**この事実がスキルのどこにも書かれていないため、
「Skia を使え」と言われた時点で入手方法から調べ直すことになる。**

実測（Q1728 / SDK 12.11.0 / `chrome/m137`）: Skia のビルド **353.5 秒**、`make build` 全体で
**6 分 21 秒**、`libskia.a` **21.9 MiB**。2 回目以降はレイヤキャッシュが効いてアプリ層のみ 34 秒。
Skia のレイヤを `COPY app` より前に置き `SKIA_VERSION` にしか依存させなければ、Dockerfile を
分割しなくてもキャッシュだけで「一度だけビルド」が担保できる。

**(2) `axo_buffer_get_dma_buf_fd()` によるゼロコピー GPU パスに一切言及していない。**
スキルは「`axo_buffer` に直接描かず、プライベートな cairo サーフェスへ描いて `memcpy`」とだけ
書いている。しかし公式例は dma-buf を `EGLImageKHR`（`EGL_LINUX_DMA_BUF_EXT`）として取り込み、
GL テクスチャ経由で Skia の Ganesh サーフェスにしており、**`memcpy` が不要**。
`axo_buffer_get_dma_buf_fd()` はヘッダに実在するのに表にも本文にも出てこない。

**(3) GPU パスに必要な manifest の記載が抜けている。**
スキルは `resources.overlay` しか書いていないが、GPU で描くには
`resources.linux.user.groups` に **`gpu`** が要る（公式例の manifest で確認）。

### A-4. `references/bbox.md` に「テキストを描けない」ことが書かれていない（2026-07-26 追記）

スキルは bbox を「解析結果の矩形描画には overlay よりずっと簡単」と勧めており、実際そのとおり
なのだが、**`bbox.h` にテキスト描画の API が無い**（`bbox_rectangle` / `bbox_quad` /
`bbox_move_to` / `bbox_line_to` / `bbox_draw_path` のみ）。

物体検出でラベルを出したいという典型的な用途では bbox を選べない。この一文が無いと、
**簡単そうだからと bbox を選び、ラベルを実装する段になって作り直すことになる**。
「矩形・四角形・線のみ。文字が要るなら axoverlay2」と明記すべき。

---

## B. 情報が不足していて自力で埋めた箇所（優先度: 高）

### B-1. larod 前処理ジョブの具体的な書き方

`references/larod.md` の「Pre-processing jobs」節は、テンソルの作り方は書かれているが
**肝心の「前処理パラメータをどう渡すか」が書かれていない**。実際に必要だったのは:

```c
larodMap* map = larodCreateMap(&error);
larodMapSetStr(map, "image.input.format", "nv12", &error);
larodMapSetIntArr2(map, "image.input.size", 640, 360, &error);
larodMapSetInt(map, "image.input.row-pitch", 640, &error);
larodMapSetStr(map, "image.output.format", "rgb-interleaved", &error);
larodMapSetIntArr2(map, "image.output.size", 640, 360, &error);
larodMapSetInt(map, "image.output.row-pitch", 1920, &error);

// 前処理モデルは fd = -1 でロードする（ファイルではなくパラメータで定義される）
larodModel* pp = larodLoadModel(conn, -1, dev, LAROD_ACCESS_PRIVATE, "pp", map, &error);
```

**このキー名は `larod.h` にも載っていない。** 公式の `object-detection-yolov5` サンプルを
読んで確認した。スキルに 10 行足すだけで、この探索が丸ごと不要になる。

特に効くポイント:
- `larodLoadModel()` の第 2 引数に **`-1`** を渡すという発想は、ドキュメントなしでは出てこない
- `row-pitch` の単位（バイト単位。RGB interleaved なら幅 × 3）
- `image.output.format` の文字列（`"rgb-interleaved"`）

### B-2. レターボックス／パディングのイディオム

「モデル入力が正方形、カメラ出力が 16:9」は極めてありふれた組み合わせなのに、
その扱いがスキルに無い。今回使った方法は汎用的なので載せる価値がある:

> 出力バッファ（640×640×3）を `memfd_create` で確保してゼロクリアし、**前処理の出力テンソルには
> 内容領域（640×360×3）だけを記述させる**。RGB interleaved は行が連続なので、書かれなかった
> 下部がそのままゼロパディングになる。追加のコピーもパディング処理も不要。

同じ memfd を推論入力（640×640×3）としてそのまま渡せるので、コピーゼロで繋がる点も含めて。

### B-3. C++ プロジェクトでの `app/Makefile`

`SKILL.md` の Makefile テンプレートは C 専用で、C++ にそのまま使えない。

- `$(CC)` / `CFLAGS` → `$(CXX)` / `CXXFLAGS`
- **`-Wbad-function-cast` / `-Wstrict-prototypes` / `-Wmissing-prototypes` は C 専用**。
  C++ で渡すとコンパイラが警告を出し、`-Werror` と組み合わさってビルドが落ちる
- `OBJS = $(PROG).c` → ソースを明示列挙する形に

CLAUDE.md で「C++ で書く」と指定されるケースは珍しくないので、C++ 版テンプレートを併記するか、
最低限「C++ の場合は上記 3 つの警告オプションを外す」と注記があると詰まらない。

### B-4. `PKGS` / pkg-config の実際の書き方

各 reference に `PKGS = gio-2.0 gio-unix-2.0 liblarod vdostream` とだけ書かれているが、
**それを Makefile でどう展開するか**が書かれていない。実際に必要だったのは:

```makefile
PKGS = gio-2.0 gio-unix-2.0 liblarod vdostream
CXXFLAGS += $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) pkg-config --cflags $(PKGS))
LDLIBS   += $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) pkg-config --libs $(PKGS))
```

`PKG_CONFIG_PATH` を明示的に引き継ぐ必要がある点がポイント。

### B-5. テストバイナリを eap に同梱する具体的な方法

`SKILL.md` の「Unit / function testing」は**方針は書いてあるが手順が書いていない**。
実際に必要だったのは 2 箇所の変更:

```dockerfile
RUN . /opt/axis/acapsdk/environment-setup* && acap-build ./ -a test_capture
```

```makefile
all: $(PROG) $(TEST_CAPTURE)      # app/Makefile の all で両方ビルドする
```

`acap-build` の `-a` オプションの存在（複数指定可、サブディレクトリのパスも可）を
明記するだけで済む。モデルやラベルの同梱も同じ仕組みなので、Larod の節からも参照したい。

### B-6. テストバイナリの syslog ident

テストバイナリで `openlog()` の ident をバイナリ名（`test_capture`）にすると、
**`view_log.sh <appName>` に一切出てこない**。ログは appName で絞り込まれるため。
テストバイナリでも ident は **appName に揃える**必要がある。

---

## C. 環境・スクリプト運用の引っかかり（優先度: 中）

### C-1. スクリプトに実行権限が付いていない

`scripts/` のうち **`deploy.sh` だけ +x が付いており**、`control.sh` / `run.sh` /
`view_log.sh` は付いていない。そのまま呼ぶと `permission denied` になる。

→ リポジトリ側で +x を付けるか、`SKILL.md` の「Available scripts」に
「`bash <script>` で起動する」と書く。

### C-2. スクリプトは cwd の `./.env` を参照する

全スクリプトが `./.env` を見るため、**プロジェクトルートから実行する必要がある**。
スクリプト自身のディレクトリではない。「Available scripts」に一行あるとよい。

### C-3. install / SSH まわりの罠（`SKILL.md` 本文への追記候補）

前回のプロジェクトで踏み、今回は事前に回避できたもの。スキルに書いておく価値がある:

- **`control.sh remove` はアプリ専用 SSH ユーザーとそのパスワードを削除する。** remove →
  再インストールのたびにユーザーがデバイス UI でパスワードを再設定する羽目になる。
  Phase 2 の反復では **remove せず上書き install** するのが正解。
- **upload の `Error: 27` は vendorId の不一致。** 同じ appName で既にインストール済みの
  アプリと `vendorId` が違う eap は弾かれる。上書き install（＝ SSH ユーザーの温存）のためには
  **vendorId を固定し続ける**必要がある。

### C-4. `deepLearningProcessor` の要否

前処理を `cpu-proc`（CPU/libyuv）で行う場合、`resources.deepLearningProcessor` が本当に
必要かは未検証。今回は推論（`a9-dlpu-tflite`）で使う前提で先に入れてある。
スキルの表は「Larod = deepLearningProcessor が必要」と読めるが、**CPU バックエンドのみの
構成では不要な可能性がある**。検証できれば注記したい。

---

## D. 新規 reference の提案: ホスト側での検証（優先度: 中）

`design.md` の「推論結果がホストで生成したものと近いこと」という検証は ACAP では定番だが、
**その「ホスト側」をどう用意するかがスキルに無い**。今回作った方法は再利用できる:

### D-1. 量子化パラメータと参照出力の取得

`.tflite` の入出力の scale / zero-point は **larod から取得できない**が、デコーダには必須。
放置すると「クラッシュしないが静かに間違った検出」になる。ホストで tflite を読めば取れる:

```bash
uv run tools/host_infer.py <model.tflite> <image.jpg> --json ref.json
```

`uv` の PEP 723 インラインメタデータ（`ai-edge-litert` + numpy + pillow、Python 3.10〜3.12）で
書けば、事前インストール不要の単一ファイルで済む。macOS arm64 でも動作を確認済み。

このプロジェクトでの実測例（skill 同梱の `yolov5s.tflite`）:

```
input : [1,640,640,3] uint8  scale=0.003921568859368563  zero_point=0
output: [1,25200,85]  uint8  scale=0.004144445527344942  zero_point=0
box coords: normalized 0..1  → 640 倍が必要
```

### D-2. デバイスとホストで完全に同一の入力を使う

デバイスの前処理出力（RGB raw ダンプ）をホスト側スクリプトにそのまま食わせられるように
しておくと、差分が出たときに**前処理由来かデコード由来かを切り分けられる**。
テストバイナリが raw を stdout に吐く設計と組み合わせると強力。

### D-3. raw ダンプの目視確認

`run.sh` が回収した raw バッファは ffmpeg で即 PNG 化できる。前処理の verify に有効:

```bash
ffmpeg -f rawvideo -pix_fmt rgb24 -s 640x640 -i output out.png
ffmpeg -f rawvideo -pix_fmt nv12  -s 640x360 -i output out.png
```

R/B の入れ替わり（`rgb-interleaved` と `bgr-interleaved` の取り違え）は数値チェックでは
見落としやすく、目視が一番速く見つかる。

### D-4. DLPU の推論結果はホストの CPU tflite と一致しない（2026-07-26 追記）

「ホスト参照と一致すること」を検証条件にすると **DLPU では必ず落ちる**。同一モデル・
**同一入力バイト**（1,228,800 byte、バイト総和まで一致を確認）で生の出力テンソル
`[1,25200,85]` uint8 を比較した実測:

| 比較 | 完全一致した要素 | 平均 \|Δ\| | 最大 \|Δ\| |
|---|---|---|---|
| 実機 `cpu-tflite` vs ホスト（ai-edge-litert） | 94.95% | 0.079 LSB | 42 |
| 実機 `a9-dlpu-tflite` vs ホスト | 48.31% | 1.00 LSB | 221 |
| 実機 `a9-dlpu-tflite` vs 実機 `cpu-tflite` | 48.37% | 1.00 LSB | 219 |

デコード後（YOLOv5s / conf 0.25）では、DLPU は検出座標が最大 11.9px ずれ、
スコアが最大 0.025 違い、**閾値ぎりぎりの検出 1 件が消える**（0.271 → 0.25 未満）。
一方 `cpu-tflite` なら 5 件すべて出て座標差は最大 4.0px。

実務上のポイント:

- 検証条件は「完全一致」ではなく**許容差つき**にする（座標 数 px / スコア 0.03 程度 /
  閾値付近の検出は対象外）
- **切り分け手順として `cpu-tflite` での実行が非常に有効**。同じコードのまま device 名だけ
  差し替えて走らせ、ホストとほぼ一致すれば「入力の渡し方もデコードも正しく、差は DLPU 固有」と
  確定できる。テストバイナリで device 名を選べるようにしておくとよい
- 比較は**デコード後の検出リストではなく生テンソル**で行う。検出リストだと NMS や閾値で
  情報が落ち、1 LSB の差なのか実装バグなのか判別できない

### D-5. 量子化スコアは同点が普通に起きる → NMS の tie-break は不定

int8/uint8 量子化の出力はスコアが離散値なので、**別のアンカーが完全に同じスコアになる**ことが
珍しくない（実測: 39 候補中 3 組が同点）。NMS でどちらが残るかはソートの安定性で決まり、
`numpy.argsort()` は既定 quicksort で**非安定**。したがって

- ホスト参照側にも「正しい順序」は無い
- デバイス実装をホストに一致させようとするのは筋が悪い（同点の並びは実装依存）

**提案**: ホスト比較の節に「同点スコアで代表ボックスが入れ替わることがある。座標が
数 px 違う同一物体の検出なので、検証では同点差を不問にする」と注記する。

**この手順は [docs/verify-raw-dumps.md](docs/verify-raw-dumps.md) に手順集としてまとめた**
（サイズ検証・PNG 化・部分切り出し・症状別の原因表・2 つのダンプの比較・テスト入力の生成）。
新規 reference にするならこれがほぼそのまま使える。

---

## E. うまく機能した設計（維持すべき）

批判点だけでなく、実際に効いたものも記録しておく。

### E-1. Phase 1（walking skeleton）の強制

**これが一番効いた。** Hello World を install → start → ログ → SSH 実行まで通してから機能実装に
入ったことで、Phase 2-A で問題が出たときに「環境か実装か」で悩む時間がゼロだった。
「ユーザーが機能を直接依頼しても、skeleton が無ければ Phase 1 を先にやれ」という指示は
実際にその通りに機能した。

### E-2. 「モデルのジオメトリを決め打ちするな」

`references/larod.md` の "Inspect the model — don't hardcode its geometry" の節は当たり。
今回はこの指示があったおかげで `larodListDevices()` をログに出し、**Phase 2-A の時点で
デバイス名の誤り（A-1）を発見できた**。決め打ちしていれば Phase 2-B で「なぜか動かない」に
なっていた。同じ趣旨を**デバイス名にも明示的に広げる**とさらに良い。

### E-3. `run.sh` は stdout を回収するという注記

テストバイナリの設計（結果は stdout、ログは stderr/syslog）が最初から決まるので、
作り直しが発生しなかった。

### E-4. Red Flags のリスト

「300 行書く前にテストしろ」「manifest の resource 追加は使うコードと同じ増分で」は
そのまま守って問題が起きなかった。特に後者は、アプリが起動しなくなったときの原因究明を
確実に減らしている。

---

## F. 優先度つき提案まとめ

| # | 内容 | 対象ファイル | 優先度 |
|---|---|---|---|
| A-1 | larod デバイス名の修正 + 前処理デバイスの追加 + 「実機で確認」の明記 | `references/larod.md` | 高 |
| A-2 | Workflow のサンプルが `larodAllocModelInputs` → `larodSetTensorFd` で動かない。入力は `larodCreateModelInputs` に | `references/larod.md` | 高 |
| B-1 | 前処理 `larodMap` のキー名と `larodLoadModel(conn, -1, ...)` の記載 | `references/larod.md` | 高 |
| B-5 | `acap-build -a` によるテストバイナリ/モデル同梱の具体手順 | `SKILL.md` | 高 |
| B-3 | C++ 版 Makefile テンプレート（C 専用警告オプションの注記） | `SKILL.md` | 高 |
| B-4 | `PKGS` を pkg-config で展開する書き方 | `SKILL.md` | 中 |
| B-2 | レターボックス／下パディングのイディオム | `references/larod.md` | 中 |
| C-3 | remove が SSH ユーザーを消す / `Error: 27` = vendorId 不一致 | `SKILL.md` | 中 |
| D | ホスト側検証（量子化パラメータ取得・raw の PNG 化・同一入力での比較） | 新規 `references/host-verification.md` | 中 |
| D-4 | DLPU の結果はホスト CPU tflite と一致しない（実測値つき）。`cpu-tflite` での切り分けを推奨 | `references/larod.md` / 新規 | 高 |
| D-5 | 量子化スコアの同点で NMS の代表が入れ替わる。ホスト側も順序は不定 | 新規 | 中 |
| A-3 | 公式例 `axoverlay2-skia` の存在 / `axo_buffer_get_dma_buf_fd()` のゼロコピー GPU パス / `gpu` グループの記載漏れ | `references/overlay.md` | 高 |
| A-4 | bbox にはテキスト描画 API が無い。ラベルが要るなら axoverlay2 | `references/bbox.md` | 高 |
| H | オーバーレイ描画の検証（`drawString` が描かれない実機の存在・グリフパス回避策・ピクセル読み戻し・圧縮 dma-buf の落とし穴） | 新規 `references/overlay-verification.md` | 高 |
| B-6 | テストバイナリの syslog ident は appName に揃える | `SKILL.md` | 低 |
| C-1 | スクリプトの実行権限 / `bash` 経由での起動 | `SKILL.md` | 低 |
| C-2 | スクリプトはプロジェクトルートから実行する | `SKILL.md` | 低 |
| C-4 | CPU バックエンドのみの場合の `deepLearningProcessor` の要否（要検証） | `SKILL.md` | 低 |

---

## G. 未検証・断定できないこと

正直に記録しておく。

- ARTPEC-8 / CV25 / EdgeTPU のデバイス名が同様に誤っているかは**未確認**（実機なし）。
  Q1728（ARTPEC-9）でしか確認していない。
- `a9-gpu-proc` を前処理に使った場合の挙動・性能は**未計測**。今回は `cpu-proc` で十分だった。
- `deepLearningProcessor` を宣言しない場合に `cpu-proc` だけの構成が動くかは**未検証**（C-4）。
- D-1 の量子化パラメータは skill 同梱の `yolov5s.tflite` の実測値。ユーザーが持つ別の int8 版では
  値が異なる可能性がある。
- **H-1 の `drawString` の件は、確かなのは「ビルドによって文字が出たり出なかったりした」ことだけ。**
  一切描画されないのか、間欠なのか、条件依存なのかは**分かっていない**。当初「0 px / 60 サンプル、
  一度も描画されない」と断定して記録したが、**その根拠にした測定が信用できないと後で判明した**
  （H-4）。ドライバ側の原因も未特定。他機種（ARTPEC-8 等）で同じことが起きるかも**未検証**。
  実務上は「アトラス経路を避ければ確実に動く」だけが確か。
- H-4 の読み戻しが表示と一致しない理由は**未特定**。当初は圧縮が原因と考えたが非圧縮でも起きた。
  測定矩形のズレなのか、読み戻し経路自体の問題なのかも切り分けていない。
- 上記を追うなら、**まず読み戻しが正しく測れることを既知の図形で検算する**こと。それが無いと
  他の検証の土台が無い。

---

## H. 新規 reference の提案: オーバーレイ描画の検証（優先度: 高）

**提案: `references/overlay-verification.md` として、以下をコード込みで収録する。**

Phase 2-C で「箱は描かれるのにラベルの文字だけが出ない」という不具合に丸 1 日相当を溶かした。
エラーは 1 件も出ず、ログも正常、`axo_submit_buffer()` まで到達している。**描画系は
「静かに一部だけ失敗する」ことがある**という前提の検証手段がスキルに無いのが痛かった。

### H-1. 症状 — ARTPEC-9 で `SkCanvas::drawString` の文字が出ないことがある

Q1728 / ARTPEC-9 で、axoverlay2 + Skia Ganesh（EGL / dma-buf ゼロコピー）構成のとき、
**矩形は正常に描かれるのにラベルの文字だけが表示されない**という症状が出た。
エラーは 1 件も出ず、ログも正常、`axo_submit_buffer()` まで到達している。

目視で確認できた事実は次の 3 点:

| ビルド | 構成 | 目視結果 |
|---|---|---|
| A | ラベル 1 本・`drawString` | **文字が出ない** |
| B | 2 段（緑 = `drawString` / 黄 = グリフパス） | **両方に文字が出る** |
| 最終 | ラベル 1 本・グリフパス | 文字が出る（出荷版） |

**グリフパス描画は全ビルドで確実に動く。`drawString` はビルドによって出たり出なかったりする。**
一致する像は「アトラス経由のテキスト描画が**間欠的に**機能しない」だが、**原因は未解明**。
決定論的な不具合より間欠のほうが厄介（「たまに文字が消える」）なので、
**アトラス経路を避けるのが安全**というのが実務上の結論。

> ⚠️ **当初この節には「`drawString` は 0 px / 60 サンプル、一度も描画されない。決定的・再現性
> 100%」と書いていた。これは誤りだった。** 根拠にした `readPixels()` によるピクセル計測が
> ビルド B の緑帯を 0 px と報告したのだが、後日ユーザーに確認すると**緑帯にも黄帯にも
> それぞれラベル文字が読めていた**。当時は「2 本の帯が隣接しているので下の帯の文字を
> 読み違えたのだろう」と目視のほうを退けたが、そうではなかった。
> **信用できない測定を根拠に、それと矛盾する目視を切り捨てた**のが誤りの構造。詳細は H-4。

フォント側は完全にシロだった（`DejaVu Sans` / 6253 グリフ / `unicharToGlyph('A')` = 36 /
`'A'` のアウトライン 15 verbs / `measureText()` 331.0 px @ 54.0 px フォント）。
なおフォントの実測値は 4K 視聴ストリーム（描画領域 1920×1080 / 54px フォント）で取ったもので、
ピクセル計測は自前の 640×360 ストリーム（18px フォント / `measureText()` 110.0 px）で取ったもの。
別条件の数字なので、両者を突き合わせて比を計算しないこと。
`flushAndSubmit(GrSyncCpu::kYes)` は全ビルドで呼ばれており未編集。ただしテキスト描画オペの数が
変わったことでアトラスへのアップロードのタイミングが変わった可能性は**否定できていない**。

フォント側は完全にシロだった（`DejaVu Sans` / 6253 グリフ / `unicharToGlyph('A')` = 36 /
`'A'` のアウトライン 15 verbs / `measureText()` 331.0 px @ 54.0 px フォント）。
なおフォントの実測値は 4K 視聴ストリーム（描画領域 1920×1080 / 54px フォント）で取ったもので、
上表のピクセル数は自前の 640×360 ストリーム（18px フォント / `measureText()` 110.0 px）で取ったもの。
別条件の数字なので、両者を突き合わせて比を計算しないこと。
`flushAndSubmit(GrSyncCpu::kYes)` も呼ばれており、flush 不足でもない。
**ドライバ側の理由は未特定。アプリ側で回避するしかない。**

### H-2. 回避策 — 文字をグリフパスとして描く

アトラスを経由しないので確実に描かれる。1〜10 fps なら負荷は無視できる
（ラベル 1 本 13 グリフ、検出 32 件でも約 416 パス/フレーム）。

```c
// Draw text glyph by glyph as filled paths. Unlike SkCanvas::drawString this
// does not go through Skia's glyph atlas, so it still draws if allocating the
// atlas texture fails.
static void draw_text_as_paths(SkCanvas* canvas, const SkFont& font, const char* text,
                               float x, float baseline, const SkPaint& paint) {
    size_t length = strlen(text);
    int count     = font.countText(text, length, SkTextEncoding::kUTF8);
    if (count <= 0 || count > MAX_LABEL) {
        return;
    }
    SkGlyphID glyphs[MAX_LABEL];
    SkScalar  xpos[MAX_LABEL];
    font.textToGlyphs(text, length, SkTextEncoding::kUTF8, glyphs, count);
    font.getXPos(glyphs, count, xpos, x);

    SkPath path;
    for (int i = 0; i < count; i++) {
        // Blank glyphs such as space have no outline.
        if (!font.getPath(glyphs[i], &path) || path.isEmpty()) {
            continue;
        }
        canvas->save();
        canvas->translate(xpos[i], baseline);
        canvas->drawPath(path, paint);
        canvas->restore();
    }
}
```

使用 API はすべて m137 のヘッダで実物確認済み: `SkFont::textToGlyphs`（`SkFont.h:299`）/
`SkFont::getXPos`（`SkFont.h:435`）/ `SkFont::getPath`（`SkFont.h:460`）/
`SkGlyphID` = `uint16_t`（`SkTypes.h:187`）。

フォント読み込みは fontconfig 無しの構成なら `SkFontMgr_New_Custom_Empty()` +
**`SkFontMgr::makeFromFile(path, 0)`**（`SkFontMgr.h:111`）。
`SkTypeface::MakeFromFile` は近年の Skia では**存在しない**。

### H-3. 検証手段 — ピクセル読み戻しで機械判定する

目視は「見えた気がする」で誤判定する（H-5 参照）。**描いた直後・submit の前に**サーフェスから
読み戻し、ラベル帯の中の文字色ピクセルを数えると確定的に判定できる。帯は明色・文字は暗色なので
「暗いピクセルが 0 なら、帯は出ているが文字が無い」と言い切れる。

```c
static int count_text_pixels(SkSurface* surface, const SkIRect& rect) {
    if (rect.isEmpty()) {
        return -1;
    }
    SkImageInfo info = SkImageInfo::Make(rect.width(), rect.height(),
                                         kRGBA_8888_SkColorType, kPremul_SkAlphaType);
    size_t   row_bytes = (size_t)rect.width() * 4;
    uint8_t* pixels    = (uint8_t*)g_malloc(row_bytes * (size_t)rect.height());
    if (!surface->readPixels(info, pixels, row_bytes, rect.x(), rect.y())) {
        g_free(pixels);
        return -2;
    }
    int dark = 0;
    for (int y = 0; y < rect.height(); y++) {
        const uint8_t* row = pixels + (size_t)y * row_bytes;
        for (int x = 0; x < rect.width(); x++) {
            const uint8_t* p = row + x * 4;
            if (p[3] > 128 && p[0] < 96 && p[1] < 96 && p[2] < 96) {
                dark++;
            }
        }
    }
    g_free(pixels);
    return dark;
}
```

複数ストリームがあるときは**最小値**を報告する。1 つのストリームだけで文字が消える事象を
取りこぼさないため。

### H-4. 落とし穴 — 読み戻した値は信用できない。二値すら当てにならなかった（重要）

> ⚠️ この節は 2 度書き直している。①当初「圧縮バッファだから読み戻せない」→ 非圧縮でも同じ変動が
> 出たので因果が誤り。②次に「絶対値は駄目だが二値（0 / 非ゼロ）は信用できる」→ **その二値も
> 目視と食い違っていた**。経緯ごと残す。**この種の測定を安易に信じるな、というのが本節の主題。**

**オーバーレイのサーフェスを `readPixels()` で読み戻した値は、表示内容と一致しない。**

実測 1（同一バイナリ・同一ストリーム・非圧縮バッファ、パス描画）:

| フレーム | 読み戻した文字ピクセル数 | 目視 |
|---|---|---|
| 1 フレーム目 | 318 | 文字あり |
| 2 〜 60 フレーム目 | **21**（一定） | 文字あり（60 秒間ずっと安定） |

最初は `AXO_FORMAT_FLAGS_COMPRESSED` を付けたバッファで気づいたため圧縮が原因だと考えたが、
**圧縮を外しても steady state では同じように値が落ちる**。

実測 2（`drawString` の帯）: 読み戻しは **60/60 サンプルで 0 px**。しかし**同じ画面を見ていた
人間には、その帯に文字が読めていた**。

つまり絶対値だけでなく、**「0 = 文字が無い」という二値の判定すら誤っていた**。
最も疑わしいのは、レイアウト変更で**測定矩形がずれ、文字のない領域を数えていた**こと。

#### この節の本当の教訓は、測定の誤りより「判断の構造」

このプロジェクトでは次の順に間違えた:

1. 読み戻しという**未検証の測定手段**を作った
2. その測定が目視と食い違った
3. **測定を正とし、目視のほうを「隣接した帯を読み違えたのだろう」と退けた**
4. 「二値は常に目視と一致した」と結論した — が、その照合先の目視は 3 で自分が退けたもの。
   **論証が循環していた**

新しい測定手段は、**既知の正解で先に検算してから**使うこと。例えば「塗り潰した矩形を描いて、
その面積が読み戻しで正しく出るか」を確かめる。それを飛ばすと、測定が現実を上書きし始める。

そして**測定と人間の観測が食い違ったら、まず測定を疑う**。特にその測定が今回のように
新規で未検証なら、疑う順序は明白なはずだった。

#### 結論

`readPixels()` によるオーバーレイの検証は、**一度きりの原因究明で他の手段と突き合わせながら
使うなら有効。常設の回帰検知には向かない。** 本プロジェクトでは最終的に検証用コードごと削除し、
実装は H-3 のコードとしてここに残した。

参考までに、検証時だけ圧縮を切る場合のコード（ただし上記のとおり、これで正確に読めるように
なるわけではない）:

```c
// Compression is recommended with GPU rendering. Turning it off for the text
// check removes one source of readback noise, but NOT all of it -- see above.
axo_format_flags flags =
    verify_text ? AXO_FORMAT_FLAGS_GPU
                : (axo_format_flags)(AXO_FORMAT_FLAGS_COMPRESSED | AXO_FORMAT_FLAGS_GPU);
axo_detailed_format* format = axo_suggest_detailed_format(AXO_FORMAT_ARGB32, flags, &error);
```

参考までに、切り替えを実装する場合のコード:

```c
// Compression is recommended with GPU rendering. Turning it off for the text
// check removes one source of readback noise, but NOT all of it -- see above.
axo_format_flags flags =
    verify_text ? AXO_FORMAT_FLAGS_GPU
                : (axo_format_flags)(AXO_FORMAT_FLAGS_COMPRESSED | AXO_FORMAT_FLAGS_GPU);
axo_detailed_format* format = axo_suggest_detailed_format(AXO_FORMAT_ARGB32, flags, &error);
```

### H-5. 手順としての教訓

- **新しい測定手段は、既知の正解で検算してから使う。** 今回はこれを飛ばした結果、
  未検証の読み戻しを信じて**実在する現象（ビルドによって drawString が出たり出なかったりする）を
  「そんな現象は無い」と誤って否定した**。塗り潰した矩形を描いて面積が正しく出るか、程度の
  検算で防げた
- **測定と人間の観測が食い違ったら、まず測定を疑う。** 特に測定側が新規で未検証なら順序は明白。
  今回は逆をやり、目視のほうを「読み違えだろう」と退けて 1 サイクル分の誤った結論を出した
- **同一バイナリ内で A/B できるスイッチを持つ。** 「アトラス経由」と「パス描画」を実行時フラグで
  切り替えられるようにしたことで、ビルド差分という交絡を排除して比較できた。ただし
  **測定手段が信用できなければ A/B の結論も信用できない**（今回がそれ）
- **箱とラベルは同じテストで一緒に描く。** 片方だけ出れば、それだけで原因をフォント経路に絞れる。
  検証を分けると目視を 2 往復お願いすることになる
- **人間に見てもらうときは、答えが一意に決まる形にする。** 「両方に文字が見えますか」ではなく
  「**上の帯の中に文字が読めますか（はい/いいえ）**」「下の帯はどうですか」と分けて聞く。
  比較対象は色を変え、明確に離して配置する。今回 2 つのラベル帯を `1.15 × フォントサイズ` しか
  離さずに並べたため、回答の解釈で揉めた（実際には両方に文字が出ていたのだが、
  こちらはそれを読み違いだと解釈した）
- **テストバイナリに自前のストリームを開かせる。** オーバーレイは視聴者のストリームに合成される
  ため、ライブビューが開いていないと何も描かれない。テストバイナリが自分で H.264 ストリームを
  開けば、以後の回帰確認は人手なしで回せる
- **`deploy.sh` はアプリのログをリセットする。** 必ず **デプロイ → 実行 → ログ参照** の順で行い、
  実行とログ参照の間にデプロイを挟まない。これを知らず、取れるはずのログを 1 回落とした
