#include "rpgmv.h"
#include <fstream>
#include <cstring>
#include <cstdio>

// 读取文件前 N 字节
static bool read_header(const std::string& path, uint8_t* buf, size_t n)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.read((char*)buf, n);
    return (size_t)f.gcount() == n;
}

// 读取整个文件
static bool read_file(const std::string& path, std::vector<uint8_t>& out)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    auto sz = f.tellg();
    f.seekg(0);
    out.resize((size_t)sz);
    f.read((char*)out.data(), sz);
    return f.good();
}

RpgmvType detect_file_type(const std::string& path)
{
    uint8_t buf[16];
    if (!read_header(path, buf, 16))
        return RpgmvType::OTHER;

    // 检查是否为标准 PNG
    if (memcmp(buf, PNG_SIG, 8) == 0)
        return RpgmvType::PNG;

    // 检查是否为 RPGMV 加密格式
    if (memcmp(buf, RPGMV_FAKE_HEADER, 16) == 0)
    {
        // 根据文件后缀区分 rpgmvp 和 png_
        std::string lower = path;
        for (auto& c : lower) c = (char)tolower(c);
        if (lower.size() >= 7 && lower.substr(lower.size() - 7) == ".rpgmvp")
            return RpgmvType::RPGMVP;
        if (lower.size() >= 5 && lower.substr(lower.size() - 5) == ".png_")
            return RpgmvType::PNG_;
        // 头部匹配但后缀不认识，也当 RPGMVP 处理
        return RpgmvType::RPGMVP;
    }

    return RpgmvType::OTHER;
}

bool is_apng(const std::string& path)
{
    // 读取前 1024 字节，检查是否含有 acTL chunk（APNG 特有）
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    uint8_t buf[1024];
    f.read((char*)buf, sizeof(buf));
    size_t n = (size_t)f.gcount();
    // 搜索 "acTL" 标志
    for (size_t i = 0; i + 4 <= n; i++)
        if (memcmp(buf + i, "acTL", 4) == 0)
            return true;
    return false;
}

bool derive_key(const std::string& path, uint8_t key[16])
{
    std::vector<uint8_t> data;
    if (!read_file(path, data)) return false;
    if (data.size() < 32) return false;
    // 验证伪造文件头
    if (memcmp(data.data(), RPGMV_FAKE_HEADER, 16) != 0) return false;
    // 已知明文攻击：加密数据[16..31] XOR PNG_HEADER_16 = key
    for (int i = 0; i < 16; i++)
        key[i] = data[16 + i] ^ PNG_HEADER_16[i];
    return true;
}

bool decrypt_rpgmv(const std::string& path, std::vector<uint8_t>& out_data)
{
    uint8_t key[16];
    if (!derive_key(path, key)) return false;
    return decrypt_rpgmv_with_key(path, key, out_data);
}

bool decrypt_rpgmv_with_key(const std::string& path,
    const uint8_t key[16],
    std::vector<uint8_t>& out_data)
{
    std::vector<uint8_t> data;
    if (!read_file(path, data)) return false;
    if (data.size() < 16) return false;
    if (memcmp(data.data(), RPGMV_FAKE_HEADER, 16) != 0) return false;

    // 跳过伪造头，取出数据体
    out_data.assign(data.begin() + 16, data.end());

    // 异或还原前16字节
    size_t xor_len = (out_data.size() < 16) ? out_data.size() : 16;
    for (size_t i = 0; i < xor_len; i++)
        out_data[i] ^= key[i];

    return true;
}

bool encrypt_rpgmv(const std::vector<uint8_t>& png_data,
    const uint8_t key[16],
    const std::string& out_path)
{
    std::ofstream f(out_path, std::ios::binary);
    if (!f) return false;

    // 写伪造文件头
    f.write((const char*)RPGMV_FAKE_HEADER, 16);

    // 复制 PNG 数据，异或加密前16字节
    std::vector<uint8_t> body(png_data);
    size_t xor_len = (body.size() < 16) ? body.size() : 16;
    for (size_t i = 0; i < xor_len; i++)
        body[i] ^= key[i];

    f.write((const char*)body.data(), body.size());
    return f.good();
}