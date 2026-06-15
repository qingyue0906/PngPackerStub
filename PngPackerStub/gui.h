#pragma once
#include <string>
#include <functional>
#include <windows.h>

using ProgressCallback = std::function<void(const std::string& filename,
    size_t done,
    size_t total)>;
using CancelCallback = std::function<bool()>;

// 显示自定义的目录选择对话框（类似 NanaZip 图1）
// 返回空字符串表示用户取消
// extract_to_subfolder: 输出参数，返回用户是否勾选了"解压到子文件夹"
std::string show_extract_dialog(const std::string& exe_name,
    bool& extract_to_subfolder);

// 运行解压进度窗口（类似 NanaZip 图2）
void run_extract_gui(
    const std::string& out_folder,
    size_t total_files,
    std::function<void(ProgressCallback, CancelCallback)> worker);

#define WM_SET_TOTAL (WM_USER + 4)
extern HWND g_prog_hwnd;
