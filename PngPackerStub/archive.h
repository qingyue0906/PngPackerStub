#pragma once
#include <vector>
#include <string>
#include <cstdint>
#include "rpgmv.h"

// ==========================================
// 档案中每个文件的元数据
// ==========================================
struct ArchiveEntry
{
    std::string          rel_path;                      // 相对路径（统一用正斜杠）
    RpgmvType            orig_type = RpgmvType::OTHER;  // 原始文件类型
    uint8_t              key[16] = {};                  // 解密密钥（仅 RPGMVP/PNG_ 有效）
    uint64_t             mtime = 0;                     // 文件修改时间（Windows FILETIME）
    std::vector<uint8_t> data;                          // 文件数据
    // PNG/RPGMVP/PNG_: [4字节meta长度]+meta+像素
    // OTHER: 原始文件数据
};

// ==========================================
// 档案格式：
//
// [第一部分 Fast LZMA2 压缩块]  <- PNG/RPGMVP/PNG_ 文件
// [第二部分 Fast LZMA2 压缩块]  <- OTHER 文件
// [8字节小端: 第一部分大小]
// [8字节小端: 第二部分大小]
// [4字节魔数: 'RPGK']
// ==========================================
static const uint8_t ARCHIVE_MAGIC[4] = { 0x52, 0x50, 0x47, 0x4B }; // "RPGK"

// 序列化一组 ArchiveEntry 为字节流（压缩前）
// 格式：[4字节条目数] 重复N次：[4字节路径长] [路径] [1字节类型] [16字节key] [8字节mtime] [8字节数据长] [数据]
std::vector<uint8_t> serialize_entries(const std::vector<ArchiveEntry>& entries);

// 从字节流反序列化为 ArchiveEntry 列表
bool deserialize_entries(const std::vector<uint8_t>& buf,
    std::vector<ArchiveEntry>& entries);

// 压缩一组 entries，写入 out_path
// 会追加到文件末尾（用于拼接两部分）
bool compress_entries(const std::vector<ArchiveEntry>& entries,
    const std::string& out_path,
    int dict_size_mb, int fast_bytes, int threads,
    const char* label,
    uint64_t& out_compressed_size);

// 从文件的指定偏移读取并解压一组 entries
bool decompress_entries(const std::string& path,
    uint64_t offset,
    uint64_t compressed_size,
    std::vector<ArchiveEntry>& entries);

// 写入档案尾部（两个部分的大小 + 魔数）
bool write_archive_tail(const std::string& path,
    uint64_t part1_size,
    uint64_t part2_size);

// 读取档案尾部，获取两个部分的大小
bool read_archive_tail(const std::string& path,
    uint64_t& part1_size,
    uint64_t& part2_size);