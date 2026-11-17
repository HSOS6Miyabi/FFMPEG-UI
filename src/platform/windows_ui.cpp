#include "ui.hpp"

#include "../core/app_controller.hpp"
#include "../core/process.hpp"

#define _WIN32_WINNT 0x0A00
#define WINVER 0x0A00
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <dwmapi.h>
#include <uxtheme.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <filesystem>
#include <string>
#include <vector>

namespace ffmpegui {
namespace {

struct Rect { int x{}, y{}, width{}, height{}; };
bool contains(const Rect& r, int x, int y) {
    return x >= r.x && y >= r.y && x < r.x + r.width && y < r.y + r.height;
}

struct Palette {
    COLORREF background, surface, card, border, text, muted, accent, accentSoft;
    COLORREF green, red, input, track, white;
};

Palette palette(bool dark) {
    if (dark) return {RGB(15, 19, 29), RGB(19, 25, 37), RGB(25, 33, 48), RGB(44, 56, 76),
        RGB(238, 243, 250), RGB(153, 166, 187), RGB(126, 144, 255), RGB(43, 55, 91),
        RGB(47, 193, 151), RGB(246, 104, 117), RGB(20, 28, 42), RGB(48, 60, 80), RGB(255,255,255)};
    return {RGB(243, 246, 251), RGB(250, 251, 254), RGB(255, 255, 255), RGB(225, 231, 240),
        RGB(27, 37, 56), RGB(105, 119, 142), RGB(80, 101, 232), RGB(235, 239, 255),
        RGB(21, 155, 121), RGB(213, 67, 85), RGB(247, 249, 253), RGB(231, 236, 245), RGB(255,255,255)};
}

struct Layout {
    Rect addFiles, addFolder, addArchive, importQueue, exportQueue, removeSelected, clearDone;
    Rect outputField, outputBrowse;
    Rect container[2], codec[3], backend[4], crfTrack, crfValue;
    Rect compression[8], audio[6], resolution[6], audioMode[3];
    Rect parallel, recursive, skip, preserve, overwrite;
    Rect queue, overallCard, overallTrack, start, pause, cancel, ssh, theme;
};

std::vector<EncoderBackend> availableBackends(const EncoderCapabilities& capabilities, VideoCodec codec) {
    std::vector<EncoderBackend> result;
    for (const auto backend : {EncoderBackend::CPU, EncoderBackend::NVENC, EncoderBackend::QSV, EncoderBackend::AMF})
        if (capabilities.supports(codec, backend)) result.push_back(backend);
    return result;
}

Layout makeLayout(int width, int height, double scale, int backendCount = 4) {
    const auto s = [scale](int v) { return static_cast<int>(std::lround(v * scale)); };
    const int pad = s(28), gap = s(18), rightW = std::clamp(width / 3, s(370), s(450));
    const int leftW = width - pad * 2 - gap - rightW;
    const int leftX = pad, rightX = leftX + leftW + gap, top = s(112);
    Layout l{};
    const int toolbarGap = s(6);
    const int primaryWidth = std::min(s(440), (leftW - s(18)) * 52 / 100);
    const int primaryButtonWidth = std::max(s(72), (primaryWidth - toolbarGap * 2) / 3);
    l.addFiles = {leftX, top, primaryButtonWidth, s(42)};
    l.addFolder = {leftX + primaryButtonWidth + toolbarGap, top, primaryButtonWidth, s(42)};
    l.addArchive = {leftX + (primaryButtonWidth + toolbarGap) * 2, top, primaryButtonWidth, s(42)};
    const int secondaryWidth = leftW - primaryWidth - s(18);
    const int secondaryInner = std::max(s(4), secondaryWidth - toolbarGap * 3);
    const int secondaryBase = secondaryInner / 4;
    const int secondaryStart = leftX + leftW - secondaryWidth;
    l.importQueue = {secondaryStart, top, secondaryBase, s(42)};
    l.exportQueue = {secondaryStart + secondaryBase + toolbarGap, top, secondaryBase, s(42)};
    l.removeSelected = {secondaryStart + (secondaryBase + toolbarGap) * 2, top, secondaryBase, s(42)};
    l.clearDone = {secondaryStart + (secondaryBase + toolbarGap) * 3, top,
                   secondaryWidth - (secondaryBase + toolbarGap) * 3, s(42)};
    const int settingsY = top + s(56);
    l.outputField = {leftX + s(20), settingsY + s(70), leftW - s(144), s(38)};
    l.outputBrowse = {leftX + leftW - s(112), settingsY + s(70), s(92), s(38)};
    const int controlX = leftX + s(152);
    const int controlW = leftW - s(172);
    const int containerW = (controlW - s(6)) / 2;
    for (int index = 0; index < 2; ++index) {
        l.container[index] = {
            controlX + index * (containerW + s(6)),
            settingsY + s(112), containerW, s(32)
        };
    }
    const int codecW = (controlW - s(12)) / 3;
    for (int index = 0; index < 3; ++index) {
        l.codec[index] = {
            controlX + index * (codecW + s(6)),
            settingsY + s(153), codecW, s(32)
        };
    }
    backendCount = std::clamp(backendCount, 1, 4);
    const int backendW = (controlW - s(6) * (backendCount - 1)) / backendCount;
    for (int index = 0; index < backendCount; ++index) {
        l.backend[index] = {
            controlX + index * (backendW + s(6)),
            settingsY + s(194), backendW, s(32)
        };
    }
    l.crfTrack = {controlX, settingsY + s(245), controlW - s(92), s(8)};
    l.crfValue = {controlX + controlW - s(80), settingsY + s(229), s(80), s(32)};
    const int compW = (controlW - s(18)) / 4;
    for (int index = 0; index < 8; ++index) {
        const int row = index / 4;
        const int column = index % 4;
        l.compression[index] = {
            controlX + column * (compW + s(6)),
            settingsY + s(280) + row * s(35),
            compW, s(30)
        };
    }
    const int audioW = (controlW - s(30)) / 6;
    for (int index = 0; index < 6; ++index) {
        l.audio[index] = {
            controlX + index * (audioW + s(6)),
            settingsY + s(351), audioW, s(30)
        };
        l.resolution[index] = {
            controlX + index * (audioW + s(6)),
            settingsY + s(389), audioW, s(30)
        };
    }
    const int audioModeW = (controlW - s(12)) / 3;
    for (int index = 0; index < 3; ++index) {
        l.audioMode[index] = {
            controlX + index * (audioModeW + s(6)),
            settingsY + s(427), audioModeW, s(30)
        };
    }
    const int optionGap = s(6);
    const int optionWidth = (leftW - s(40) - optionGap * 4) / 5;
    const int optionY = settingsY + s(468);
    l.parallel = {leftX + s(20), optionY, optionWidth, s(32)};
    l.recursive = {leftX + s(20) + optionWidth + optionGap, optionY, optionWidth, s(32)};
    l.skip = {leftX + s(20) + (optionWidth + optionGap) * 2, optionY, optionWidth, s(32)};
    l.preserve = {leftX + s(20) + (optionWidth + optionGap) * 3, optionY, optionWidth, s(32)};
    l.overwrite = {leftX + s(20) + (optionWidth + optionGap) * 4, optionY, optionWidth, s(32)};
    l.queue = {leftX, settingsY + s(538), leftW, height - (settingsY + s(530)) - pad};
    l.overallCard = {rightX, top, rightW, s(270)};
    l.overallTrack = {rightX + s(22), top + s(126), rightW - s(44), s(12)};
    l.start = {rightX + s(22), top + s(206), rightW - s(44), s(44)};
    const int actionGap = s(8), actionWidth = (rightW - s(52) - actionGap) / 2;
    l.pause = {rightX + s(22), top + s(206), actionWidth, s(44)};
    l.cancel = {rightX + s(22) + actionWidth + actionGap, top + s(206),
                rightW - s(44) - actionWidth - actionGap, s(44)};
    l.ssh = {width - s(224), s(28), s(100), s(36)};
    l.theme = {width - s(112), s(28), s(82), s(36)};
    return l;
}

void fillRound(HDC dc, const Rect& r, COLORREF fill, COLORREF border, int radius = 10) {
    HBRUSH brush = CreateSolidBrush(fill); HPEN pen = CreatePen(PS_SOLID, 1, border);
    auto oldBrush = SelectObject(dc, brush); auto oldPen = SelectObject(dc, pen);
    RoundRect(dc, r.x, r.y, r.x + r.width, r.y + r.height, radius * 2, radius * 2);
    SelectObject(dc, oldBrush); SelectObject(dc, oldPen); DeleteObject(brush); DeleteObject(pen);
}

void drawText(HDC dc, const Rect& r, const std::wstring& value, COLORREF color, HFONT font,
              UINT flags = DT_LEFT | DT_VCENTER | DT_SINGLELINE) {
    SetTextColor(dc, color); SetBkMode(dc, TRANSPARENT);
    auto old = SelectObject(dc, font);
    RECT rect{r.x, r.y, r.x + r.width, r.y + r.height};
    DrawTextW(dc, value.c_str(), static_cast<int>(value.size()), &rect, flags | DT_END_ELLIPSIS);
    SelectObject(dc, old);
}

void drawButton(HDC dc, const Rect& r, const std::wstring& text, const Palette& c,
                HFONT font, bool active = false, bool disabled = false) {
    const COLORREF bg = disabled ? c.track : active ? c.accent : c.input;
    fillRound(dc, r, bg, active || disabled ? bg : c.border, 8);
    drawText(dc, r, text, active && !disabled ? c.white : c.text, font, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void drawCard(HDC dc, const Rect& r, const Palette& c) {
    fillRound(dc, {r.x, r.y + 2, r.width, r.height}, c.border, c.border, 12);
    fillRound(dc, r, c.card, c.border, 12);
}

std::wstring wide(const std::string& value) { return wideFromUtf8(value); }
std::string basename(const std::string& path) {
    const std::filesystem::path filePath(wide(path));
    return utf8FromWide(filePath.filename().c_str());
}

std::wstring formatTime(const std::optional<double>& value) {
    if (!value || *value < 0) return L"--:--:--";
    const auto total = static_cast<unsigned long long>(*value);
    wchar_t buffer[32]{};
    swprintf_s(buffer, L"%02llu:%02llu:%02llu", total / 3600, (total % 3600) / 60, total % 60);
    return buffer;
}
std::wstring formatSize(std::uint64_t bytes) {
    wchar_t buffer[48]{}; const double value = static_cast<double>(bytes);
    if (value >= 1024.0 * 1024.0 * 1024.0) swprintf_s(buffer, L"%.2f GiB", value / (1024.0*1024.0*1024.0));
    else if (value >= 1024.0 * 1024.0) swprintf_s(buffer, L"%.1f MiB", value / (1024.0*1024.0));
    else if (value >= 1024.0) swprintf_s(buffer, L"%.0f KiB", value / 1024.0);
    else swprintf_s(buffer, L"%llu B", static_cast<unsigned long long>(bytes));
    return buffer;
}
std::wstring formatTaskSize(std::uint64_t inputBytes, std::uint64_t outputBytes) {
    if (inputBytes == 0) return {};
    std::wstring result = L"输入 " + formatSize(inputBytes);
    if (outputBytes > 0) {
        result += L"  →  输出 " + formatSize(outputBytes);
        if (inputBytes > outputBytes) {
            const double saved = (1.0 - static_cast<double>(outputBytes) / static_cast<double>(inputBytes)) * 100.0;
            wchar_t value[32]{}; swprintf_s(value, L"  ·  节省 %.1f%%", saved); result += value;
        }
    }
    return result;
}

std::wstring formatProgressLine(const ConversionProgress& progress, const std::string& fallback) {
    if (!progress.frame && !progress.currentTimeSeconds && !progress.fps && !progress.speed &&
        !progress.bitrate && !progress.totalSizeBytes) return wide(fallback);
    std::wstring line;
    auto append = [&](const std::wstring& value) { if (!line.empty()) line += L"  ·  "; line += value; };
    if (progress.frame) append(L"帧 " + std::to_wstring(*progress.frame));
    if (progress.currentTimeSeconds || progress.durationSeconds)
        append(formatTime(progress.currentTimeSeconds) + L" / " + formatTime(progress.durationSeconds));
    if (progress.fps) { wchar_t value[32]{}; swprintf_s(value,L"%.1f FPS",*progress.fps); append(value); }
    if (progress.speed) { wchar_t value[32]{}; swprintf_s(value,L"%.2fx",*progress.speed); append(value); }
    if (progress.etaSeconds) append(L"剩余 " + formatTime(progress.etaSeconds));
    if (progress.bitrate) append(L"码率 " + wide(*progress.bitrate));
    if (progress.totalSizeBytes) append(L"已写入 " + formatSize(*progress.totalSizeBytes));
    return line;
}

class SshManagerDialog {
public:
    int run(HINSTANCE instance, HWND owner, AppController& controller) {
        instance_ = instance;
        controller_ = &controller;

        WNDCLASSW windowClass{};
        windowClass.lpfnWndProc = &SshManagerDialog::windowProc;
        windowClass.hInstance = instance;
        windowClass.lpszClassName = L"FFmpegStudioSshManager";
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        RegisterClassW(&windowClass);

        const UINT dpi = GetDpiForWindow(owner);
        scale_ = static_cast<double>(dpi == 0 ? 96 : dpi) / 96.0;

        const int width = scaled(1000);
        const int height = scaled(740);
        RECT ownerRectangle{};
        GetWindowRect(owner, &ownerRectangle);

        const int ownerWidth = static_cast<int>(
            ownerRectangle.right - ownerRectangle.left);
        const int ownerHeight = static_cast<int>(
            ownerRectangle.bottom - ownerRectangle.top);
        const int x = static_cast<int>(ownerRectangle.left) +
            std::max(0, (ownerWidth - width) / 2);
        const int y = static_cast<int>(ownerRectangle.top) +
            std::max(0, (ownerHeight - height) / 2);

        hwnd_ = CreateWindowExW(
            WS_EX_DLGMODALFRAME,
            windowClass.lpszClassName,
            L"SSH 服务器管理",
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
            x, y, width, height,
            owner, nullptr, instance, this);
        if (hwnd_ == nullptr) {
            return 1;
        }

        EnableWindow(owner, FALSE);
        ShowWindow(hwnd_, SW_SHOW);
        UpdateWindow(hwnd_);

        MSG message{};
        while (IsWindow(hwnd_) && GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (!IsDialogMessageW(hwnd_, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }

        EnableWindow(owner, TRUE);
        SetActiveWindow(owner);
        SetForegroundWindow(owner);
        return 0;
    }

private:
    enum : int {
        ListId = 1001,
        AliasId = 1002,
        HostId = 1003,
        UserId = 1004,
        PortId = 1005,
        IdentityId = 1006,
        ProxyId = 1007,
        AuthId = 1008,
        PasswordId = 1009,
        StatusId = 1010,
        AddId = 1011,
        SaveId = 1012,
        TestId = 1013,
        UseId = 1014,
        RemoveId = 1015,
        LocalId = 1016,
        CloseId = 1017
    };

    static LRESULT CALLBACK windowProc(
        HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        auto* self = reinterpret_cast<SshManagerDialog*>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));

        if (message == WM_NCCREATE) {
            const auto* creation = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = static_cast<SshManagerDialog*>(creation->lpCreateParams);
            self->hwnd_ = hwnd;
            SetWindowLongPtrW(
                hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }

        if (self == nullptr) {
            return DefWindowProcW(hwnd, message, wParam, lParam);
        }

        return self->handle(message, wParam, lParam);
    }

    int scaled(int value) const {
        return static_cast<int>(std::lround(value * scale_));
    }

    std::string controlText(HWND controlHandle) const {
        const int length = GetWindowTextLengthW(controlHandle);
        const auto bufferLength = static_cast<std::size_t>(
            std::max(0, length)) + 1;
        std::wstring value(bufferLength, L'\0');

        GetWindowTextW(
            controlHandle, value.data(), static_cast<int>(value.size()));
        value.resize(static_cast<std::size_t>(std::max(0, length)));
        return utf8FromWide(value.c_str());
    }

    HWND createControl(
        const wchar_t* type,
        const wchar_t* label,
        DWORD style,
        int x,
        int y,
        int width,
        int height,
        int controlId,
        DWORD extendedStyle = 0,
        bool tabStop = true) {
        const DWORD windowStyle =
            WS_CHILD | WS_VISIBLE | style | (tabStop ? WS_TABSTOP : 0);
        HWND result = CreateWindowExW(
            extendedStyle,
            type,
            label,
            windowStyle,
            scaled(x), scaled(y), scaled(width), scaled(height),
            hwnd_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(controlId)),
            instance_,
            nullptr);

        if (result != nullptr && font_ != nullptr) {
            SendMessageW(
                result, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        }
        return result;
    }

    void createControls() {
        font_ = CreateFontW(
            -scaled(14), 0, 0, 0, FW_NORMAL,
            FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

        createControl(
            L"STATIC", L"本机 SSH 配置", SS_LEFT,
            20, 20, 280, 26, 0, 0, false);
        list_ = createControl(
            L"LISTBOX", L"",
            LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_BORDER,
            20, 52, 300, 520, ListId, WS_EX_CLIENTEDGE);

        const auto addLabel = [this](
            const wchar_t* label, int x, int y, int width) {
            createControl(
                L"STATIC", label, SS_LEFT,
                x, y, width, 20, 0, 0, false);
        };

        addLabel(L"服务器别名", 350, 24, 270);
        addLabel(L"主机名或 IP 地址", 660, 24, 300);
        alias_ = createControl(
            L"EDIT", L"", ES_AUTOHSCROLL,
            350, 48, 270, 32, AliasId, WS_EX_CLIENTEDGE);
        host_ = createControl(
            L"EDIT", L"", ES_AUTOHSCROLL,
            660, 48, 300, 32, HostId, WS_EX_CLIENTEDGE);

        addLabel(L"用户名", 350, 104, 270);
        addLabel(L"SSH 端口", 660, 104, 130);
        user_ = createControl(
            L"EDIT", L"", ES_AUTOHSCROLL,
            350, 128, 270, 32, UserId, WS_EX_CLIENTEDGE);
        port_ = createControl(
            L"EDIT", L"22", ES_AUTOHSCROLL | ES_NUMBER,
            660, 128, 130, 32, PortId, WS_EX_CLIENTEDGE);

        addLabel(L"私钥文件（可留空以使用 SSH Agent）", 350, 184, 610);
        identity_ = createControl(
            L"EDIT", L"", ES_AUTOHSCROLL,
            350, 208, 610, 32, IdentityId, WS_EX_CLIENTEDGE);

        addLabel(L"跳转服务器（可选，填写 SSH config 中的 Host 别名）",
                 350, 264, 610);
        proxy_ = createControl(
            L"EDIT", L"", ES_AUTOHSCROLL,
            350, 288, 610, 32, ProxyId, WS_EX_CLIENTEDGE);

        addLabel(L"认证方式", 350, 344, 250);
        passwordLabel_ = createControl(
            L"STATIC", L"密码（仅密码认证时填写）", SS_LEFT,
            660, 344, 300, 20, 0, 0, false);
        auth_ = createControl(
            L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL,
            350, 368, 280, 32, AuthId);
        SendMessageW(
            auth_, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(L"SSH 私钥 / Agent 认证"));
        SendMessageW(
            auth_, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(L"密码认证"));
        SendMessageW(auth_, CB_SETCURSEL, 0, 0);

        password_ = createControl(
            L"EDIT", L"", ES_AUTOHSCROLL | ES_PASSWORD,
            660, 368, 300, 32, PasswordId, WS_EX_CLIENTEDGE);
        updatePasswordVisibility();

        status_ = createControl(
            L"STATIC", L"服务器列表来自本机 SSH 配置文件。", SS_LEFT,
            20, 592, 940, 32, StatusId, 0, false);

        const auto addButton = [this](
            const wchar_t* label, int id, int x, int width = 112) {
            return createControl(
                L"BUTTON", label, BS_PUSHBUTTON,
                x, 642, width, 42, id);
        };

        addButton(L"新增", AddId, 20);
        addButton(L"保存并同步", SaveId, 140, 132);
        addButton(L"测试连接", TestId, 280);
        addButton(L"使用此服务器", UseId, 400, 132);
        addButton(L"删除", RemoveId, 540);
        addButton(L"本机编码", LocalId, 660);
        addButton(L"关闭", CloseId, 780);

        refreshList();
    }

    void updatePasswordVisibility() {
        const bool passwordAuthentication =
            SendMessageW(auth_, CB_GETCURSEL, 0, 0) == 1;
        const int visibility = passwordAuthentication ? SW_SHOW : SW_HIDE;

        ShowWindow(passwordLabel_, visibility);
        ShowWindow(password_, visibility);

        if (!passwordAuthentication) {
            SetWindowTextW(password_, L"");
        }
    }

    void setStatus(const std::string& message) {
        SetWindowTextW(status_, wide(message).c_str());
    }

    void refreshList() {
        servers_ = controller_->sshServers();
        SendMessageW(list_, LB_RESETCONTENT, 0, 0);

        for (const auto& server : servers_) {
            const std::string description =
                server.alias + "  ·  " + server.user + "@" +
                server.hostName + ":" + std::to_string(server.port);
            const std::wstring label = wide(description);
            SendMessageW(
                list_, LB_ADDSTRING, 0,
                reinterpret_cast<LPARAM>(label.c_str()));
        }
    }

    void clearFields() {
        SetWindowTextW(alias_, L"");
        SetWindowTextW(host_, L"");
        SetWindowTextW(user_, L"");
        SetWindowTextW(port_, L"22");
        SetWindowTextW(identity_, L"");
        SetWindowTextW(proxy_, L"");
        SetWindowTextW(password_, L"");
        SendMessageW(auth_, CB_SETCURSEL, 0, 0);
        updatePasswordVisibility();
        SendMessageW(list_, LB_SETCURSEL, static_cast<WPARAM>(-1), 0);
    }

    void populateServer(int index) {
        if (index < 0 || index >= static_cast<int>(servers_.size())) {
            return;
        }

        const auto& server = servers_[static_cast<std::size_t>(index)];
        SetWindowTextW(alias_, wide(server.alias).c_str());
        SetWindowTextW(host_, wide(server.hostName).c_str());
        SetWindowTextW(user_, wide(server.user).c_str());
        SetWindowTextW(port_, std::to_wstring(server.port).c_str());
        SetWindowTextW(identity_, wide(server.identityFile).c_str());
        SetWindowTextW(proxy_, wide(server.proxyJump).c_str());
        SetWindowTextW(password_, L"");

        const int authenticationIndex =
            server.authMethod == SshAuthMethod::Password ? 1 : 0;
        SendMessageW(auth_, CB_SETCURSEL, authenticationIndex, 0);
        updatePasswordVisibility();
    }

    bool saveFields() {
        SshServer server;
        server.alias = controlText(alias_);
        server.hostName = controlText(host_);
        server.user = controlText(user_);
        server.identityFile = controlText(identity_);
        server.proxyJump = controlText(proxy_);

        const std::string portText = controlText(port_);
        try {
            std::size_t parsedCharacters = 0;
            const unsigned long parsedPort = std::stoul(portText, &parsedCharacters);
            if (parsedCharacters != portText.size() || parsedPort > 65535) {
                server.port = 0;
            } else {
                server.port = static_cast<unsigned int>(parsedPort);
            }
        } catch (const std::exception&) {
            server.port = 0;
        }

        server.authMethod =
            SendMessageW(auth_, CB_GETCURSEL, 0, 0) == 1
                ? SshAuthMethod::Password
                : SshAuthMethod::PublicKey;

        std::string error;
        if (!controller_->saveSshServer(server, controlText(password_), error)) {
            setStatus(error);
            return false;
        }

        SetWindowTextW(password_, L"");
        refreshList();

        for (std::size_t index = 0; index < servers_.size(); ++index) {
            if (servers_[index].alias == server.alias) {
                SendMessageW(
                    list_, LB_SETCURSEL, static_cast<WPARAM>(index), 0);
                break;
            }
        }

        setStatus("SSH 配置已保存，原有的其他 SSH 选项已保留。");
        return true;
    }

    void useSelectedServer() {
        if (!saveFields()) {
            return;
        }

        std::string error;
        if (!controller_->selectRemoteServer(controlText(alias_), error)) {
            setStatus(error);
            return;
        }

        DestroyWindow(hwnd_);
    }

    void removeSelectedServer() {
        const int selectedIndex =
            static_cast<int>(SendMessageW(list_, LB_GETCURSEL, 0, 0));
        if (selectedIndex < 0 ||
            selectedIndex >= static_cast<int>(servers_.size())) {
            setStatus("请先选择要删除的服务器。");
            return;
        }

        const std::string alias =
            servers_[static_cast<std::size_t>(selectedIndex)].alias;
        std::string error;
        if (!controller_->removeSshServer(alias, error)) {
            setStatus(error);
            return;
        }

        refreshList();
        clearFields();
        setStatus("服务器已删除，SSH 配置已同步更新。");
    }

    LRESULT handle(UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
        case WM_CREATE:
            createControls();
            return 0;

        case WM_COMMAND: {
            const int controlId = LOWORD(wParam);
            const int notification = HIWORD(wParam);

            if (controlId == ListId && notification == LBN_SELCHANGE) {
                const int selectedIndex =
                    static_cast<int>(SendMessageW(list_, LB_GETCURSEL, 0, 0));
                populateServer(selectedIndex);
                return 0;
            }

            if (controlId == AuthId && notification == CBN_SELCHANGE) {
                updatePasswordVisibility();
                return 0;
            }

            if (controlId == AddId) {
                clearFields();
                setStatus("填写服务器信息，然后保存。");
                return 0;
            }

            if (controlId == SaveId) {
                saveFields();
                return 0;
            }

            if (controlId == TestId) {
                if (saveFields()) {
                    std::string error;
                    controller_->testSshServer(controlText(alias_), error);
                    setStatus(error);
                }
                return 0;
            }

            if (controlId == UseId) {
                useSelectedServer();
                return 0;
            }

            if (controlId == RemoveId) {
                removeSelectedServer();
                return 0;
            }

            if (controlId == LocalId) {
                controller_->selectLocalEncoder();
                DestroyWindow(hwnd_);
                return 0;
            }

            if (controlId == CloseId) {
                DestroyWindow(hwnd_);
                return 0;
            }
            break;
        }

        case WM_CLOSE:
            DestroyWindow(hwnd_);
            return 0;

        case WM_DESTROY:
            if (font_ != nullptr) {
                DeleteObject(font_);
                font_ = nullptr;
            }
            hwnd_ = nullptr;
            return 0;

        default:
            break;
        }

        return DefWindowProcW(hwnd_, message, wParam, lParam);
    }

    HINSTANCE instance_{};
    HWND hwnd_{};
    AppController* controller_{};
    double scale_{1.0};

    HFONT font_{};
    HWND list_{};
    HWND alias_{};
    HWND host_{};
    HWND user_{};
    HWND port_{};
    HWND identity_{};
    HWND proxy_{};
    HWND auth_{};
    HWND passwordLabel_{};
    HWND password_{};
    HWND status_{};

    std::vector<SshServer> servers_;
};

class WindowsWindow {
public:
    explicit WindowsWindow(AppController& controller) : controller_(controller) {}
    int run(HINSTANCE instance) {
        instance_ = instance;
        WNDCLASSW wc{}; wc.lpfnWndProc = &WindowsWindow::windowProc; wc.hInstance = instance;
        wc.lpszClassName = L"FFmpegStudioBatchWindow"; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr; RegisterClassW(&wc);
        const DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
        RECT rect{0, 0, 1500, 1020}; AdjustWindowRect(&rect, style, FALSE);
        window_ = CreateWindowExW(0, wc.lpszClassName, L"FFmpeg Studio", style,
            CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top,
            nullptr, nullptr, instance, this);
        if (!window_) return 1;
        DragAcceptFiles(window_, TRUE);
        applyModernStyle(); ShowWindow(window_, SW_SHOW); UpdateWindow(window_);
        SetTimer(window_, 1, 160, nullptr);
        MSG msg{}; while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        return static_cast<int>(msg.wParam);
    }
private:
    static LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<WindowsWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lp);
            self = static_cast<WindowsWindow*>(create->lpCreateParams); self->window_ = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        return self ? self->handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
    }
    double scale() const {
        const UINT dpi = GetDpiForWindow(window_);
        return static_cast<double>(dpi == 0 ? 96 : dpi) / 96.0;
    }
    int s(int v) const { return static_cast<int>(std::lround(v * scale())); }
    HFONT font(int px, int weight = FW_NORMAL) const {
        return CreateFontW(-s(px), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    }
    void applyModernStyle() {
        const BOOL dark = dark_ ? TRUE : FALSE;
        const int rounded = 2, mica = 2;
        const COLORREF caption = dark_ ? RGB(15,19,29) : RGB(243,246,251);
        const COLORREF border = dark_ ? RGB(44,56,76) : RGB(225,231,240);
        DwmSetWindowAttribute(window_, static_cast<DWMWINDOWATTRIBUTE>(20), &dark, sizeof(dark));
        DwmSetWindowAttribute(window_, static_cast<DWMWINDOWATTRIBUTE>(33), &rounded, sizeof(rounded));
        DwmSetWindowAttribute(window_, static_cast<DWMWINDOWATTRIBUTE>(34), &border, sizeof(border));
        DwmSetWindowAttribute(window_, static_cast<DWMWINDOWATTRIBUTE>(35), &caption, sizeof(caption));
        DwmSetWindowAttribute(window_, static_cast<DWMWINDOWATTRIBUTE>(38), &mica, sizeof(mica));
        SetWindowTheme(window_, dark_ ? L"DarkMode_Explorer" : L"Explorer", nullptr);
    }
    void updateSettings() { controller_.updateSettings(settings_); }
    void showSshManager() {
        SshManagerDialog dialog;
        dialog.run(instance_,window_,controller_);
        settings_=controller_.snapshot().settings;
        applyModernStyle();
        InvalidateRect(window_,nullptr,FALSE);
    }
    void addFiles(bool archiveOnly = false) {
        std::vector<wchar_t> buffer(65536);
        const wchar_t mediaFilter[] =
            L"支持的媒体文件\0"
            L"*.mp4;*.m4v;*.mov;*.mkv;*.avi;*.webm;*.wmv;*.flv;"
            L"*.mpg;*.mpeg;*.3gp;*.ts;*.m2ts;*.mts;*.zip;*.pptx\0"
            L"所有文件\0*.*\0\0";
        const wchar_t archiveFilter[] = L"ZIP 和 PowerPoint 文件\0*.zip;*.pptx\0所有文件\0*.*\0\0";
        OPENFILENAMEW dialog{}; dialog.lStructSize = sizeof(dialog); dialog.hwndOwner = window_;
        dialog.lpstrFilter = archiveOnly ? archiveFilter : mediaFilter;
        dialog.lpstrFile = buffer.data(); dialog.nMaxFile = static_cast<DWORD>(buffer.size());
        dialog.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_ALLOWMULTISELECT | OFN_NOCHANGEDIR;
        dialog.lpstrTitle = archiveOnly ? L"添加 ZIP 或 PowerPoint 文件" : L"添加视频、ZIP 或 PPTX 文件";
        if (!GetOpenFileNameW(&dialog)) return;
        std::vector<std::string> paths;
        const std::wstring first = buffer.data();
        const wchar_t* cursor = buffer.data() + first.size() + 1;
        if (*cursor == L'\0') paths.push_back(utf8FromWide(first.c_str()));
        else {
            while (*cursor) {
                std::filesystem::path path(first); path /= cursor;
                paths.push_back(utf8FromWide(path.c_str())); cursor += wcslen(cursor) + 1;
            }
        }
        controller_.addInputs(paths);
    }
    void addFolder() {
        BROWSEINFOW info{}; info.hwndOwner = window_; info.lpszTitle = L"选择包含视频或压缩包的文件夹";
        info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_EDITBOX;
        PIDLIST_ABSOLUTE item = SHBrowseForFolderW(&info);
        if (!item) return;
        std::array<wchar_t, 32768> path{};
        if (SHGetPathFromIDListW(item, path.data())) controller_.addInputs({utf8FromWide(path.data())});
        CoTaskMemFree(item);
    }
    void importQueueDialog() {
        std::array<wchar_t,32768> path{};
        const wchar_t filter[]=L"FFmpeg Studio 任务清单\0*.ffq;*.txt\0所有文件\0*.*\0\0";
        OPENFILENAMEW dialog{};dialog.lStructSize=sizeof(dialog);dialog.hwndOwner=window_;dialog.lpstrFilter=filter;
        dialog.lpstrFile=path.data();dialog.nMaxFile=static_cast<DWORD>(path.size());
        dialog.Flags=OFN_EXPLORER|OFN_FILEMUSTEXIST|OFN_NOCHANGEDIR;dialog.lpstrTitle=L"导入任务清单";
        if(!GetOpenFileNameW(&dialog))return;
        std::string error;const auto added=controller_.importQueue(utf8FromWide(path.data()),error);
        if(!error.empty())MessageBoxW(window_,wide(error).c_str(),L"导入失败",MB_ICONERROR|MB_OK);
        else if(added==0)MessageBoxW(window_,L"清单中没有新增任务。",L"导入完成",MB_ICONINFORMATION|MB_OK);
    }
    void exportQueueDialog() {
        std::array<wchar_t,32768> path{};
        const wchar_t filter[]=L"FFmpeg Studio 任务清单\0*.ffq\0文本文件\0*.txt\0\0";
        OPENFILENAMEW dialog{};dialog.lStructSize=sizeof(dialog);dialog.hwndOwner=window_;dialog.lpstrFilter=filter;
        dialog.lpstrFile=path.data();dialog.nMaxFile=static_cast<DWORD>(path.size());
        dialog.lpstrDefExt=L"ffq";dialog.Flags=OFN_EXPLORER|OFN_OVERWRITEPROMPT|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
        dialog.lpstrTitle=L"导出任务清单";
        if(!GetSaveFileNameW(&dialog))return;
        std::string error;if(!controller_.exportQueue(utf8FromWide(path.data()),error))
            MessageBoxW(window_,wide(error).c_str(),L"导出失败",MB_ICONERROR|MB_OK);
    }
    void chooseOutput() {
        BROWSEINFOW info{}; info.hwndOwner = window_; info.lpszTitle = L"选择输出文件夹";
        info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_EDITBOX;
        PIDLIST_ABSOLUTE item = SHBrowseForFolderW(&info);
        if (!item) return;
        std::array<wchar_t, 32768> path{};
        if (SHGetPathFromIDListW(item, path.data())) {
            settings_.outputDirectory = utf8FromWide(path.data()); updateSettings();
        }
        CoTaskMemFree(item);
    }
    void commitCrf(int x, const Layout& l) {
        editingCrf_=false;crfEditText_.clear();
        const int minX = l.crfTrack.x, maxX = l.crfTrack.x + l.crfTrack.width;
        const double fraction = std::clamp(static_cast<double>(x - minX) / std::max(1, maxX - minX), 0.0, 1.0);
        settings_.crf = static_cast<int>(std::lround(fraction * crfMaximum(settings_.codec)));
        updateSettings();
    }
    void commitCrfText() {
        if (!editingCrf_) return;
        if (!crfEditText_.empty()) {
            try {
                settings_.crf = std::clamp(
                    std::stoi(crfEditText_), 0, crfMaximum(settings_.codec));
                updateSettings();
            } catch (const std::exception&) {
                // Keep the previous value when the entered CRF is invalid.
            }
        }
        editingCrf_ = false;
        crfEditText_.clear();
    }
    void startOrCancel() {
        const auto snap = controller_.snapshot();
        if (!snap.running) {
            if (!controller_.startBatch()) {
                MessageBoxW(
                    window_,
                    L"请添加待处理文件，并确认当前编码器可用。",
                    L"无法开始任务",
                    MB_ICONINFORMATION | MB_OK);
            }
        } else if (snap.paused) controller_.resumeBatch();
        else controller_.pauseBatch();
    }
    std::vector<std::pair<BatchTaskSnapshot, BatchItemSnapshot>> visibleRows(const AppSnapshot& snap) const {
        std::vector<std::pair<BatchTaskSnapshot, BatchItemSnapshot>> rows;
        for (const auto& task : snap.tasks) {
            if (task.kind == TaskKind::Video) {
                BatchItemSnapshot item;
                if (!task.items.empty()) item = task.items.front();
                else {
                    item.name = basename(task.inputPath);
                    item.state = task.state;
                    item.progress = task.progress;
                    item.detail = task.detail;
                }
                rows.emplace_back(task, item);
            } else {
                BatchItemSnapshot parent; parent.name = basename(task.inputPath); parent.state = task.state;
                parent.progress = task.progress; parent.detail = task.detail;
                rows.emplace_back(task, parent);
                for (const auto& item : task.items) rows.emplace_back(task, item);
            }
        }
        return rows;
    }
    void drawProgress(HDC dc, Rect r, double value, const Palette& c) {
        fillRound(dc, r, c.track, c.track, 5);
        const int fill = static_cast<int>(std::lround(r.width * std::clamp(value, 0.0, 1.0)));
        if (fill > 0) fillRound(dc, {r.x, r.y, fill, r.height}, c.accent, c.accent, 5);
    }
    void draw() {
        const auto snap = controller_.snapshot();
        RECT client{}; GetClientRect(window_, &client); const int width = client.right, height = client.bottom;
        HDC target = GetDC(window_);
        HDC dc = CreateCompatibleDC(target);
        HBITMAP bitmap = CreateCompatibleBitmap(target, width, height);
        auto oldBitmap = SelectObject(dc, bitmap);
        const Palette c = palette(dark_); const auto backends = availableBackends(snap.capabilities, settings_.codec);
        const auto l = makeLayout(width, height, scale(), static_cast<int>(backends.size()));
        HBRUSH backgroundBrush = CreateSolidBrush(c.background);
        RECT fullRectangle{0, 0, width, height};
        FillRect(dc, &fullRectangle, backgroundBrush);
        DeleteObject(backgroundBrush);
        HFONT title = font(25, FW_BOLD), normal = font(14), smallFont = font(12), heading = font(16, FW_SEMIBOLD);
        drawText(dc, {s(30),s(22),s(420),s(38)}, L"FFmpeg Studio", c.text, title);
        drawText(dc, {s(31),s(60),s(700),s(24)}, L"批量视频压缩 · ZIP / PPTX 媒体处理 · CPU 与硬件编码", c.muted, smallFont);
        drawButton(dc, l.ssh, L"SSH 服务器", c, smallFont, !settings_.remoteHost.empty());
        drawButton(dc, l.theme, dark_ ? L"浅色外观" : L"深色外观", c, smallFont);

        drawButton(dc,l.addFiles,L"添加文件",c,normal,true); drawButton(dc,l.addFolder,L"添加文件夹",c,normal);
        drawButton(dc,l.addArchive,L"添加 ZIP / PPTX",c,normal);
        drawButton(dc,l.importQueue,L"导入清单",c,smallFont); drawButton(dc,l.exportQueue,L"导出清单",c,smallFont);
        drawButton(dc,l.removeSelected,L"移除选中",c,smallFont); drawButton(dc,l.clearDone,L"清除已完成",c,smallFont);

        const int leftX = s(28), top = s(168), leftW = l.queue.width;
        drawCard(dc, {leftX, top, leftW, s(528)}, c);
        drawText(dc,{leftX+s(20),top+s(14),s(200),s(26)},L"编码设置",c.text,heading);
        drawText(dc,{leftX+s(20),top+s(43),s(220),s(20)},L"输出文件夹",c.muted,smallFont);
        const std::wstring output = settings_.outputDirectory.empty()
            ? L"默认：与每个源文件保存在同一文件夹"
            : wide(settings_.outputDirectory);
        drawText(dc,l.outputField,output,c.text,smallFont);
        drawButton(dc,l.outputBrowse,L"选择目录",c,smallFont);
        const Rect labelColumn{leftX + s(20), 0, s(120), s(32)};
        drawText(dc,{labelColumn.x,top+s(112),labelColumn.width,labelColumn.height},L"输出容器",c.muted,smallFont);
        const std::array<std::wstring,2> containers{L"MP4",L"与源文件相同"};
        for (int index = 0; index < 2; ++index) {
            drawButton(dc,l.container[index],containers[index],c,smallFont,
                static_cast<int>(settings_.outputContainer)==index);
        }
        drawText(dc,{labelColumn.x,top+s(153),labelColumn.width,s(32)},L"视频编码",c.muted,smallFont);
        const std::array<std::wstring,3> codecs{L"H.264",L"H.265",L"AV1"};
        for (int index=0;index<3;++index) {
            drawButton(dc,l.codec[index],codecs[index],c,normal,
                static_cast<int>(settings_.codec)==index);
        }
        drawText(dc,{labelColumn.x,top+s(194),labelColumn.width,s(32)},L"编码设备",c.muted,smallFont);
        const std::array<std::wstring,4> backendLabels{L"CPU",L"NVIDIA NVENC",L"Intel QSV",L"AMD AMF"};
        for (std::size_t index=0;index<backends.size();++index) {
            std::size_t labelIndex=0;
            switch(backends[index]) {
            case EncoderBackend::CPU: labelIndex=0; break;
            case EncoderBackend::NVENC: labelIndex=1; break;
            case EncoderBackend::QSV: labelIndex=2; break;
            case EncoderBackend::AMF: labelIndex=3; break;
            }
            drawButton(dc,l.backend[index],backendLabels[labelIndex],c,smallFont,
                settings_.backend==backends[index]);
        }
        if (backends.empty()) {
            drawText(dc,{l.backend[0].x,top+s(194),l.backend[0].width,s(32)},
                L"当前 FFmpeg 环境没有可用的编码器",c.red,smallFont);
        }
        drawText(dc,{labelColumn.x,top+s(229),labelColumn.width,s(32)},L"目标质量",c.muted,smallFont);
        drawProgress(dc,{l.crfTrack.x,l.crfTrack.y,l.crfTrack.width,l.crfTrack.height},
            static_cast<double>(settings_.crf)/crfMaximum(settings_.codec),c);
        const double crfFraction=static_cast<double>(settings_.crf)/crfMaximum(settings_.codec);
        const int knobX=l.crfTrack.x+static_cast<int>(std::lround(l.crfTrack.width*crfFraction));
        fillRound(dc,{knobX-s(7),l.crfTrack.y-s(5),s(14),s(20)},c.accent,c.accent,7);
        const std::wstring crfValue=editingCrf_
            ? (crfEditText_.empty()?L"输入":crfEditText_)
            : std::to_wstring(settings_.crf);
        drawButton(dc,l.crfValue,crfValue,c,normal,true);
        drawText(dc,{labelColumn.x,top+s(280),labelColumn.width,s(65)},L"压缩级别",c.muted,smallFont);
        const std::array<std::wstring,8> levels{L"极速",L"非常快",L"快速",L"均衡",L"慢速",L"较慢",L"很慢",L"极致压缩"};
        for(int index=0;index<8;++index) {
            drawButton(dc,l.compression[index],levels[index],c,smallFont,
                static_cast<int>(settings_.compression)==index);
        }
        drawText(dc,{labelColumn.x,top+s(351),labelColumn.width,s(30)},L"音频码率",c.muted,smallFont);
        const std::array<int,6> rates{96,128,160,192,256,320};
        for(int index=0;index<6;++index) {
            drawButton(dc,l.audio[index],std::to_wstring(rates[index])+L" kbps",c,smallFont,
                settings_.audioBitrateKbps==rates[index],
                settings_.audioMode!=AudioMode::Encode);
        }
        drawText(dc,{labelColumn.x,top+s(389),labelColumn.width,s(30)},L"输出分辨率",c.muted,smallFont);
        const std::array<std::wstring,6> resolutionLabels{L"原始尺寸",L"2160p",L"1440p",L"1080p",L"720p",L"480p"};
        for(int index=0;index<6;++index) {
            drawButton(dc,l.resolution[index],resolutionLabels[index],c,smallFont,
                static_cast<int>(settings_.outputResolution)==index);
        }
        drawText(dc,{labelColumn.x,top+s(427),labelColumn.width,s(30)},L"音频处理",c.muted,smallFont);
        const std::array<std::wstring,3> audioModes{L"自动编码",L"保留原音频",L"移除音频"};
        for(int index=0;index<3;++index) {
            drawButton(dc,l.audioMode[index],audioModes[index],c,smallFont,
                static_cast<int>(settings_.audioMode)==index);
        }
        const std::wstring parallelLabel = settings_.maxParallelJobs == 0
            ? L"并行：自动"
            : L"并行：" + std::to_wstring(settings_.maxParallelJobs);
        drawButton(dc, l.parallel, parallelLabel, c, smallFont);
        drawButton(dc,l.recursive,settings_.recursive?L"递归扫描✓":L"递归扫描",c,smallFont,settings_.recursive);
        drawButton(
            dc, l.skip,
            settings_.skipAlreadyProcessed ? L"跳过重复✓" : L"跳过重复",
            c, smallFont, settings_.skipAlreadyProcessed);
        drawButton(
            dc, l.preserve,
            settings_.preserveDirectoryStructure ? L"保留目录✓" : L"保留目录",
            c, smallFont, settings_.preserveDirectoryStructure);
        drawButton(dc,l.overwrite,settings_.overwrite?L"覆盖文件✓":L"保留文件",c,smallFont,settings_.overwrite);

        drawCard(dc,l.queue,c);
        drawText(dc,{l.queue.x+s(20),l.queue.y+s(14),s(280),s(26)},L"处理队列",c.text,heading);
        const std::wstring queueSummary =
            L"总计 " + std::to_wstring(snap.totalTasks) +
            L"  ·  运行 " + std::to_wstring(snap.runningTasks) +
            L"  ·  完成 " + std::to_wstring(snap.completedTasks) +
            L"  ·  失败 " + std::to_wstring(snap.failedTasks);
        drawText(
            dc,
            {l.queue.x + l.queue.width - s(340), l.queue.y + s(16), s(320), s(22)},
            queueSummary, c.muted, smallFont,
            DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        const auto rows = visibleRows(snap);
        const int rowHeight = s(68);
        const int listTop = l.queue.y + s(54);
        const int visibleRowsCount =
            std::max(1, (l.queue.height - s(68)) / rowHeight);
        const int maxScroll = std::max(
            0, static_cast<int>(rows.size()) - visibleRowsCount);
        scrollOffset_ = std::clamp(scrollOffset_, 0, maxScroll);
        if(rows.empty()) {
            drawText(dc,{l.queue.x+s(30),listTop+s(36),l.queue.width-s(60),s(100)},
                L"将视频、ZIP 或 PPTX 文件拖到窗口中，或使用上方按钮添加文件和文件夹。",c.muted,normal,DT_CENTER|DT_VCENTER|DT_WORDBREAK);
        } else {
            for (int i = 0;
                 i < visibleRowsCount &&
                 i + scrollOffset_ < static_cast<int>(rows.size());
                 ++i) {
                const auto& [task,item]=rows[static_cast<std::size_t>(i+scrollOffset_)];
                const int y = listTop + i * rowHeight;
                const bool child =
                    task.kind != TaskKind::Video &&
                    item.name != basename(task.inputPath);
                if (child) {
                    drawText(
                        dc,
                        {l.queue.x + s(38), y + s(4), l.queue.width - s(58), s(20)},
                        L"↳ " + wide(item.name), c.muted, smallFont);
                } else {
                    drawText(
                        dc,
                        {l.queue.x + s(20), y + s(5), l.queue.width - s(40), s(22)},
                        wide(item.name) + L"   ·   " + wide(taskKindName(task.kind)),
                        c.text, normal);
                }
                const TaskState state = child ? item.state : task.state;
                const double progress = child ? item.progress : task.progress;
                if (selectedTask_ == task.id)
                    fillRound(
                        dc,
                        {l.queue.x + s(10), y + s(2),
                         l.queue.width - s(20), rowHeight - s(4)},
                        c.accentSoft, c.accentSoft, 7);
                const std::wstring stateText=wide(taskStateName(state));
                const COLORREF stateColor =
                    state == TaskState::Failed
                        ? c.red
                        : (state == TaskState::Completed || state == TaskState::Skipped)
                            ? c.green
                            : c.muted;
                drawText(
                    dc,
                    {l.queue.x + l.queue.width - s(160), y + s(5), s(140), s(20)},
                    stateText, stateColor, smallFont,
                    DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
                drawProgress(dc,{l.queue.x+s(20),y+s(36),l.queue.width-s(180),s(7)},progress,c);
                std::wstring detail;
                if (child) {
                    detail = formatProgressLine(item.progressInfo, item.detail);
                } else if (task.kind == TaskKind::Video && !task.items.empty()) {
                    detail = formatProgressLine(
                        task.items.front().progressInfo, task.detail);
                } else {
                    detail = wide(task.detail);
                }
                const std::wstring sizeLine = child
                    ? formatTaskSize(item.inputSizeBytes, item.outputSizeBytes)
                    : formatTaskSize(task.inputSizeBytes, task.outputSizeBytes);
                if (!sizeLine.empty() &&
                    (state == TaskState::Completed || state == TaskState::Skipped)) {
                    detail = sizeLine;
                }
                drawText(
                    dc,
                    {l.queue.x + l.queue.width - s(148), y + s(27), s(128), s(22)},
                    detail, c.muted, smallFont,
                    DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
                HPEN separatorPen = CreatePen(PS_SOLID, 1, c.border);
                HGDIOBJ previousPen = SelectObject(dc, separatorPen);
                MoveToEx(dc, l.queue.x + s(18), y + rowHeight - s(2), nullptr);
                LineTo(dc, l.queue.x + l.queue.width - s(18), y + rowHeight - s(2));
                SelectObject(dc, previousPen);
                DeleteObject(separatorPen);
            }
        }
        if (rows.size() > static_cast<std::size_t>(visibleRowsCount)) {
            drawText(
                dc,
                {l.queue.x + l.queue.width - s(130),
                 l.queue.y + l.queue.height - s(24), s(110), s(18)},
                L"滚动查看其余任务", c.muted, smallFont,
                DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        }

        drawCard(dc,l.overallCard,c);
        drawText(
            dc,
            {l.overallCard.x + s(22), l.overallCard.y + s(18),
             l.overallCard.width - s(44), s(28)},
            L"批量处理进度", c.text, heading);
        drawText(
            dc,
            {l.overallCard.x + s(22), l.overallCard.y + s(54),
             l.overallCard.width - s(44), s(24)},
            wide(snap.status), c.text, normal);
        drawText(
            dc,
            {l.overallCard.x + s(22), l.overallCard.y + s(82),
             l.overallCard.width - s(44), s(22)},
            wide(snap.detail), c.muted, smallFont);
        drawProgress(dc,l.overallTrack,snap.overallProgress,c);
        wchar_t percent[24]{}; swprintf_s(percent,L"%.1f%%",snap.overallProgress*100.0);
        drawText(dc,{l.overallCard.x+s(22),l.overallCard.y+s(144),s(100),s(26)},percent,c.accent,heading);
        const std::wstring completedLabel =
            L"完成 " + std::to_wstring(snap.completedTasks + snap.skippedTasks) +
            L" / " + std::to_wstring(snap.totalTasks);
        drawText(
            dc,
            {l.overallCard.x + s(130), l.overallCard.y + s(145),
             l.overallCard.width - s(152), s(24)},
            completedLabel, c.muted, smallFont);
        if (!snap.running) {
            drawButton(dc,l.start,L"开始批量处理",c,normal,true,
                snap.tasks.empty() || !snap.capabilities.supports(settings_.codec,settings_.backend));
        } else {
            drawButton(dc,l.pause,snap.paused?L"继续处理":L"暂停处理",c,normal,true);
            drawButton(dc,l.cancel,L"取消任务",c,normal,false);
        }

        const int infoY=l.overallCard.y+l.overallCard.height+s(18);
        const Rect info{l.overallCard.x,infoY,l.overallCard.width,height-infoY-s(28)};
        drawCard(dc,info,c);
        drawText(dc,{info.x+s(22),info.y+s(18),info.width-s(44),s(26)},L"设备与编码摘要",c.text,heading);
        drawText(
            dc, {info.x + s(22), info.y + s(58), info.width - s(44), s(22)},
            L"格式  " + wide(codecName(settings_.codec)) +
                L" · " + wide(outputContainerName(settings_.outputContainer)),
            c.text, normal);
        drawText(
            dc, {info.x + s(22), info.y + s(88), info.width - s(44), s(22)},
            L"编码器  " + wide(backendName(settings_.backend)), c.text, normal);
        drawText(
            dc, {info.x + s(22), info.y + s(118), info.width - s(44), s(22)},
            L"CRF  " + std::to_wstring(settings_.crf) +
                L"  ·  " + wide(compressionName(settings_.compression)),
            c.text, normal);
        drawText(
            dc, {info.x + s(22), info.y + s(148), info.width - s(44), s(22)},
            L"音频  " + wide(audioModeName(settings_.audioMode)) +
                (settings_.audioMode == AudioMode::Encode
                    ? L" · " + std::to_wstring(settings_.audioBitrateKbps) + L" kbps"
                    : L""),
            c.text, normal);
        drawText(
            dc, {info.x + s(22), info.y + s(178), info.width - s(44), s(22)},
            L"分辨率  " + wide(outputResolutionName(settings_.outputResolution)),
            c.text, normal);
        const std::wstring computeTarget = settings_.remoteHost.empty()
            ? L"计算设备  本机"
            : L"计算设备  远程 · " + wide(settings_.remoteHost);
        drawText(
            dc, {info.x + s(22), info.y + s(208), info.width - s(44), s(22)},
            computeTarget, c.text, normal);
        drawText(
            dc,
            {info.x + s(22), info.y + s(212), info.width - s(44), s(44)},
            wide(snap.resourcePlan.explanation), c.muted, smallFont,
            DT_LEFT | DT_TOP | DT_WORDBREAK);
        drawText(
            dc,
            {info.x + s(22), info.y + s(270), info.width - s(44), s(70)},
            L"支持的视频：H.264、H.265、AV1。\n"
            L"ZIP 与 PPTX 会保留其他文件并更新视频引用。\n"
            L"音频码率可独立设置，并按输出容器选择兼容的音频编码。",
            c.muted, smallFont, DT_LEFT | DT_TOP | DT_WORDBREAK);
        if (!snap.error.empty()) {
            drawText(
                dc,
                {info.x + s(22), info.y + info.height - s(74),
                 info.width - s(44), s(58)},
                wide(snap.error), c.red, smallFont,
                DT_LEFT | DT_TOP | DT_WORDBREAK);
        }

        DeleteObject(title); DeleteObject(normal); DeleteObject(smallFont); DeleteObject(heading);
        BitBlt(target, 0, 0, width, height, dc, 0, 0, SRCCOPY);
        SelectObject(dc, oldBitmap);
        DeleteObject(bitmap);
        DeleteDC(dc);
        ReleaseDC(window_, target);
    }
    void handleClick(int x, int y) {
        RECT clientRectangle{};
        GetClientRect(window_, &clientRectangle);

        const AppSnapshot snapshot = controller_.snapshot();
        const auto backends = availableBackends(snapshot.capabilities, settings_.codec);
        const Layout layout = makeLayout(
            clientRectangle.right, clientRectangle.bottom,
            scale(), static_cast<int>(backends.size()));

        if (editingCrf_ && !contains(layout.crfValue, x, y)) {
            commitCrfText();
        }

        if (contains(layout.ssh, x, y)) {
            if (!snapshot.running) {
                showSshManager();
            }
            return;
        }
        if (contains(layout.theme, x, y)) {
            dark_ = !dark_;
            applyModernStyle();
            return;
        }
        if (contains(layout.addFiles, x, y)) {
            addFiles();
            return;
        }
        if (contains(layout.addFolder, x, y)) {
            addFolder();
            return;
        }
        if (contains(layout.addArchive, x, y)) {
            addFiles(true);
            return;
        }
        if (contains(layout.importQueue, x, y)) {
            importQueueDialog();
            return;
        }
        if (contains(layout.exportQueue, x, y)) {
            exportQueueDialog();
            return;
        }
        if (contains(layout.removeSelected, x, y)) {
            if (selectedTask_ != 0) {
                controller_.removeTask(selectedTask_);
            }
            return;
        }
        if (contains(layout.clearDone, x, y)) {
            controller_.clearCompleted();
            return;
        }
        if (contains(layout.outputBrowse, x, y)) {
            chooseOutput();
            return;
        }
        if (!snapshot.running && contains(layout.start, x, y)) {
            startOrCancel();
            return;
        }
        if (snapshot.running && contains(layout.pause, x, y)) {
            if (snapshot.paused) {
                controller_.resumeBatch();
            } else {
                controller_.pauseBatch();
            }
            return;
        }
        if (snapshot.running && contains(layout.cancel, x, y)) {
            controller_.cancelBatch();
            return;
        }
        if (contains(layout.crfValue, x, y)) {
            editingCrf_ = true;
            crfEditText_.clear();
            SetFocus(window_);
            return;
        }

        for (std::size_t index = 0; index < 2; ++index) {
            if (contains(layout.container[index], x, y)) {
                settings_.outputContainer = static_cast<OutputContainer>(index);
                updateSettings();
                return;
            }
        }

        for (std::size_t index = 0; index < 3; ++index) {
            if (!contains(layout.codec[index], x, y)) {
                continue;
            }

            settings_.codec = static_cast<VideoCodec>(index);
            settings_.crf = defaultCrf(settings_.codec);

            const EncoderCapabilities capabilities = controller_.snapshot().capabilities;
            if (!capabilities.supports(settings_.codec, settings_.backend)) {
                for (const auto candidate : {
                         EncoderBackend::CPU,
                         EncoderBackend::NVENC,
                         EncoderBackend::QSV,
                         EncoderBackend::AMF
                     }) {
                    if (capabilities.supports(settings_.codec, candidate)) {
                        settings_.backend = candidate;
                        break;
                    }
                }
            }

            updateSettings();
            return;
        }

        for (std::size_t index = 0; index < backends.size(); ++index) {
            if (contains(layout.backend[index], x, y)) {
                settings_.backend = backends[index];
                updateSettings();
                return;
            }
        }

        for (std::size_t index = 0; index < 8; ++index) {
            if (contains(layout.compression[index], x, y)) {
                settings_.compression = static_cast<CompressionLevel>(index);
                updateSettings();
                return;
            }
        }

        const std::array<int, 6> audioRates{96, 128, 160, 192, 256, 320};
        if (settings_.audioMode == AudioMode::Encode) {
            for (std::size_t index = 0; index < audioRates.size(); ++index) {
                if (contains(layout.audio[index], x, y)) {
                    settings_.audioBitrateKbps = audioRates[index];
                    updateSettings();
                    return;
                }
            }
        }

        for (std::size_t index = 0; index < 6; ++index) {
            if (contains(layout.resolution[index], x, y)) {
                settings_.outputResolution = static_cast<OutputResolution>(index);
                updateSettings();
                return;
            }
        }

        for (std::size_t index = 0; index < 3; ++index) {
            if (contains(layout.audioMode[index], x, y)) {
                settings_.audioMode = static_cast<AudioMode>(index);
                updateSettings();
                return;
            }
        }

        if (contains(layout.parallel, x, y)) {
            const std::array<int, 5> choices{0, 1, 2, 4, 8};
            const auto current = std::find(
                choices.begin(), choices.end(), settings_.maxParallelJobs);
            const auto currentIndex = static_cast<std::size_t>(
                std::distance(choices.begin(), current));
            settings_.maxParallelJobs =
                choices[(currentIndex + 1) % choices.size()];
            updateSettings();
            return;
        }
        if (contains(layout.recursive, x, y)) {
            settings_.recursive = !settings_.recursive;
            updateSettings();
            return;
        }
        if (contains(layout.skip, x, y)) {
            settings_.skipAlreadyProcessed = !settings_.skipAlreadyProcessed;
            updateSettings();
            return;
        }
        if (contains(layout.preserve, x, y)) {
            settings_.preserveDirectoryStructure =
                !settings_.preserveDirectoryStructure;
            updateSettings();
            return;
        }
        if (contains(layout.overwrite, x, y)) {
            settings_.overwrite = !settings_.overwrite;
            updateSettings();
            return;
        }

        const int crfTop = layout.crfTrack.y - s(10);
        const int crfBottom = layout.crfTrack.y + s(18);
        const int crfRight = layout.crfTrack.x + layout.crfTrack.width;
        if (y >= crfTop && y <= crfBottom &&
            x >= layout.crfTrack.x && x <= crfRight) {
            draggingCrf_ = true;
            SetCapture(window_);
            commitCrf(x, layout);
            return;
        }

        const int queueTop = layout.queue.y + s(54);
        if (y >= queueTop &&
            x >= layout.queue.x &&
            x <= layout.queue.x + layout.queue.width) {
            const int rowHeight = s(68);
            const int visibleIndex = (y - queueTop) / rowHeight + scrollOffset_;
            const auto visibleItems = visibleRows(controller_.snapshot());

            if (visibleIndex >= 0 &&
                visibleIndex < static_cast<int>(visibleItems.size())) {
                selectedTask_ =
                    visibleItems[static_cast<std::size_t>(visibleIndex)].first.id;
            }
        }
    }

    LRESULT handle(UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
        case WM_CREATE: {
            const AppSnapshot snapshot = controller_.snapshot();
            settings_ = snapshot.settings;
            return 0;
        }
        case WM_SIZE:
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        case WM_GETMINMAXINFO: {
            auto* information = reinterpret_cast<MINMAXINFO*>(lParam);
            information->ptMinTrackSize.x = s(1180);
            information->ptMinTrackSize.y = s(820);
            return 0;
        }
        case WM_DPICHANGED: {
            const auto* suggestedRectangle = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(
                window_, nullptr,
                suggestedRectangle->left,
                suggestedRectangle->top,
                suggestedRectangle->right - suggestedRectangle->left,
                suggestedRectangle->bottom - suggestedRectangle->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
            applyModernStyle();
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        }
        case WM_LBUTTONDOWN:
            handleClick(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        case WM_MOUSEMOVE:
            if (draggingCrf_ && (wParam & MK_LBUTTON) != 0) {
                RECT clientRectangle{};
                GetClientRect(window_, &clientRectangle);
                const Layout layout = makeLayout(
                    clientRectangle.right, clientRectangle.bottom, scale());
                commitCrf(GET_X_LPARAM(lParam), layout);
                InvalidateRect(window_, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONUP:
            if (draggingCrf_) {
                draggingCrf_ = false;
                ReleaseCapture();
            }
            return 0;
        case WM_CHAR:
            if (editingCrf_) {
                if (wParam >= L'0' && wParam <= L'9' && crfEditText_.size() < 3) {
                    crfEditText_.push_back(static_cast<wchar_t>(wParam));
                } else if (wParam == 8 && !crfEditText_.empty()) {
                    crfEditText_.pop_back();
                } else if (wParam == 13) {
                    commitCrfText();
                } else if (wParam == 27) {
                    editingCrf_ = false;
                    crfEditText_.clear();
                }

                InvalidateRect(window_, nullptr, FALSE);
                return 0;
            }
            break;
        case WM_KEYDOWN:
            if (editingCrf_ && wParam == VK_ESCAPE) {
                editingCrf_ = false;
                crfEditText_.clear();
                InvalidateRect(window_, nullptr, FALSE);
                return 0;
            }
            break;
        case WM_MOUSEWHEEL:
            scrollOffset_ = std::max(
                0, scrollOffset_ + (GET_WHEEL_DELTA_WPARAM(wParam) > 0 ? -3 : 3));
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        case WM_DROPFILES: {
            const HDROP drop = reinterpret_cast<HDROP>(wParam);
            const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            std::vector<std::string> paths;
            paths.reserve(count);

            for (UINT index = 0; index < count; ++index) {
                const UINT length = DragQueryFileW(drop, index, nullptr, 0);
                std::wstring path(static_cast<std::size_t>(length) + 1, L'\0');
                DragQueryFileW(drop, index, path.data(), length + 1);
                paths.push_back(utf8FromWide(path.c_str()));
            }

            DragFinish(drop);
            controller_.addInputs(paths);
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        }
        case WM_TIMER:
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            BeginPaint(window_, &paint);
            draw();
            EndPaint(window_, &paint);
            return 0;
        }
        case WM_DESTROY:
            KillTimer(window_, 1);
            controller_.shutdown();
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(window_, message, wParam, lParam);
        }

        return DefWindowProcW(window_, message, wParam, lParam);
    }
    AppController& controller_; HWND window_{}; HINSTANCE instance_{}; BatchSettings settings_; bool dark_{false};
    bool draggingCrf_{false};
    bool editingCrf_{false};
    std::wstring crfEditText_;
    int scrollOffset_{0};
    std::uint64_t selectedTask_{0};
};

} // namespace

int runNativeUi(AppController& controller) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WindowsWindow window(controller);
    return window.run(GetModuleHandleW(nullptr));
}

} // namespace ffmpegui
