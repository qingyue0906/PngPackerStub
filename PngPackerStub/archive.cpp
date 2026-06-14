#include "archive.h"
#include "fast_lzma2/fast-lzma2.h"
#include <fstream>
#include <cstring>
#include <cstdio>
#include <windows.h>

// ==========================================
// 工具函数
// ==========================================
static void write_u32_le(uint8_t* p, uint32_t v)
{
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
    p[2] = (v >> 16) & 0xFF;
    p[3] = (v >> 24) & 0xFF;
}

static uint32_t read_u32_le(const uint8_t* p)
{
    return (uint32_t)p[0] |
        ((uint32_t)p[1] << 8) |
        ((uint32_t)p[2] << 16) |
        ((uint32_t)p[3] << 24);
}

static void write_u64_le(uint8_t* p, uint64_t v)
{
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t)(v >> (i * 8));
}

static uint64_t read_u64_le(const uint8_t* p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v |= (uint64_t)p[i] << (i * 8);
    return v;
}

static bool append_to_file(const std::string& path, const std::vector<uint8_t>& data)
{
    std::ofstream f(path, std::ios::binary | std::ios::app);
    if (!f) return false;
    f.write((const char*)data.data(), data.size());
    return f.good();
}

static bool read_file_range(const std::string& path,
    uint64_t offset, uint64_t size,
    std::vector<uint8_t>& out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.seekg((std::streamoff)offset);
    out.resize((size_t)size);
    f.read((char*)out.data(), (std::streamsize)size);
    return (uint64_t)f.gcount() == size;
}

static void set_file_mtime(const std::string& path, uint64_t mtime)
{
    if (mtime == 0) return;
    // FILE_FLAG_BACKUP_SEMANTICS 是打开目录句柄的必要标志
    HANDLE h = CreateFileA(path.c_str(), FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    FILETIME ft;
    ft.dwHighDateTime = (DWORD)(mtime >> 32);
    ft.dwLowDateTime = (DWORD)(mtime & 0xFFFFFFFF);
    SetFileTime(h, NULL, NULL, &ft);
    CloseHandle(h);
}

// ==========================================
// 进度条
// ==========================================
static void print_progress(const char* label, uint64_t current, uint64_t total, int width = 40)
{
    if (total == 0) return;
    double pct = (double)current / (double)total;
    if (pct > 1.0) pct = 1.0;
    int filled = (current >= total) ? width : (int)(pct * width + 0.5);

    printf("\r%s [", label);
    for (int i = 0; i < width; i++)
        printf(i < filled ? "#" : "-");
    printf("] %5.1f%%", pct * 100.0);
    fflush(stdout);

    if (current >= total) printf("\n");
}

// ==========================================
// 序列化 / 反序列化
// ==========================================
std::vector<uint8_t> serialize_entries(const std::vector<ArchiveEntry>& entries)
{
    std::vector<uint8_t> buf;
    uint8_t tmp[8];

    // 写条目数
    write_u32_le(tmp, (uint32_t)entries.size());
    buf.insert(buf.end(), tmp, tmp + 4);

    for (auto& e : entries)
    {
        // 路径长度 + 路径
        write_u32_le(tmp, (uint32_t)e.rel_path.size());
        buf.insert(buf.end(), tmp, tmp + 4);
        buf.insert(buf.end(), e.rel_path.begin(), e.rel_path.end());

        // 类型（1字节）
        buf.push_back((uint8_t)e.orig_type);

        // 密钥（16字节）
        buf.insert(buf.end(), e.key, e.key + 16);

        // mtime（8字节）
        write_u64_le(tmp, e.mtime);
        buf.insert(buf.end(), tmp, tmp + 8);

        // 数据长度 + 数据
        write_u64_le(tmp, (uint64_t)e.data.size());
        buf.insert(buf.end(), tmp, tmp + 8);
        buf.insert(buf.end(), e.data.begin(), e.data.end());
    }

    return buf;
}

bool deserialize_entries(const std::vector<uint8_t>& buf,
    std::vector<ArchiveEntry>& entries)
{
    if (buf.size() < 4) return false;
    size_t pos = 0;

    uint32_t n = read_u32_le(buf.data() + pos); pos += 4;
    entries.resize(n);

    for (uint32_t i = 0; i < n; i++)
    {
        // 路径
        if (pos + 4 > buf.size()) return false;
        uint32_t path_len = read_u32_le(buf.data() + pos); pos += 4;
        if (pos + path_len > buf.size()) return false;
        entries[i].rel_path.assign((char*)buf.data() + pos, path_len); pos += path_len;

        // 类型
        if (pos + 1 > buf.size()) return false;
        entries[i].orig_type = (RpgmvType)buf[pos]; pos += 1;

        // 密钥
        if (pos + 16 > buf.size()) return false;
        memcpy(entries[i].key, buf.data() + pos, 16); pos += 16;

        // mtime
        if (pos + 8 > buf.size()) return false;
        entries[i].mtime = read_u64_le(buf.data() + pos); pos += 8;

        // 数据
        if (pos + 8 > buf.size()) return false;
        uint64_t data_len = read_u64_le(buf.data() + pos); pos += 8;
        if (pos + data_len > buf.size()) return false;
        entries[i].data.assign(buf.data() + pos, buf.data() + pos + data_len);
        pos += data_len;
    }

    return true;
}

// ==========================================
// Fast LZMA2 压缩 / 解压
// ==========================================
static bool fl2_compress(const std::vector<uint8_t>& src,
    std::vector<uint8_t>& dst,
    int dict_size_mb, int fast_bytes, int threads,
    const char* label)
{
    FL2_CStream* cs = FL2_createCStreamMt(threads, 0);
    if (!cs) {
        printf("[错误] 无法创建 Fast LZMA2 压缩流\n");
        return false;
    }

    FL2_CStream_setParameter(cs, FL2_p_compressionLevel, 10);
    FL2_CStream_setParameter(cs, FL2_p_dictionarySize, (size_t)dict_size_mb * 1024 * 1024);
    FL2_CStream_setParameter(cs, FL2_p_fastLength, fast_bytes);
    FL2_setCStreamTimeout(cs, 100);
    FL2_initCStream(cs, 0);

    size_t bound = FL2_compressBound(src.size());
    dst.resize(8 + bound);

    uint64_t src_size = src.size();
    for (int i = 0; i < 8; i++)
        dst[i] = (uint8_t)(src_size >> (i * 8));
    printf("  [调试] 压缩原始大小记录: %llu\n", src_size);

    FL2_inBuffer  in_buf = { src.data(), src.size(), 0 };
    FL2_outBuffer out_buf = { dst.data() + 8, bound, 0 };

    // 循环直到所有输入都被消费
    while (in_buf.pos < in_buf.size)
    {
        size_t ret = FL2_compressStream(cs, &out_buf, &in_buf);
        while (FL2_isTimedOut(ret)) {
            unsigned long long progress = FL2_getCStreamProgress(cs, nullptr);
            print_progress(label, progress, src.size());
            ret = FL2_compressStream(cs, &out_buf, &in_buf);
        }
        if (FL2_isError(ret)) {
            printf("[错误] Fast LZMA2 压缩失败: %s\n", FL2_getErrorName(ret));
            FL2_freeCStream(cs);
            return false;
        }
        unsigned long long progress = FL2_getCStreamProgress(cs, nullptr);
        print_progress(label, progress, src.size());
    }

    size_t ret = FL2_endStream(cs, &out_buf);
    while (ret == 1 || FL2_isTimedOut(ret)) {
        unsigned long long progress = FL2_getCStreamProgress(cs, nullptr);
        print_progress(label, progress, src.size());
        ret = FL2_endStream(cs, &out_buf);
    }
    if (FL2_isError(ret)) {
        printf("[错误] Fast LZMA2 结束流失败: %s\n", FL2_getErrorName(ret));
        FL2_freeCStream(cs);
        return false;
    }

    print_progress(label, src.size(), src.size());
    FL2_freeCStream(cs);
    printf("  [调试] 压缩完成，输出大小: %zu，out_buf.pos: %zu\n", 8 + out_buf.pos, out_buf.pos);
    dst.resize(8 + out_buf.pos);
    return true;
}

static bool fl2_decompress(const std::vector<uint8_t>& src,
    std::vector<uint8_t>& dst)
{
    printf("  [调试] 解压输入大小: %zu 字节\n", src.size());
    if (src.size() < 8) {
        printf("  [调试] 数据太小\n");
        return false;
    }

    uint64_t dst_size = read_u64_le(src.data());
    printf("  [调试] 期望解压后大小: %llu 字节\n", dst_size);
    dst.resize((size_t)dst_size);

    // 必须用流式接口才能保证完整解压
    FL2_DStream* ds = FL2_createDStream();
    if (!ds) {
        printf("[错误] 无法创建解压流\n");
        return false;
    }

    FL2_inBuffer  in_buf = { src.data() + 8, src.size() - 8, 0 };
    FL2_outBuffer out_buf = { dst.data(), dst.size(), 0 };

    size_t ret = 0;
    do {
        ret = FL2_decompressStream(ds, &out_buf, &in_buf);
        if (FL2_isTimedOut(ret)) continue;
        if (FL2_isError(ret)) break;
    } while (ret != 0);

    FL2_freeDStream(ds);

    printf("  [调试] 流式解压完成，out_buf.pos: %zu\n", out_buf.pos);

    if (FL2_isError(ret)) {
        printf("[错误] Fast LZMA2 解压失败: %s\n", FL2_getErrorName(ret));
        return false;
    }

    dst.resize(out_buf.pos);
    return true;
}

// ==========================================
// 压缩并追加到文件
// ==========================================
bool compress_entries(const std::vector<ArchiveEntry>& entries,
    const std::string& out_path,
    int dict_size_mb, int fast_bytes, int threads,
    const char* label,
    uint64_t& out_compressed_size)
{
    if (entries.empty()) {
        out_compressed_size = 0;
        return true;
    }

    printf("正在序列化 %zu 个条目...\n", entries.size());
    std::vector<uint8_t> serialized = serialize_entries(entries);
    printf("  [调试] 序列化后大小: %zu 字节\n", serialized.size());

    printf("正在压缩（%s）...\n", label);
    std::vector<uint8_t> compressed;
    if (!fl2_compress(serialized, compressed, dict_size_mb, fast_bytes, threads, label))
        return false;

    if (!append_to_file(out_path, compressed)) {
        printf("[错误] 写入文件失败: %s\n", out_path.c_str());
        return false;
    }

    out_compressed_size = compressed.size();
    return true;
}

// ==========================================
// 从文件读取并解压
// ==========================================
bool decompress_entries(const std::string& path,
    uint64_t offset,
    uint64_t compressed_size,
    std::vector<ArchiveEntry>& entries)
{
    if (compressed_size == 0) {
        entries.clear();
        return true;
    }

    std::vector<uint8_t> compressed;
    if (!read_file_range(path, offset, compressed_size, compressed)) {
        printf("[错误] 读取文件失败: %s\n", path.c_str());
        return false;
    }

    std::vector<uint8_t> serialized;
    if (!fl2_decompress(compressed, serialized))
        return false;

    return deserialize_entries(serialized, entries);
}

// ==========================================
// 档案尾部
// ==========================================
bool write_archive_tail(const std::string& path,
    uint64_t part1_size,
    uint64_t part2_size)
{
    std::ofstream f(path, std::ios::binary | std::ios::app);
    if (!f) return false;

    uint8_t buf[20];
    write_u64_le(buf, part1_size);
    write_u64_le(buf + 8, part2_size);
    memcpy(buf + 16, ARCHIVE_MAGIC, 4);

    f.write((const char*)buf, 20);
    return f.good();
}

bool read_archive_tail(const std::string& path,
    uint64_t& part1_size,
    uint64_t& part2_size)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;

    auto file_size = f.tellg();
    if (file_size < 20) return false;

    f.seekg(-20, std::ios::end);
    uint8_t buf[20];
    f.read((char*)buf, 20);

    if (memcmp(buf + 16, ARCHIVE_MAGIC, 4) != 0) {
        printf("[错误] 魔数验证失败，不是有效的档案文件\n");
        return false;
    }

    part1_size = read_u64_le(buf);
    part2_size = read_u64_le(buf + 8);
    return true;
}