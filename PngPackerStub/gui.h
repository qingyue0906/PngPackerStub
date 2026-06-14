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
std::string show_extract_dialog(const std::string& default_path = "");

// 运行解压进度窗口（类似 NanaZip 图2）
void run_extract_gui(
    const std::string& out_folder,
    size_t total_files,
    std::function<void(ProgressCallback, CancelCallback)> worker);