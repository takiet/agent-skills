from PIL import Image


def resize_and_pad_top_left_pillow(img, target_size=640):
    """入力画像をアスペクト比維持で縮小し、左上詰めで640x640にパディングする。

    Args:
        img (PIL.Image.Image): Pillowで読み込んだ画像
        target_size (int): 出力サイズ

    Returns:
        PIL.Image.Image: 処理後の640x640の画像
    """
    if img is None:
        raise ValueError("入力画像が空（None）です。")

    w, h = img.size

    # 640x640より大きい場合のみ、アスペクト比を維持して縮小
    if h > target_size or w > target_size:
        if w > h:
            # 横長の場合：幅を640にする
            new_w = target_size
            new_h = int(h * (target_size / w))
        else:
            # 縦長または正方形の場合：高さを640にする
            new_h = target_size
            new_w = int(w * (target_size / h))

        # 高品質なリサイズ（Resampling.LANCZOS）
        img = img.resize((new_w, new_h), Image.Resampling.LANCZOS)

    # 640x640 の黒背景画像を作成
    new_img = Image.new("RGB", (target_size, target_size), (0, 0, 0))

    # 左上 (0, 0) に縮小後の画像を貼り付け
    new_img.paste(img, (0, 0))

    return new_img


# --- 使い方（実行例） ---
if __name__ == "__main__":
    # 画像の読み込み
    with Image.open("bus.jpg") as input_img:
        # 関数を実行
        output_img = resize_and_pad_top_left_pillow(input_img)

        # 保存
        output_img.save("output_pillow.jpg", "JPEG")

