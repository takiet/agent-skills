# テストプログラムの raw 出力を ffmpeg で確認する

`run.sh` が回収したテストバイナリの生バッファを目で見える形にして検証するための手順集。

このファイルのコマンドは **2026-07-24 に実データ（Phase 2-A の `test_capture` が出した
1,228,800 byte の RGB ダンプ）で全て実行確認済み**。未検証のものは明示してある。

環境: macOS / ffmpeg 8.1.2 (`brew install ffmpeg`)

---

## 0. 前提 — ダンプの取り方

テストバイナリは **結果を stdout、ログを stderr / syslog** に出す設計にしておく。
`run.sh` は stdout だけを `./output` に落とすので、混ぜるとダンプが壊れる。

```bash
bash $SC/run.sh yolov5_detector test_capture     # -> ./output
```

---

## 1. まずサイズを確認する（ffmpeg より先に）

解像度やフォーマットの取り違えは、**サイズを見るのが一番速い**。ffmpeg に食わせる前に確認する。

| フォーマット | バイト数の式 | 640×360 | 640×640 |
|---|---|---|---|
| RGB / BGR interleaved | `w × h × 3` | 691,200 | 1,228,800 |
| NV12 (YUV420SP) | `w × h × 3 / 2` | 345,600 | 614,400 |
| Y800 / gray | `w × h` | 230,400 | 409,600 |
| planar RGB | `w × h × 3` | 691,200 | 1,228,800 |

```bash
ls -l output
# または期待値との突き合わせ
python3 -c "print(len(open('output','rb').read()))"
```

サイズが式と合わないなら、そこで止めて原因を潰す。**row-pitch（ストライド）が幅より大きい**
場合はこの式では合わないので、テストバイナリのログに pitch を出しておくこと。

---

## 2. PNG にして目で見る

### RGB interleaved

```bash
ffmpeg -y -f rawvideo -pix_fmt rgb24 -s 640x640 -i output out.png
```

### NV12

```bash
ffmpeg -y -f rawvideo -pix_fmt nv12 -s 640x360 -i output out.png
```

### Y800 / gray（NV12 の Y 面だけ見たいときにも使える）

```bash
ffmpeg -y -f rawvideo -pix_fmt gray -s 640x360 -i output out.png
```

> NV12 ファイルを `gray` で開くと、Y 面ぶんを 1 フレームとして読んだあと UV 面が半端に残り
> `Invalid buffer size` が出る。**PNG 自体は正しく作られる**ので、この警告は無視してよい。

### R/B が入れ替わっていないかの確認

```bash
ffmpeg -y -f rawvideo -pix_fmt bgr24 -s 640x640 -i output out_bgr.png
```

`rgb24` 版と見比べて、**肌色や空の色が自然なほう**が正しい。
`rgb-interleaved` と `bgr-interleaved` の取り違えは数値チェックでは見つけにくく、目視が最速。

### 表示

```bash
open out.png          # macOS
ffplay -f rawvideo -pix_fmt rgb24 -s 640x640 output   # 変換せず直接見る
```

---

## 3. 一部だけ切り出す

`crop=w:h:x:y` を使う。レターボックスの検証で効く。

```bash
# 画像が入っている領域だけ（上 360 行）
ffmpeg -y -f rawvideo -pix_fmt rgb24 -s 640x640 -i output -vf crop=640:360:0:0 content.png

# パディング領域だけ（下 280 行）— 真っ黒なら PNG が極端に小さくなる
ffmpeg -y -f rawvideo -pix_fmt rgb24 -s 640x640 -i output -vf crop=640:280:0:360 pad.png
ls -l pad.png     # 実測 1,191 byte = 全面同色
```

PNG のファイルサイズが「全ゼロかどうか」の雑だが有効な指標になる。
厳密に確認するなら Python で:

```bash
python3 -c "
d = open('output','rb').read()
content = 640*360*3
print('size', len(d))
print('padding all zero:', set(d[content:]) == {0})
print('last content row max', max(d[359*1920:360*1920]))
print('first pad row max   ', max(d[360*1920:361*1920]))
"
```

---

## 4. よくある失敗と見分け方

| 症状 | 原因 |
|---|---|
| 画像が**斜めにずれる / 階段状にシアーする** | 幅（row-pitch）の指定が実データと違う。1 バイトのずれが行ごとに累積する |
| `Invalid buffer size, packet size N < expected M` | 指定した解像度・フォーマットとファイルサイズが不一致。式に戻って確認 |
| **青と赤が入れ替わって見える** | `rgb24` / `bgr24` の取り違え。larod の `image.output.format` の指定ミス |
| 縦に潰れている / 引き伸ばされている | アスペクト比を維持せずスケールしている（レターボックスになっていない） |
| 下半分が緑や紫になる | NV12 で UV 面が未初期化。RGB 変換前提のバッファをそのまま NV12 として読んでいる可能性も |
| 全面ノイズ | オフセットのずれ。fd offset や先頭のヘッダ分だけずれていないか |
| 上は正常で下だけ壊れる | バッファサイズ不足、または複数ジョブ間で書き込みオフセットがドリフトしている |

**斜めずれの切り分け**: 幅を ±1 して試すと、正しい幅で一気に整う。
row-pitch そのものの解説は [row-pitch.md](row-pitch.md) を参照。

```bash
ffmpeg -y -f rawvideo -pix_fmt rgb24 -s 639x640 -i output skew.png   # わざと 1 ずらして比較
```

---

## 5. 2 つのダンプを比べる

ホスト側の期待画像とデバイスの出力、あるいは変更前後の比較に使う。

### 並べる

```bash
ffmpeg -y -i a.png -i b.png -filter_complex hstack sbs.png
```

### 数値で比べる（PSNR）

```bash
ffmpeg -i a.png -i b.png -filter_complex psnr -f null - 2>&1 | grep -i psnr
# 例: PSNR r:42.93 g:48.45 b:42.07 average:43.72
```

同一なら `inf`。40dB 台なら色変換の誤差程度、20dB 台以下なら明確に別物。

### 差分を可視化する

```bash
ffmpeg -y -i a.png -i b.png \
  -filter_complex "blend=all_mode=difference,eq=contrast=8" diff.png
```

`eq=contrast=8` で微小な差を持ち上げている。**どこが**違うかが分かるので、
「下だけ壊れている」「1 行ずれている」といった構造的な差の判別に強い。

---

## 6. 逆方向 — テスト入力用の raw を作る

テストバイナリに固定入力を食わせたいときは、画像から raw を作る。

```bash
# 画像 -> RGB interleaved 640x640（下パディング付き）
ffmpeg -y -i test.jpg \
  -vf "scale=640:-1,pad=640:640:0:0:black" \
  -f rawvideo -pix_fmt rgb24 test_640x640.rgb
ls -l test_640x640.rgb    # 1228800 になるはず

# 画像 -> NV12 640x360
ffmpeg -y -i test.jpg -vf scale=640:360 -f rawvideo -pix_fmt nv12 test_640x360.nv12
```

`pad=w:h:x:y:color` の `x:y` が貼り付け位置。`0:0` で上詰め＝下パディングになり、
デバイス側の前処理と一致する。

---

## 7. Phase 2-A で実際に使ったコマンド

記録として。

```bash
# 1. ダンプ取得
bash $SC/run.sh yolov5_detector test_capture

# 2. サイズ確認 -> 1,228,800 ✅
ls -l output

# 3. 全体を PNG 化（上 360 行に非ストレッチの映像、下 280 行が黒であること）
ffmpeg -y -f rawvideo -pix_fmt rgb24 -s 640x640 -i output rgb640.png

# 4. パディング領域が全ゼロであることを厳密に確認
python3 -c "
d=open('output','rb').read()
print(set(d[640*360*3:])=={0}, max(d[359*1920:360*1920]), max(d[360*1920:361*1920]))
"
# -> True 212 0  ✅ 境界も一致

# 5. R/B 反転がないことを NV12 の参照画像と見比べて確認
ffmpeg -y -f rawvideo -pix_fmt bgr24 -s 640x640 -i output rgb640_bgr.png
```

---

## 8. 未検証

- **planar RGB**（`VDO_FORMAT_PLANAR_RGB`）の扱い。ffmpeg の `gbrp` は **G, B, R の順**なので
  RGB プレーン順のデータをそのまま渡すと色がずれる。`-vf shuffleplanes` での並べ替えが要るはずだが
  **未検証**。使うときは実データで確認すること。
- JPEG / H.264 ダンプの確認（`ffmpeg -i output out.png` で開けるはずだが、このプロジェクトでは未使用）。
- row-pitch が幅より大きい（パディング付きストライド）ケース。ffmpeg の rawvideo は
  ストライド指定ができないので、Python で行ごとに切り出す前処理が要る。
