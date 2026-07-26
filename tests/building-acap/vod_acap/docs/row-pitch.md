# row-pitch（ストライド）とレターボックスの仕組み

VDO / larod のテンソルで指定する `row-pitch` の意味と、それを使って下パディングを
実現している仕組みの記録。Phase 2-A の実装（`app/capture.cc`）の背景説明。

関連: [verify-raw-dumps.md](verify-raw-dumps.md)（ピッチを間違えたときの症状の確認方法）

---

## 1. 定義

**row-pitch（= ストライド）は「ある行の先頭から、次の行の先頭までのバイト数」。**
「画像の幅」ではなく「メモリ上で行が何バイトおきに並んでいるか」を表す。

このプロジェクトで使っている値:

| 用途 | フォーマット | 幅 | ピッチ | 内訳 |
|---|---|---|---|---|
| 前処理の入力 | NV12（Y 面） | 640 px | **640 byte** | 1 画素 1 バイト |
| 前処理の出力 | RGB interleaved | 640 px | **1920 byte** | 640 px × 3 byte (R,G,B) |

NV12 入力側のピッチは決め打ちせず、VDO の `info` マップから実測値を取っている
（`app/capture.cc:84`）。デバイスやストリーム設定によってはアライメントのため
幅より大きい値が返る可能性があるため。

```c
capture->src_pitch = vdo_map_get_uint32(info, "pitch", 0);
```

---

## 2. なぜ「幅 × バイト数」と別概念なのか

多くの場合は一致するが、**一致しないことがある**から別パラメータになっている。

ハードウェアは行の先頭が 16 / 32 / 64 バイト境界に揃っていると速いので、幅が半端なときに
行末へ詰め物が入ることがある。例えば幅 641 px の RGB interleaved なら:

```
実データ  641 × 3 = 1923 byte
ピッチ                1984 byte   ← 61 byte のパディングが行末に付く
```

このとき幅だけ知っていてもデータは読めない。**次の行がどこから始まるかはピッチにしか
書かれていない。** だから larod も VDO も幅とピッチを別々に持つ。

---

## 3. 下パディングが成立する仕組み

Phase 2-A の肝。出力テンソルは **640×360（コンテンツ領域だけ）** を記述しているが、
裏付けている memfd は **640×640×3 = 1,228,800 byte**（`app/capture.cc:176-208`）。

```
memfd (1,228,800 byte) を 1920 byte ごとに区切って見た図

offset       0 ┌────────────────────────┐ ← 行 0    ┐
          1920 │                        │ ← 行 1    │ larod が
          3840 │      640×360 の映像     │ ← 行 2    │ 360 行だけ
               │                        │   ...     │ 書き込む
        689280 │                        │ ← 行 359  ┘
        691200 ├────────────────────────┤ ← 行 360  ┐
               │                        │           │ 一度もアクセス
               │   memset(0) のまま      │           │ されない
               │                        │           │ = 下パディング
       1228800 └────────────────────────┘ ← 行 639  ┘
```

`larodSetTensorFdOffset(tensor, 0)` で先頭から書き始め、ピッチが 1920 なので
**n 行目は必ず offset `n × 1920` に着地する**。これは 640×640 のキャンバスとして見たときの
n 行目と完全に同じ位置。

### パラメータの役割分担

| パラメータ | 値 | 役割 |
|---|---|---|
| dims | 640 × 360 × 3 | larod が **どれだけ書くか** |
| pitch | 1920 | それを **どの幅のキャンバスに配置するか** |
| fdOffset | 0 | 書き始めの位置 |
| fdSize | 1,228,800 | バッファ全体のサイズ（コンテンツ分ではない） |

360 行ぶん書いた時点で larod は仕事を終えるので、残り 280 行はゼロクリアされたまま残る。
**パディング用のコピーもクリア処理も一切不要。**

`fdSize` に content サイズ（691,200）ではなく全体（1,228,800）を渡しているのは、
このバッファ全体が 1 枚の 640×640 画像として Phase 2-B の推論入力へそのまま渡るため。

---

## 4. ピッチが「幅」から独立していることの価値

仮に縦長の映像でコンテンツが 360×640 になったとする。ピッチを 1920（キャンバス幅）のまま
にしておけば:

```
行 0:  [ 360px の映像 ][ ← 280px ぶんはゼロのまま → ]  次の行は 1920 byte 先
行 1:  [ 360px の映像 ][ ← 280px ぶんはゼロのまま → ]
```

と**右側がパディングされた（ピラーボックス）配置**になる。ピッチが画像の幅から
切り離されているおかげで、書き込み側は何も知らないまま任意の位置・任意のキャンバスに
嵌め込める。

---

## 5. API での指定方法

### larod テンソル（SDK 12.11.0 のヘッダで確認済み）

```c
bool larodBuildTensorDims(larodTensor* tensor, const larodTensorLayout layout,
                          const size_t width, const size_t height,
                          const size_t num_channels, larodError** error);

bool larodBuildTensorPitches(larodTensor* tensor,
                             const larodTensorLayout layout, const size_t pitch,
                             const size_t height, const size_t num_channels,
                             larodError** error);
```

**`larodBuildTensorPitches` の第 3 引数はバイト単位の行ピッチそのもの**（幅ではない）。
`larodBuildTensorDims` の第 3 引数が画素単位の幅なのと対になっていて紛らわしいので注意。

実際の呼び出し（`app/capture.cc:192-203`）:

```c
larodBuildTensorDims(tensor,    LAROD_TENSOR_LAYOUT_NHWC, 640, 360, 3, &error)
larodBuildTensorPitches(tensor, LAROD_TENSOR_LAYOUT_NHWC, 1920, 360, 3, &error)
//                                                        ^^^^ byte 単位
```

### larod 前処理ジョブの larodMap

```c
larodMapSetInt(map, "image.input.row-pitch",  640,  &error);   // NV12 Y 面
larodMapSetInt(map, "image.output.row-pitch", 1920, &error);   // RGB interleaved
```

これらのキー名は `larod.h` に記載がなく、公式の `object-detection-yolov5` サンプルで確認した。

---

## 6. 間違えるとどうなるか

読み手が想定するピッチと実際のピッチが `d` バイトずれていると、n 行目の先頭が `n × d` バイト
ずれる。**ずれが行ごとに累積する**ので、画像全体が平行四辺形状に流れる（斜めシアー）。

1 バイトのずれでも 640 行目では 640 バイト ≒ 213 画素ずれるので、PNG 化すれば一目で分かる。

切り分け手順は [verify-raw-dumps.md](verify-raw-dumps.md) の 4 章を参照。幅を ±1 して
ずれの向きを見ると正しい値に当たりが付く:

```bash
ffmpeg -y -f rawvideo -pix_fmt rgb24 -s 639x640 -i output skew.png
```

---

## 7. コードを触るときの注意

`app/capture.cc:200` は出力ピッチを `content_width * 3` と書いている。これが正しいのは
`app/capture.cc:301` で `content_width = dst_width`（＝常に 640）としているから。

```c
capture->content_width  = dst_width;
capture->content_height = (unsigned)((uint64_t)src_height * dst_width / src_width);
```

つまり現状は**必ず幅基準でフィット（＝下パディング）**する前提。

**将来「高さ基準でフィット（ピラーボックス）」に対応すると `content_width < dst_width` に
なり、ピッチは `content_width * 3` ではなく `dst_width * 3` でなければならなくなる。**
ここを追従し忘れると 6 章の斜めずれがそのまま出る。

なお現状のコードは `content_height > dst_height` になるケースを検出してエラーにしている
（`app/capture.cc:303`）ので、前提が崩れたまま黙って動くことはない。

---

## 8. 未検証

- ARTPEC-9 の NV12 ストリームで、幅 640 以外の設定にしたときに VDO が返す `pitch` が
  幅と一致するかどうか。今回は 640×360 でのみ確認しており、`pitch = 640`（＝幅と一致）だった。
- ffmpeg の rawvideo デマクサはストライド指定ができないため、**ピッチ > 幅×bpp のデータは
  そのままでは PNG 化できない**。Python で行ごとに切り出す前処理が必要（未実装）。
