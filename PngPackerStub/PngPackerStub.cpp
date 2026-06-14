#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <filesystem>
#include <thread>
#include <mutex>
#include <atomic>
#include <windows.h>
#include <fstream>
#include "fast_lzma2/fast-lzma2.h"
#include "archive.h"
#include "rpgmv.h"
#include "png_split.h"


namespace fs = std::filesystem;

// ==========================================
// 工具函数
// ==========================================
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

static bool write_file(const std::string& path, const std::vector<uint8_t>& data)
{
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write((const char*)data.data(), data.size());
    return f.good();
}

static void set_file_mtime(const std::string& path, uint64_t mtime)
{
    if (mtime == 0) return;
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

static uint32_t read_u32_be(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
        ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static bool deserialize_png_parts(const std::vector<uint8_t>& buf, PngParts& out)
{
    if (buf.size() < 4) return false;
    uint32_t meta_len = read_u32_be(buf.data());
    if (buf.size() < 4 + meta_len) return false;
    out.meta.assign(buf.data() + 4, buf.data() + 4 + meta_len);
    out.pixels.assign(buf.data() + 4 + meta_len, buf.data() + buf.size());
    return true;
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
// 从自身读取附加的档案数据
// ==========================================
static bool read_payload(std::vector<uint8_t>& payload)
{
    char self_path[MAX_PATH];
    GetModuleFileNameA(NULL, self_path, MAX_PATH);

    FILE* f = fopen(self_path, "rb");
    if (!f) {
        printf("[错误] 无法打开自身文件\n");
        return false;
    }

    // 读取末尾 12 字节（8字节大小 + 4字节魔数 "PGKS"）
    fseek(f, -12, SEEK_END);
    uint8_t tail[12];
    fread(tail, 1, 12, f);

    const uint8_t SFX_MAGIC[4] = { 0x50, 0x47, 0x4B, 0x53 };
    if (memcmp(tail + 8, SFX_MAGIC, 4) != 0) {
        printf("[错误] 魔数验证失败，不是有效的自解压文件\n");
        fclose(f);
        return false;
    }

    uint64_t data_size = 0;
    for (int i = 0; i < 8; i++)
        data_size |= (uint64_t)tail[i] << (i * 8);

    fseek(f, -(long)(data_size + 12), SEEK_END);
    payload.resize((size_t)data_size);
    fread(payload.data(), 1, (size_t)data_size, f);
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

    // 读取附加在自身末尾的档案数据
    std::vector<uint8_t> payload;
    if (!read_payload(payload)) return 1;

    // 把 payload 写到临时文件，供 decompress_entries 读取
    // （decompress_entries 需要从文件读取，不支持直接从内存读）
    char tmp_path_buf[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp_path_buf);
    std::string tmp_archive = std::string(tmp_path_buf) + "pgksfx_tmp.bin";

    {
        std::ofstream tf(tmp_archive, std::ios::binary);
        tf.write((const char*)payload.data(), payload.size());
    }

    // 读取档案尾部，获取两部分大小
    uint64_t part1_size, part2_size;
    if (!read_archive_tail(tmp_archive, part1_size, part2_size)) {
        printf("[错误] 档案格式无效\n");
        fs::remove(tmp_archive);
        return 1;
    }

    // 解压两部分
    std::vector<ArchiveEntry> part1_entries, part2_entries;

    if (part1_size > 0)
    {
        printf("正在解压图片部分...\n");
        if (!decompress_entries(tmp_archive, 0, part1_size, part1_entries)) {
            printf("[错误] 图片部分解压失败\n");
            fs::remove(tmp_archive);
            return 1;
        }
    }

    if (part2_size > 0)
    {
        printf("正在解压文件部分...\n");
        if (!decompress_entries(tmp_archive, part1_size, part2_size, part2_entries)) {
            printf("[错误] 文件部分解压失败\n");
            fs::remove(tmp_archive);
            return 1;
        }
    }

    fs::remove(tmp_archive);

    // 解压到当前目录下的 output 文件夹
    std::string out_folder = "output";
    size_t total = part1_entries.size() + part2_entries.size();
    printf("共 %zu 个文件，解压到 .\\output\\\n", total);

    std::atomic<size_t> done(0);
    std::mutex print_mutex;
    std::vector<std::pair<std::string, uint64_t>> dir_mtimes;
    std::mutex dir_mutex;

    // 还原第一部分（多线程）
    {
        std::atomic<size_t> idx(0);
        int thread_count = std::thread::hardware_concurrency();
        if (thread_count < 1) thread_count = 4;
        std::vector<std::thread> threads_vec;

        auto worker = [&]()
            {
                while (true)
                {
                    size_t i = idx.fetch_add(1);
                    if (i >= part1_entries.size()) break;

                    auto& ae = part1_entries[i];
                    fs::path out_path = fs::path(out_folder) / fs::path(ae.rel_path);

                    {
                        std::lock_guard<std::mutex> lk(print_mutex);
                        fs::create_directories(out_path.parent_path());
                    }

                    PngParts parts;
                    if (!deserialize_png_parts(ae.data, parts))
                    {
                        std::lock_guard<std::mutex> lk(print_mutex);
                        printf("\n[警告] PNG 数据损坏，跳过: %s\n", ae.rel_path.c_str());
                        size_t d = done.fetch_add(1) + 1;
                        print_progress("还原", d, total);
                        continue;
                    }

                    if (ae.orig_type == RpgmvType::PNG)
                    {
                        if (!restore_png(parts.meta, parts.pixels, out_path.string()))
                        {
                            std::lock_guard<std::mutex> lk(print_mutex);
                            printf("\n[警告] PNG 还原失败: %s\n", ae.rel_path.c_str());
                        }
                    }
                    else
                    {
                        fs::path tmp_png = out_path.string() + ".tmp.png";

                        if (!restore_png(parts.meta, parts.pixels, tmp_png.string()))
                        {
                            std::lock_guard<std::mutex> lk(print_mutex);
                            printf("\n[警告] PNG 还原失败: %s\n", ae.rel_path.c_str());
                            done.fetch_add(1);
                            continue;
                        }

                        std::vector<uint8_t> png_data;
                        if (!read_file(tmp_png.string(), png_data))
                        {
                            fs::remove(tmp_png);
                            done.fetch_add(1);
                            continue;
                        }
                        fs::remove(tmp_png);

                        if (!encrypt_rpgmv(png_data, ae.key, out_path.string()))
                        {
                            std::lock_guard<std::mutex> lk(print_mutex);
                            printf("\n[警告] RPGMV 加密失败: %s\n", ae.rel_path.c_str());
                        }
                    }

                    set_file_mtime(out_path.string(), ae.mtime);

                    size_t d = done.fetch_add(1) + 1;
                    {
                        std::lock_guard<std::mutex> lk(print_mutex);
                        print_progress("还原", d, total);
                    }
                }
            };

        for (int t = 0; t < thread_count; t++)
            threads_vec.emplace_back(worker);
        for (auto& t : threads_vec)
            t.join();
    }

    // 还原第二部分
    for (auto& ae : part2_entries)
    {
        if (ae.orig_type == RpgmvType::DIRECTORY)
        {
            fs::path out_path = fs::path(out_folder) / fs::path(ae.rel_path);
            fs::create_directories(out_path);
            std::lock_guard<std::mutex> lk(dir_mutex);
            dir_mtimes.push_back({ out_path.string(), ae.mtime });
        }
        else
        {
            fs::path out_path = fs::path(out_folder) / fs::path(ae.rel_path);
            fs::create_directories(out_path.parent_path());

            if (!write_file(out_path.string(), ae.data))
                printf("\n[警告] 写入失败: %s\n", ae.rel_path.c_str());
            else
                set_file_mtime(out_path.string(), ae.mtime);
        }

        size_t d = done.fetch_add(1) + 1;
        print_progress("还原", d, total);
    }

    // 最后统一设置文件夹时间戳（从深到浅）
    std::sort(dir_mtimes.begin(), dir_mtimes.end(),
        [](const auto& a, const auto& b) {
            return a.first.size() > b.first.size();
        });
    for (auto& [path, mtime] : dir_mtimes)
        set_file_mtime(path, mtime);

    printf("\n[成功] 所有文件已解压到 .\\output\\\n");
    printf("按任意键退出...\n");
    getchar();
    return 0;
}