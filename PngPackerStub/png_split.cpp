#include "png_split.h"
#include "zlib/zlib.h"
#include <fstream>
#include <cstring>

// 从缓冲区读取 4 字节大端整数
static uint32_t read_u32_be(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) |
        ((uint32_t)p[1] << 16) |
        ((uint32_t)p[2] << 8) |
        ((uint32_t)p[3]);
}

// 把 4 字节大端整数写入缓冲区
static void write_u32_be(uint8_t* p, uint32_t v)
{
    p[0] = (v >> 24) & 0xFF;
    p[1] = (v >> 16) & 0xFF;
    p[2] = (v >> 8) & 0xFF;
    p[3] = v & 0xFF;
}

// 读取整个文件到 vector
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

// 把 vector 写入文件
static bool write_file(const std::string& path, const std::vector<uint8_t>& data)
{
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write((const char*)data.data(), data.size());
    return f.good();
}

bool split_png(const std::string& png_path, PngParts& out)
{
    std::vector<uint8_t> file;
    if (!read_file(png_path, file)) {
        printf("[错误] 无法读取文件: %s\n", png_path.c_str());
        return false;
    }

    // 检查 PNG 签名（前 8 字节固定）
    const uint8_t PNG_SIG[8] = { 0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A };
    if (file.size() < 8 || memcmp(file.data(), PNG_SIG, 8) != 0) {
        printf("[错误] 不是标准 PNG 文件: %s\n", png_path.c_str());
        return false;
    }

    out.meta.clear();
    out.pixels.clear();

    std::vector<uint8_t> raw_idat; // 收集所有 IDAT 的原始压缩数据
    size_t pos = 8;                // 跳过签名，从第 9 字节开始读 chunk

    while (pos + 8 <= file.size())
    {
        uint32_t length = read_u32_be(file.data() + pos);
        uint8_t  type[4];
        memcpy(type, file.data() + pos + 4, 4);

        // 检查是否越界
        if (pos + 8 + length + 4 > file.size()) break;

        if (memcmp(type, "IDAT", 4) == 0)
        {
            // 收集 IDAT 数据，在 meta 里写一个长度为 0 的占位块
            raw_idat.insert(raw_idat.end(),
                file.data() + pos + 8,
                file.data() + pos + 8 + length);

            // 占位：写 [长度=0][类型=IDAT]，不写数据
            uint8_t placeholder[8];
            write_u32_be(placeholder, 0);
            memcpy(placeholder + 4, "IDAT", 4);
            out.meta.insert(out.meta.end(), placeholder, placeholder + 8);
        }
        else
        {
            // 非 IDAT 块：原样写入 meta（长度+类型+数据，不含CRC）
            out.meta.insert(out.meta.end(),
                file.data() + pos,
                file.data() + pos + 8 + length);
        }

        pos += 8 + length + 4; // 跳过 CRC（4字节）

        if (memcmp(type, "IEND", 4) == 0) break;
    }

    if (raw_idat.empty()) {
        printf("[错误] 未找到 IDAT 数据: %s\n", png_path.c_str());
        return false;
    }

    // 用 zlib 解压 IDAT 数据得到原始像素流
    // 先用一个较大的缓冲区，PNG 解压后通常是宽*高*(通道数+1)字节
    uLongf pixels_size = (uLongf)(raw_idat.size() * 6 + 1024);
    out.pixels.resize(pixels_size);

    int ret = uncompress(out.pixels.data(), &pixels_size,
        raw_idat.data(), (uLong)raw_idat.size());

    // 缓冲区不够就翻倍扩大，最多重试 8 次
    int retry = 0;
    while (ret == Z_BUF_ERROR && retry < 8)
    {
        pixels_size *= 2;
        out.pixels.resize(pixels_size);
        ret = uncompress(out.pixels.data(), &pixels_size,
            raw_idat.data(), (uLong)raw_idat.size());
        retry++;
    }

    if (ret != Z_OK) {
        printf("[错误] zlib 解压失败，错误码: %d，文件: %s\n", ret, png_path.c_str());
        return false;
    }

    out.pixels.resize(pixels_size); // 裁剪到实际大小
    return true;
}

bool restore_png(const std::vector<uint8_t>& meta,
    const std::vector<uint8_t>& pixels,
    const std::string& out_path)
{
    // 重新压缩像素流
    uLongf compressed_size = compressBound((uLong)pixels.size());
    std::vector<uint8_t> compressed(compressed_size);

    int ret = compress2(compressed.data(), &compressed_size,
        pixels.data(), (uLong)pixels.size(), 6);
    if (ret != Z_OK) {
        printf("[错误] zlib 压缩失败，错误码: %d\n", ret);
        return false;
    }
    compressed.resize(compressed_size);

    // 开始组装 PNG 文件
    std::vector<uint8_t> out_file;

    // 写入 PNG 签名
    const uint8_t PNG_SIG[8] = { 0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A };
    out_file.insert(out_file.end(), PNG_SIG, PNG_SIG + 8);

    bool idat_written = false;
    size_t pos = 0;

    while (pos + 8 <= meta.size())
    {
        uint32_t length = read_u32_be(meta.data() + pos);
        uint8_t  type[4];
        memcpy(type, meta.data() + pos + 4, 4);

        if (memcmp(type, "IDAT", 4) == 0)
        {
            if (!idat_written)
            {
                // 写真正的 IDAT：长度 + 类型 + 数据 + CRC
                uint8_t hdr[8];
                write_u32_be(hdr, (uint32_t)compressed_size);
                memcpy(hdr + 4, "IDAT", 4);
                out_file.insert(out_file.end(), hdr, hdr + 8);
                out_file.insert(out_file.end(),
                    compressed.data(),
                    compressed.data() + compressed_size);

                // 计算 CRC（覆盖类型+数据）
                uLong crc = crc32(0L, Z_NULL, 0);
                crc = crc32(crc, (const Bytef*)"IDAT", 4);
                crc = crc32(crc, compressed.data(), (uInt)compressed_size);
                uint8_t crc_buf[4];
                write_u32_be(crc_buf, (uint32_t)crc);
                out_file.insert(out_file.end(), crc_buf, crc_buf + 4);

                idat_written = true;
            }
            // 后续的 IDAT 占位块跳过
            pos += 8 + length;
        }
        else
        {
            // 非 IDAT 块：长度+类型+数据 原样写回，然后重新计算 CRC
            uint32_t chunk_data_len = length;
            const uint8_t* chunk_data_ptr = meta.data() + pos + 8;

            // 写长度+类型+数据
            out_file.insert(out_file.end(),
                meta.data() + pos,
                meta.data() + pos + 8 + chunk_data_len);

            // 计算并写 CRC
            uLong crc = crc32(0L, Z_NULL, 0);
            crc = crc32(crc, type, 4);
            if (chunk_data_len > 0)
                crc = crc32(crc, chunk_data_ptr, chunk_data_len);
            uint8_t crc_buf[4];
            write_u32_be(crc_buf, (uint32_t)crc);
            out_file.insert(out_file.end(), crc_buf, crc_buf + 4);

            pos += 8 + chunk_data_len;
        }
    }

    return write_file(out_path, out_file);
}