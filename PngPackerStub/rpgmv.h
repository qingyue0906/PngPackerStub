#pragma once
#include <vector>
#include <string>
#include <cstdint>

// RPGMV 文件类型
enum class RpgmvType : uint8_t
{
    PNG = 0,  // 标准 PNG
    RPGMVP = 1,  // .rpgmvp 加密图片
    PNG_ = 2,  // .png_ 加密图片
    OTHER = 3,  // 其他文件，不做图片处理
    DIRECTORY = 4,  // 空文件夹
};

// RPG Maker MV/MZ 伪造文件头（16字节）
static const uint8_t RPGMV_FAKE_HEADER[16] = {
    0x52, 0x50, 0x47, 0x4D, 0x56, 0x00, 0x00, 0x00,
    0x00, 0x03, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00
};

// 标准 PNG 前16字节
static const uint8_t PNG_HEADER_16[16] = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,
    0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52
};

// 标准 PNG 前8字节签名
static const uint8_t PNG_SIG[8] = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A
};

// 检测文件的实际类型（读取前16字节进行校验，不依赖后缀）
// 返回 OTHER 表示既不是 PNG 也不是 RPGMV 加密图片
RpgmvType detect_file_type(const std::string& path);

// 检测是否为 APNG（动态PNG），APNG 不能用 split_png 处理
bool is_apng(const std::string& path);

// 从一个 RPGMVP/PNG_ 文件中反推16字节密钥
// 成功返回 true，key 填充16字节
bool derive_key(const std::string& path, uint8_t key[16]);

// 解密 RPGMVP/PNG_ 文件，输出标准 PNG 数据到 out_data
// 成功返回 true
bool decrypt_rpgmv(const std::string& path, std::vector<uint8_t>& out_data);

// 用已知密钥解密 RPGMVP/PNG_ 文件，输出标准 PNG 数据
bool decrypt_rpgmv_with_key(const std::string& path,
    const uint8_t key[16],
    std::vector<uint8_t>& out_data);

// 将标准 PNG 数据加密为 RPGMV 格式，写入 out_path
bool encrypt_rpgmv(const std::vector<uint8_t>& png_data,
    const uint8_t key[16],
    const std::string& out_path);