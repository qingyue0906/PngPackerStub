#pragma once
#include <vector>
#include <string>
#include <cstdint>

// 存放拆分结果的结构体
struct PngParts
{
    std::vector<uint8_t> meta;   // 元数据骨架
    std::vector<uint8_t> pixels; // 解压后的原始像素流
};

// 将一个 PNG 文件拆分为元数据骨架 + 原始像素流
// 成功返回 true，失败返回 false
bool split_png(const std::string& png_path, PngParts& out);

// 将元数据骨架 + 原始像素流还原为完整 PNG 文件
// 成功返回 true，失败返回 false
bool restore_png(const std::vector<uint8_t>& meta,
    const std::vector<uint8_t>& pixels,
    const std::string& out_path);