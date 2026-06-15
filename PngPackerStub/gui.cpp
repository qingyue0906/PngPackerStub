#include "gui.h"
#include <windows.h>
#include <shlobj.h>
#include <commctrl.h>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>

#include <shlwapi.h>
#pragma comment(lib, "shlwapi.lib")

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")

// ==========================================
// 控件 ID
// ==========================================
#define IDC_EDIT_PATH     101
#define IDC_BTN_BROWSE    102
#define IDC_BTN_OK        103
#define IDC_BTN_CANCEL    104
#define IDC_PROGRESS      105
#define IDC_LBL_ELAPSED   106
#define IDC_LBL_REMAINING 107
#define IDC_LBL_SPEED     108
#define IDC_LBL_FILES     109
#define IDC_LBL_TOTAL     110
#define IDC_LBL_PROCESSED 111
#define IDC_BTN_BACKGROUND 112
#define IDC_BTN_PAUSE     113
#define IDC_BTN_CANCEL2   114

#define WM_PROGRESS_UPDATE (WM_USER + 1)
#define WM_EXTRACT_DONE    (WM_USER + 2)
#define WM_EXTRACT_CANCEL  (WM_USER + 3)

HWND g_prog_hwnd = nullptr;

// 递归给窗口及所有子控件设置字体
static void set_font_recursive(HWND hwnd, HFONT font)
{
    SendMessage(hwnd, WM_SETFONT, (WPARAM)font, TRUE);
    EnumChildWindows(hwnd, [](HWND child, LPARAM lp) -> BOOL {
        SendMessage(child, WM_SETFONT, (WPARAM)lp, TRUE);
        return TRUE;
        }, (LPARAM)font);
}

// 创建 Segoe UI 9pt 字体
static HFONT create_ui_font()
{
    return CreateFontA(
        -MulDiv(9, GetDeviceCaps(GetDC(nullptr), LOGPIXELSY), 72),
        0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_SWISS, "Segoe UI");
}

// ==========================================
// 工具：创建标签
// ==========================================
static HWND make_label(HWND parent, const char* text, int x, int y, int w, int h,
    UINT style = SS_LEFT, int id = 0)
{
    return CreateWindowA("STATIC", text,
        WS_CHILD | WS_VISIBLE | style,
        x, y, w, h, parent, (HMENU)(intptr_t)id, nullptr, nullptr);
}

static HWND make_label_right(HWND parent, const char* text, int x, int y, int w, int h, int id = 0)
{
    return CreateWindowA("STATIC", text,
        WS_CHILD | WS_VISIBLE | SS_RIGHT,
        x, y, w, h, parent, (HMENU)(intptr_t)id, nullptr, nullptr);
}

// ==========================================
// 目录选择对话框
// ==========================================
struct ExtractDlgData
{
    std::string result;
    std::string subfolder_name; // exe 文件名（不含扩展名）
    bool        ok = false;
    bool        extract_to_subfolder = true;
};

static LRESULT CALLBACK ExtractDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    ExtractDlgData* data = (ExtractDlgData*)GetWindowLongPtrA(hwnd, GWLP_USERDATA);

    switch (msg)
    {
    case WM_CREATE:
    {
        CREATESTRUCTA* cs = (CREATESTRUCTA*)lp;
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        data = (ExtractDlgData*)cs->lpCreateParams;

        // 标签
        make_label(hwnd, "提取到(X):", 12, 14, 80, 18);

        // 路径输入框
        char exe_dir[MAX_PATH];
        GetModuleFileNameA(nullptr, exe_dir, MAX_PATH);
        PathRemoveFileSpecA(exe_dir);
        CreateWindowA("EDIT", exe_dir,
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            12, 34, 360, 24, hwnd, (HMENU)IDC_EDIT_PATH, nullptr, nullptr);

        // 浏览按钮
        CreateWindowA("BUTTON", "...",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            378, 34, 30, 24, hwnd, (HMENU)IDC_BTN_BROWSE, nullptr, nullptr);

        // 勾选框：解压到子文件夹
        std::string chk_label = "解压到子文件夹 \"" + data->subfolder_name + "\"";
        HWND hwnd_chk = CreateWindowA("BUTTON", chk_label.c_str(),
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            12, 68, 400, 20, hwnd, (HMENU)105, nullptr, nullptr);
        SendMessage(hwnd_chk, BM_SETCHECK, BST_CHECKED, 0); // 默认勾选

        // 确定/取消
        CreateWindowA("BUTTON", "确定",
            WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            230, 96, 88, 28, hwnd, (HMENU)IDC_BTN_OK, nullptr, nullptr);
        CreateWindowA("BUTTON", "取消",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            326, 96, 88, 28, hwnd, (HMENU)IDC_BTN_CANCEL, nullptr, nullptr);

        set_font_recursive(hwnd, create_ui_font());
        return 0;
    }

    case WM_COMMAND:
    {
        int id = LOWORD(wp);
        if (id == IDC_BTN_BROWSE)
        {
            // 用系统文件夹选择对话框
            IFileDialog* pfd = nullptr;
            if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pfd))))
            {
                DWORD opts;
                pfd->GetOptions(&opts);
                pfd->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
                pfd->SetTitle(L"选择解压目标文件夹");
                if (SUCCEEDED(pfd->Show(hwnd)))
                {
                    IShellItem* psi = nullptr;
                    if (SUCCEEDED(pfd->GetResult(&psi)))
                    {
                        PWSTR pszPath = nullptr;
                        if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &pszPath)))
                        {
                            int len = WideCharToMultiByte(CP_ACP, 0, pszPath, -1,
                                nullptr, 0, nullptr, nullptr);
                            std::string path(len - 1, '\0');
                            WideCharToMultiByte(CP_ACP, 0, pszPath, -1,
                                path.data(), len, nullptr, nullptr);
                            SetWindowTextA(GetDlgItem(hwnd, IDC_EDIT_PATH), path.c_str());
                            CoTaskMemFree(pszPath);
                        }
                        psi->Release();
                    }
                }
                pfd->Release();
            }
        }
        else if (id == IDC_BTN_OK)
        {
            char buf[MAX_PATH];
            GetWindowTextA(GetDlgItem(hwnd, IDC_EDIT_PATH), buf, MAX_PATH);
            data->result = buf;
            data->extract_to_subfolder =
                (SendMessage(GetDlgItem(hwnd, 105), BM_GETCHECK, 0, 0) == BST_CHECKED);
            data->ok = true;
            DestroyWindow(hwnd);
        }
        else if (id == IDC_BTN_CANCEL)
        {
            data->ok = false;
            DestroyWindow(hwnd);
        }
        return 0;
    }

    case WM_CLOSE:
        data->ok = false;
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

std::string show_extract_dialog(const std::string& exe_name,
    bool& extract_to_subfolder)
{
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    WNDCLASSA wc = {};
    wc.lpfnWndProc = ExtractDlgProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "PngPackerExtractDlg";
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassA(&wc);

    ExtractDlgData data;
    data.subfolder_name = exe_name;
    HWND hwnd = CreateWindowExA(
        WS_EX_DLGMODALFRAME,
        "PngPackerExtractDlg", "提取",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, 440, 170,
        nullptr, nullptr, wc.hInstance, &data);

    // 居中
    int W = 440, H = 170;
    int x = (GetSystemMetrics(SM_CXSCREEN) - W) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - H) / 2;
    SetWindowPos(hwnd, nullptr, x, y, W, H, SWP_NOZORDER);

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    extract_to_subfolder = data.extract_to_subfolder;
    return data.ok ? data.result : "";
}

// ==========================================
// 解压进度窗口
// ==========================================
struct ProgressData
{
    HWND hwnd = nullptr;
    HWND hwnd_prog = nullptr;
    HWND hwnd_elapsed = nullptr;
    HWND hwnd_remaining = nullptr;
    HWND hwnd_speed = nullptr;
    HWND hwnd_files = nullptr;
    HWND hwnd_total = nullptr;
    HWND hwnd_processed = nullptr;
    HWND hwnd_pause = nullptr;

    size_t              total = 0;
    std::atomic<bool>   canceled{ false };
    std::atomic<bool>   paused{ false };
    std::atomic<bool>   done{ false };
    std::atomic<size_t> cur_done{ 0 };
    std::atomic<size_t> cur_bytes{ 0 };

    std::mutex          msg_mutex;
    std::string         cur_filename;

    std::chrono::steady_clock::time_point start_time;
    size_t              total_bytes = 0;
};

static ProgressData* g_prog = nullptr;

static void format_time(char* buf, size_t bufsz, long long seconds)
{
    long long h = seconds / 3600;
    long long m = (seconds % 3600) / 60;
    long long s = seconds % 60;
    snprintf(buf, bufsz, "%02lld:%02lld:%02lld", h, m, s);
}

static void format_size(char* buf, size_t bufsz, size_t bytes)
{
    if (bytes >= 1024 * 1024 * 1024)
        snprintf(buf, bufsz, "%.1f GB", bytes / (1024.0 * 1024 * 1024));
    else if (bytes >= 1024 * 1024)
        snprintf(buf, bufsz, "%.1f MB", bytes / (1024.0 * 1024));
    else if (bytes >= 1024)
        snprintf(buf, bufsz, "%.1f KB", bytes / 1024.0);
    else
        snprintf(buf, bufsz, "%zu B", bytes);
}

static LRESULT CALLBACK ProgressWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_CREATE:
    {
        INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_PROGRESS_CLASS };
        InitCommonControlsEx(&icc);

        // 左列标签（固定文字）
        make_label(hwnd, "已用时间:", 12, 16, 90, 18);
        make_label(hwnd, "剩余时间:", 12, 38, 90, 18);
        make_label(hwnd, "文件:", 12, 60, 90, 18);

        // 左列数值
        g_prog->hwnd_elapsed = make_label(hwnd, "00:00:00", 108, 16, 120, 18, SS_LEFT, IDC_LBL_ELAPSED);
        g_prog->hwnd_remaining = make_label(hwnd, "00:00:00", 108, 38, 120, 18, SS_LEFT, IDC_LBL_REMAINING);
        g_prog->hwnd_files = make_label(hwnd, "0", 108, 60, 120, 18, SS_LEFT, IDC_LBL_FILES);

        // 右列标签
        make_label(hwnd, "总大小:", 320, 16, 80, 18);
        make_label(hwnd, "速度:", 320, 38, 80, 18);
        make_label(hwnd, "已处理:", 320, 60, 80, 18);

        // 右列数值
        g_prog->hwnd_total = make_label(hwnd, "-", 406, 16, 120, 18, SS_LEFT, IDC_LBL_TOTAL);
        g_prog->hwnd_speed = make_label(hwnd, "-", 406, 38, 120, 18, SS_LEFT, IDC_LBL_SPEED);
        g_prog->hwnd_processed = make_label(hwnd, "-", 406, 60, 120, 18, SS_LEFT, IDC_LBL_PROCESSED);

        // 进度条
        g_prog->hwnd_prog = CreateWindowA(PROGRESS_CLASSA, nullptr,
            WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
            12, 90, 520, 28, hwnd, (HMENU)IDC_PROGRESS, nullptr, nullptr);
        SendMessage(g_prog->hwnd_prog, PBM_SETRANGE32, 0, (LPARAM)g_prog->total);

        // 底部按钮
        CreateWindowA("BUTTON", "后台(B)",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            130, 132, 88, 28, hwnd, (HMENU)IDC_BTN_BACKGROUND, nullptr, nullptr);
        g_prog->hwnd_pause = CreateWindowA("BUTTON", "暂停(P)",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            226, 132, 88, 28, hwnd, (HMENU)IDC_BTN_PAUSE, nullptr, nullptr);
        CreateWindowA("BUTTON", "取消",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            322, 132, 88, 28, hwnd, (HMENU)IDC_BTN_CANCEL2, nullptr, nullptr);

        // 启动定时器，每秒更新一次时间和速度
        SetTimer(hwnd, 1, 1000, nullptr);

        g_prog->start_time = std::chrono::steady_clock::now();

        set_font_recursive(hwnd, create_ui_font());
        return 0;
    }

    case WM_TIMER:
    {
        if (!g_prog) break;

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - g_prog->start_time).count();

        char buf[64];
        format_time(buf, sizeof(buf), elapsed);
        SetWindowTextA(g_prog->hwnd_elapsed, buf);

        size_t done = g_prog->cur_done.load();
        size_t total = g_prog->total;

        // 剩余时间估算
        if (done > 0 && elapsed > 0)
        {
            long long remaining = elapsed * (long long)(total - done) / (long long)done;
            format_time(buf, sizeof(buf), remaining);
            SetWindowTextA(g_prog->hwnd_remaining, buf);
        }

        // 速度（文件/秒）
        if (elapsed > 0)
        {
            snprintf(buf, sizeof(buf), "%.1f 文件/秒", (double)done / elapsed);
            SetWindowTextA(g_prog->hwnd_speed, buf);
        }

        return 0;
    }

    case WM_SET_TOTAL:
    {
        if (!g_prog) break;
        g_prog->total = (size_t)wp;
        SendMessage(g_prog->hwnd_prog, PBM_SETRANGE32, 0, (LPARAM)wp);
        return 0;
    }

    case WM_PROGRESS_UPDATE:
    {
        if (!g_prog) break;

        size_t done = g_prog->cur_done.load();
        size_t total = g_prog->total;

        // 更新文件计数
        char buf[128];
        snprintf(buf, sizeof(buf), "%zu", done);
        SetWindowTextA(g_prog->hwnd_files, buf);

        // 更新进度条
        SendMessage(g_prog->hwnd_prog, PBM_SETPOS, (WPARAM)done, 0);

        // 更新标题
        if (total > 0)
        {
            snprintf(buf, sizeof(buf), "%zu%% 正在提取", done * 100 / total);
            SetWindowTextA(hwnd, buf);
        }

        return 0;
    }

    case WM_EXTRACT_DONE:
        KillTimer(hwnd, 1);
        MessageBoxA(hwnd, "解压完成！", "完成", MB_OK | MB_ICONINFORMATION);
        DestroyWindow(hwnd);
        return 0;

    case WM_EXTRACT_CANCEL:
        KillTimer(hwnd, 1);
        MessageBoxA(hwnd, "解压已取消。", "取消", MB_OK | MB_ICONWARNING);
        DestroyWindow(hwnd);
        return 0;

    case WM_COMMAND:
    {
        int id = LOWORD(wp);
        if (id == IDC_BTN_CANCEL2)
        {
            g_prog->canceled = true;
            EnableWindow(GetDlgItem(hwnd, IDC_BTN_CANCEL2), FALSE);
        }
        else if (id == IDC_BTN_PAUSE)
        {
            bool now_paused = !g_prog->paused.load();
            g_prog->paused = now_paused;
            SetWindowTextA(g_prog->hwnd_pause, now_paused ? "继续(P)" : "暂停(P)");
        }
        else if (id == IDC_BTN_BACKGROUND)
        {
            ShowWindow(hwnd, SW_MINIMIZE);
        }
        return 0;
    }

    case WM_CLOSE:
        g_prog->canceled = true;
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, 1);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

void run_extract_gui(
    const std::string& out_folder,
    size_t total_files,
    std::function<void(ProgressCallback, CancelCallback)> worker)
{
    ProgressData prog;
    prog.total = total_files;
    g_prog = &prog;

    WNDCLASSA wc = {};
    wc.lpfnWndProc = ProgressWndProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "PngPackerProgressWnd";
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassA(&wc);

    int W = 560, H = 210;
    int x = (GetSystemMetrics(SM_CXSCREEN) - W) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - H) / 2;

    prog.hwnd = CreateWindowExA(
        0,
        "PngPackerProgressWnd", "0% 正在提取",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        x, y, W, H,
        nullptr, nullptr, wc.hInstance, nullptr);

    ShowWindow(prog.hwnd, SW_SHOW);
    UpdateWindow(prog.hwnd);
    g_prog_hwnd = prog.hwnd;

    // 工作线程
    std::thread work_thread([&]()
        {
            ProgressCallback progress_cb = [&](const std::string& filename,
                size_t done, size_t total)
                {
                    // 暂停时等待
                    while (prog.paused.load() && !prog.canceled.load())
                        Sleep(100);

                    prog.cur_done = done;
                    PostMessage(prog.hwnd, WM_PROGRESS_UPDATE, 0, 0);
                };

            CancelCallback cancel_cb = [&]() -> bool
                {
                    return prog.canceled.load();
                };

            worker(progress_cb, cancel_cb);

            if (prog.canceled)
                PostMessage(prog.hwnd, WM_EXTRACT_CANCEL, 0, 0);
            else
                PostMessage(prog.hwnd, WM_EXTRACT_DONE, 0, 0);
        });

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    work_thread.join();
    g_prog = nullptr;
}