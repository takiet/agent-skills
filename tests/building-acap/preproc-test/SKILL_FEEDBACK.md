# building-acap スキルへのフィードバック

対象: `skills/building-acap`（2026-07-31 時点）
報告元: ppcomp（ARTPEC-9 前処理ベンチマーク）の実装セッション

## 要約

`manifest.json` の `resources` を `acapPackageConf` の**中**に入れる誤りが、SKILL.md 側を修正した
**後**にも再発しました。SKILL.md の記述は正しく、原因はそこではありません。**エージェントが
SKILL.md を通らない経路で入ってくる**ことが原因です。

## 何が起きたか（実測）

サブエージェントが実際に `Read` したスキルファイルをトランスクリプトから確認しました。

| 作業 | Read したファイル |
|---|---|
| 増分 2-A | `references/vdo.md`, `references/larod.md` |
| 増分 2-B | `references/larod.md` |
| 増分 2-C / 2-D | なし |

**`SKILL.md` は一度も読まれていません。**トランスクリプト中の `SKILL.md` は `ls` / `find` の
出力にファイル名として現れるだけです。実装エージェントには `Skill` ツールが無く（`tools:` が
`Bash, Read, Write, Edit, Glob, Grep`）、必要な API のリファレンスへパス直打ちで到達しました。

## 原因

エージェントが読んだ `references/vdo.md:22-25` はこうなっています。

````
`manifest.json` — the app user must belong to the `video` group:

```json
"resources": { "linux": { "user": { "groups": ["video"] } } }
```
````

**断片だけで、外側の器がありません。**`schemaVersion` も `acapPackageConf` も見えないため、
「`resources` が何にぶら下がるか」の情報がゼロです。この断片を持って、本体が丸ごと
`acapPackageConf` に包まれた `manifest.json` を開けば、その中に入れるのが自然な推論になります。

`SKILL.md:272` の
「a sibling of `acapPackageConf`, not nested inside it」＋ JSON 例は**正確で十分**ですが、
エージェントが通らない場所にあります。人間は SKILL.md を頭から読むので効きますが、
エージェントは目的の API のリファレンスに直行します。

## 提案

### 1（本命）リファレンス側の断片に器を付ける

同じ形の裸の断片が **7 ファイル・8 箇所**あります。

`bbox.md:29` / `ddh.md:25` / `larod.md:22` / `overlay.md:34` / `overlay.md:41` /
`serial.md:22` / `storage.md:22` / `vdo.md:25`

いずれも `"resources"` から始まり、親が見えません。例えば `vdo.md` なら：

```json
{
  "schemaVersion": "2.1.0",
  "resources": { "linux": { "user": { "groups": ["video"] } } },
  "acapPackageConf": { "setup": { "appName": "..." } }
}
```

`acapPackageConf` の行を 1 行入れるだけで、「兄弟である」ことが断片そのものから読み取れます。
コピーされる単位に情報を載せるのが要点で、断片の外に置いた注意書きは届きません。

### 2 `SKILL.md:272` は変更不要

記述は正しく、例も適切です。ここを更に強調しても今回の経路には効きません。

### 3（設計方針）リファレンスは単体で完結させる

今回の教訓を一般化すると、**各 `references/*.md` は SKILL.md を読んでいない読み手を前提に
書く**ということです。エージェントは目次から辿らず、必要なページに直接跳びます。
SKILL.md にしか書かれていない前提は、リファレンス経由の利用者には存在しないのと同じです。

`resources` の位置以外にも、SKILL.md にしか無い前提がリファレンス側で暗黙になっていないか、
一度洗い出す価値があると思います。

## 補足（修正済みの確認）

`references/larod.md` の Device names 表は、ARTPEC-9 の行が実機の値（`a9-dlpu-tflite` /
`cpu-proc` / `a9-gpu-proc`、`axis-` 接頭辞なし）に修正済みで、
冒頭に「**Ask the device, don't trust this page.**」も追加されていました。以前報告した
食い違いは解消しています。今回のセッションでもこの表と実機の `larodListDevices()` の出力は
一致しました。

## 参考: 今回の実測値（ARTPEC-9 / Q1728、n=50）

`a9-gpu-proc` は **NV12→RGB interleaved の色変換しか持ちません**。`rgb-planar` を要求すると
`Could not run job: This backend only supports color conversion from NV12 to RGB interleaved`
で落ちます。前処理を GPU に逃がす価値があったのは scale のみでした。

| シナリオ | cpu-proc | a9-gpu-proc |
|---|---:|---:|
| crop 1920x1080→300x300 (NV12) | 1.015 ms | 3.838 ms |
| scale 1920x1080→300x300 (NV12) | 9.406 ms | 4.487 ms |
| NV12→RGB interleaved | 7.539 ms | 8.985 ms |
| NV12→RGB planar | 22.118 ms | N/A |

`references/larod.md` の前処理の節に、この「GPU が常に速いわけではない」「planar は非対応」
という手掛かりがあると、デバイス選択の判断が早くなると思います。
