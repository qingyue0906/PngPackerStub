#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <filesystem>
#include <windows.h>
#include "fast_lzma2/fast-lzma2.h"
#include "png_split.h"

namespace fs = std::filesystem;

// ==========================================
// 工具函数
// ==========================================
static uint32_t read_u32_be(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) |
        ((uint32_t)p[1] << 16) |
        ((uint32_t)p[2] << 8) |
        ((uint32_t)p[3]);
}

static void set_file_mtime(const std::string& path, uint64_t mtime)
{
    if (mtime == 0) return;
    HANDLE h = CreateFileA(path.c_str(), FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ, NULL, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, NULL);
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
// LZMA2 解压
// ==========================================
static bool lzma2_decompress(const uint8_t* src, size_t src_size,
    std::vector<uint8_t>& dst)
{
    if (src_size < 8) return false;

    uint64_t dst_size = 0;
    for (int i = 0; i < 8; i++)
        dst_size |= (uint64_t)src[i] << (i * 8);

    dst.resize((size_t)dst_size);

    size_t ret = FL2_decompress(
        dst.data(), dst.size(),
        src + 8, src_size - 8);

    if (FL2_isError(ret)) {
        printf("[错误] Fast LZMA2 解压失败: %s\n", FL2_getErrorName(ret));
        return false;
    }

    dst.resize(ret);
    return true;
}

// ==========================================
// 档案格式解析（和 packer.cpp 完全一致）
// ==========================================
struct FileEntry
{
    std::string          rel_path;
    std::vector<uint8_t> data;
    uint64_t             mtime = 0;
};

static bool parse_archive_buffer(const std::vector<uint8_t>& buf,
    std::vector<FileEntry>& entries)
{
    if (buf.size() < 4) return false;
    size_t pos = 0;

    uint32_t n = read_u32_be(buf.data() + pos); pos += 4;
    entries.resize(n);

    std::vector<uint64_t> data_lens(n);
    for (uint32_t i = 0; i < n; i++)
    {
        if (pos + 4 > buf.size()) return false;
        uint32_t name_len = read_u32_be(buf.data() + pos); pos += 4;

        if (pos + name_len > buf.size()) return false;
        entries[i].rel_path.assign((char*)buf.data() + pos, name_len); pos += name_len;

        if (pos + 8 > buf.size()) return false;
        uint64_t hi = read_u32_be(buf.data() + pos);
        uint64_t lo = read_u32_be(buf.data() + pos + 4);
        data_lens[i] = (hi << 32) | lo;
        pos += 8;

        if (pos + 8 > buf.size()) return false;
        uint64_t mhi = read_u32_be(buf.data() + pos);
        uint64_t mlo = read_u32_be(buf.data() + pos + 4);
        entries[i].mtime = (mhi << 32) | mlo;
        pos += 8;
    }

    for (uint32_t i = 0; i < n; i++)
    {
        uint64_t len = data_lens[i];
        if (pos + len > buf.size()) return false;
        entries[i].data.assign(buf.data() + pos, buf.data() + pos + len);
        pos += len;
    }

    return true;
}

static bool deserialize_parts(const std::vector<uint8_t>& buf, PngParts& out)
{
    if (buf.size() < 4) return false;
    uint32_t meta_len = read_u32_be(buf.data());
    if (buf.size() < 4 + meta_len) return false;
    out.meta.assign(buf.data() + 4, buf.data() + 4 + meta_len);
    out.pixels.assign(buf.data() + 4 + meta_len, buf.data() + buf.size());
    return true;
}

// ==========================================
// 从自身读取附加数据
// ==========================================
// 自解压文件末尾格式：
// [压缩数据] [8字节小端：压缩数据长度] [4字节魔数：0x50474B53 "PGKS"]
static bool read_payload(std::vector<uint8_t>& compressed)
{
    // 获取自身路径
    char self_path[MAX_PATH];
    GetModuleFileNameA(NULL, self_path, MAX_PATH);

    FILE* f = fopen(self_path, "rb");
    if (!f) {
        printf("[错误] 无法打开自身文件\n");
        return false;
    }

    // 读取末尾 12 字节（8字节长度 + 4字节魔数）
    fseek(f, -12, SEEK_END);
    uint8_t tail[12];
    fread(tail, 1, 12, f);

    // 验证魔数
    const uint8_t MAGIC[4] = { 0x50, 0x47, 0x4B, 0x53 }; // "PGKS"
    if (memcmp(tail + 8, MAGIC, 4) != 0) {
        printf("[错误] 魔数验证失败，这不是有效的自解压文件\n");
        fclose(f);
        return false;
    }

    // 读取压缩数据长度
    uint64_t data_size = 0;
    for (int i = 0; i < 8; i++)
        data_size |= (uint64_t)tail[i] << (i * 8);

    // 定位到压缩数据起始位置
    fseek(f, -(long)(data_size + 12), SEEK_END);
    compressed.resize((size_t)data_size);
    fread(compressed.data(), 1, (size_t)data_size, f);
    fclose(f);

    return true;
}

// ==========================================
// 主逻辑
// ==========================================
int main()
{
    printf("PngPacker 自解压程序\n");
    printf("====================\n");

    // 读取附加在自身末尾的压缩数据
    std::vector<uint8_t> compressed;
    if (!read_payload(compressed)) return 1;

    printf("正在 LZMA2 解压...\n");
    std::vector<uint8_t> archive_buf;
    if (!lzma2_decompress(compressed.data(), compressed.size(), archive_buf))
        return 1;

    std::vector<FileEntry> entries;
    if (!parse_archive_buffer(archive_buf, entries)) {
        printf("[错误] 档案格式损坏\n");
        return 1;
    }

    // 解压到当前目录下的 output 文件夹
    std::string out_folder = "output";
    printf("共 %zu 个文件，解压到 .\\output\\\n", entries.size());

    size_t total = entries.size();
    size_t done = 0;

    for (auto& entry : entries)
    {
        std::string rel = entry.rel_path;
        if (rel.size() > 4 && rel.substr(rel.size() - 4) == ".bin")
            rel = rel.substr(0, rel.size() - 4);

        fs::path out_path = fs::path(out_folder) / fs::path(rel);
        fs::create_directories(out_path.parent_path());

        PngParts parts;
        if (!deserialize_parts(entry.data, parts)) {
            printf("\n[警告] 数据损坏，跳过: %s\n", rel.c_str());
            done++;
            print_progress("还原", done, total);
            continue;
        }

        if (!restore_png(parts.meta, parts.pixels, out_path.string())) {
            printf("\n[警告] 还原失败，跳过: %s\n", rel.c_str());
            done++;
            print_progress("还原", done, total);
            continue;
        }

        set_file_mtime(out_path.string(), entry.mtime);
        done++;
        print_progress("还原", done, total);
    }

    printf("\n[成功] 所有图片已解压到 .\\output\\\n");
    printf("按任意键退出...\n");
    getchar();
    return 0;
}