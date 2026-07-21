#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""
全息投影小狗资产背景抠黑脚本
(Holographic Assets Background Remover)

本脚本提供两种模式去除灰白背景，并将去除的区域替换为纯黑（全息投影中的透明），而不影响小狗主体：
1. rembg (推荐): 基于深度学习(U2-Net)的智能无损抠图。首次运行会自动下载模型。
2. floodfill (备选): 基于 OpenCV 的边缘泛洪填充。适用于边缘清晰的纯色或渐变背景。

依赖安装:
pip install rembg opencv-python numpy pillow

使用示例:
python remove_background.py -i input_frames_folder -o output_frames_folder -m rembg
"""

import os
import argparse
import glob
import cv2
import numpy as np
from PIL import Image

def process_with_rembg(input_path, output_path):
    try:
        from rembg import remove, new_session
    except ImportError:
        print("错误: 未安装 rembg 库。请运行 `pip install rembg`。")
        return False

    print(f"[Rembg] 正在处理: {input_path} ...")
    
    # 使用 U2net 模型（默认推荐模型）
    session = new_session("u2net")
    
    # 使用 PIL 打开图像
    input_img = Image.open(input_path).convert("RGBA")
    
    # 移除背景，得到透明背景(RGBA)图像
    output_img = remove(input_img, session=session)
    
    # 创建一个纯黑背景
    black_bg = Image.new("RGBA", output_img.size, (0, 0, 0, 255))
    
    # 将抠除背景后的图片（透明区域）叠加到纯黑背景上
    # 第三个参数用 output_img 作为 Mask，仅把小狗贴在黑色画布上
    black_bg.paste(output_img, (0, 0), output_img)
    
    # 转换为 RGB 并保存（去掉 Alpha 通道）
    final_img = black_bg.convert("RGB")
    final_img.save(output_path, quality=95)
    return True

def process_with_floodfill(input_path, output_path, tolerance=10):
    print(f"[FloodFill] 正在处理: {input_path} ...")
    
    # 读取原始图像
    img = cv2.imread(input_path)
    if img is None:
        print(f"无法读取图像: {input_path}")
        return False
        
    h, w = img.shape[:2]
    
    # 创建 FloodFill 所需的 Mask，大小比原图大两圈 (H+2, W+2)，类型为 np.uint8，全为 0
    mask = np.zeros((h + 2, w + 2), np.uint8)
    
    # 定义容差：(loDiff, upDiff)。这里表示与起始像素比较
    loDiff = (tolerance, tolerance, tolerance)
    upDiff = (tolerance, tolerance, tolerance)
    
    # 纯黑色 (BGR in OpenCV)
    fill_color = (0, 0, 0)
    
    # 我们假设背景连接到图像的四个角，以此为种子点进行 Flood Fill 泛洪
    seed_points = [
        (0, 0), 
        (0, h - 1), 
        (w - 1, 0), 
        (w - 1, h - 1)
    ]
    
    for pt in seed_points:
        cv2.floodFill(img, mask, pt, fill_color, loDiff, upDiff, flags=cv2.FLOODFILL_FIXED_RANGE)
        
    cv2.imwrite(output_path, img)
    return True

def main():
    parser = argparse.ArgumentParser(description="批量去除小狗图片的背景，并在全息投影环境替换为纯黑色。")
    parser.add_argument("-i", "--input", required=True, help="输入图片路径，或包含图片序列的文件夹路径。")
    parser.add_argument("-o", "--output", required=True, help="输出保存的文件夹路径。")
    parser.add_argument("-m", "--method", choices=["rembg", "floodfill"], default="rembg", 
                        help="抠图算法选择：'rembg' (AI大模型，强烈推荐) 或 'floodfill' (OpenCV泛洪)。默认 rembg。")
    parser.add_argument("-t", "--tolerance", type=int, default=15, 
                        help="仅适用于 floodfill：背景容差，默认15。调大可移除更多渐变，调小避免侵入小狗边缘。")
    
    args = parser.parse_args()

    input_path = args.input
    output_dir = args.output
    
    if not os.path.exists(output_dir):
        os.makedirs(output_dir)
        
    # 获取需要处理的文件列表
    files_to_process = []
    if os.path.isfile(input_path):
        files_to_process.append(input_path)
    elif os.path.isdir(input_path):
        # 支持常见图片格式
        extensions = ('*.png', '*.jpg', '*.jpeg', '*.bmp')
        for ext in extensions:
            files_to_process.extend(glob.glob(os.path.join(input_path, ext)))
            files_to_process.extend(glob.glob(os.path.join(input_path, ext.upper())))
    else:
        print(f"输入路径不存在: {input_path}")
        return

    if not files_to_process:
        print("没有找到任何可处理的图片！")
        return
        
    print(f"找到 {len(files_to_process)} 张图片，使用 '{args.method}' 方法进行处理。")
    
    success_count = 0
    for idx, file_path in enumerate(files_to_process, 1):
        filename = os.path.basename(file_path)
        out_path = os.path.join(output_dir, filename)
        
        # 处理流程
        if args.method == "rembg":
            success = process_with_rembg(file_path, out_path)
        else:
            success = process_with_floodfill(file_path, out_path, tolerance=args.tolerance)
            
        if success:
            success_count += 1
            
    print(f"处理完成！成功 {success_count}/{len(files_to_process)} 张图片。保存在 {output_dir}")

if __name__ == "__main__":
    main()
