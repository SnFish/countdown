#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <sapi.h>
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <limits>
#include <iterator>
#include "timer.hpp"
#include "resource.h"

namespace {
constexpr wchar_t windowClass[] = L"Countdown.Native.Window";
constexpr int windowWidth = 150, windowHeight = 48;
constexpr UINT trayMessage = WM_APP + 1;
constexpr UINT_PTR tickId = 1;
enum Command : UINT { Toggle = 200, Reset, Lock, Settings, Hide, Topmost, Voice, Help, Quit };
enum Hotkey : int { HotToggle = 1, HotReset, HotLock, HotQuit };

struct Config {
    int seconds = 290, voiceSeconds = 45, volume = 70;
    int minimumSeconds = 1, maximumSeconds = 300;
    int background = 50, textAlpha = 100, lockAlpha = 70;
    COLORREF color = RGB(255, 255, 255);
    COLORREF backgroundColor = RGB(57, 57, 57);
    bool voice = true, topmost = true, hint = false;
    int x = std::numeric_limits<int>::min(), y = 218;
};

class Speech {
    ISpVoice* voice_ = nullptr;
    bool attempted_ = false;
public:
    ~Speech() { if (voice_) voice_->Release(); }
    bool say(const wchar_t* text, int volume) {
        if (!attempted_) {
            attempted_ = true;
            if (SUCCEEDED(CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_INPROC_SERVER,
                                          IID_ISpVoice, reinterpret_cast<void**>(&voice_)))) {
                // Prefer the installed Mandarin voice, otherwise use the system default.
                ISpObjectTokenCategory* category = nullptr;
                if (SUCCEEDED(CoCreateInstance(CLSID_SpObjectTokenCategory, nullptr, CLSCTX_INPROC_SERVER,
                                               IID_ISpObjectTokenCategory, reinterpret_cast<void**>(&category)))) {
                    IEnumSpObjectTokens* tokens = nullptr;
                    category->SetId(SPCAT_VOICES, FALSE);
                    if (SUCCEEDED(category->EnumTokens(L"Language=804", nullptr, &tokens))) {
                        ISpObjectToken* token = nullptr;
                        if (tokens->Next(1, &token, nullptr) == S_OK) {
                            voice_->SetVoice(token);
                            token->Release();
                        }
                        tokens->Release();
                    }
                    category->Release();
                }
                voice_->SetRate(3);
            }
        }
        if (!voice_) return false;
        voice_->SetVolume(static_cast<USHORT>(volume));
        return SUCCEEDED(voice_->Speak(text, SPF_ASYNC | SPF_PURGEBEFORESPEAK | SPF_IS_NOT_XML, nullptr));
    }
    void stop() { if (voice_) voice_->Speak(nullptr, SPF_ASYNC | SPF_PURGEBEFORESPEAK, nullptr); }
};

// Two reusable DIBs: a premultiplied output surface and a grayscale GDI mask.
// GDI's alpha channel is undefined, so explicitly composite the mask ourselves.
class Surface {
    HDC dc_ = nullptr, maskDc_ = nullptr;
    HBITMAP bitmap_ = nullptr, mask_ = nullptr;
    HGDIOBJ old_ = nullptr, oldMask_ = nullptr;
    DWORD* pixels_ = nullptr;
    DWORD* maskPixels_ = nullptr;
    int width_ = 0, height_ = 0;
public:
    ~Surface() { release(); }
    void release() {
        if (dc_) {
            if (old_) SelectObject(dc_, old_);
            if (bitmap_) DeleteObject(bitmap_);
            DeleteDC(dc_);
        }
        if (maskDc_) {
            if (oldMask_) SelectObject(maskDc_, oldMask_);
            if (mask_) DeleteObject(mask_);
            DeleteDC(maskDc_);
        }
        dc_ = maskDc_ = nullptr;
        bitmap_ = mask_ = nullptr;
        old_ = oldMask_ = nullptr;
        pixels_ = maskPixels_ = nullptr;
        width_ = height_ = 0;
    }
    bool resize(int width, int height) {
        if (width == width_ && height == height_ && dc_) return true;
        release();
        width_ = width; height_ = height;
        BITMAPINFO info{};
        info.bmiHeader = {sizeof(BITMAPINFOHEADER), width, -height, 1, 32, BI_RGB, 0, 0, 0, 0, 0};
        dc_ = CreateCompatibleDC(nullptr);
        maskDc_ = CreateCompatibleDC(nullptr);
        if (!dc_ || !maskDc_) { release(); return false; }
        bitmap_ = CreateDIBSection(dc_, &info, DIB_RGB_COLORS, reinterpret_cast<void**>(&pixels_), nullptr, 0);
        mask_ = CreateDIBSection(maskDc_, &info, DIB_RGB_COLORS, reinterpret_cast<void**>(&maskPixels_), nullptr, 0);
        if (!bitmap_ || !mask_) { release(); return false; }
        old_ = SelectObject(dc_, bitmap_);
        oldMask_ = SelectObject(maskDc_, mask_);
        SetBkMode(maskDc_, TRANSPARENT);
        SetTextColor(maskDc_, RGB(255, 255, 255));
        return true;
    }
    HDC dc() const { return dc_; }
    HDC maskDc() const { return maskDc_; }
    void clear() { std::fill_n(pixels_, width_ * height_, 0); }
    void clearMask() { GdiFlush(); std::fill_n(maskPixels_, width_ * height_, 0); }
    void blend(int x, int y, COLORREF color, int alpha) {
        if (x < 0 || y < 0 || x >= width_ || y >= height_ || alpha <= 0) return;
        DWORD& dst = pixels_[y * width_ + x];
        const int inv = 255 - alpha;
        const int r = (GetRValue(color) * alpha + ((dst >> 16) & 255) * inv + 127) / 255;
        const int g = (GetGValue(color) * alpha + ((dst >> 8) & 255) * inv + 127) / 255;
        const int b = (GetBValue(color) * alpha + (dst & 255) * inv + 127) / 255;
        const int a = alpha + (((dst >> 24) * inv + 127) / 255);
        dst = (static_cast<DWORD>(a) << 24) | (r << 16) | (g << 8) | b;
    }
    void composite(COLORREF color, int alpha) {
        GdiFlush();
        for (int y = 0; y < height_; ++y) for (int x = 0; x < width_; ++x) {
            const DWORD mask = maskPixels_[y * width_ + x];
            const int coverage = (GetRValue(mask) + GetGValue(mask) + GetBValue(mask)) / 3;
            blend(x, y, color, (coverage * alpha + 127) / 255);
        }
    }
    void rounded(int left, int top, int right, int bottom, float radius, COLORREF color, int alpha, bool replace = false) {
        if (right <= left || bottom <= top) return;
        radius = std::clamp(radius, 0.0f, std::min(right - left, bottom - top) / 2.0f);
        for (int y = top; y < bottom; ++y) for (int x = left; x < right; ++x) {
            const float cx = std::clamp(x + 0.5f, left + radius, right - radius);
            const float cy = std::clamp(y + 0.5f, top + radius, bottom - radius);
            const float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            const float coverage = std::clamp(radius + 0.5f - std::sqrt(dx * dx + dy * dy), 0.0f, 1.0f);
            if (replace && x >= 0 && y >= 0 && x < width_ && y < height_) {
                const DWORD target = (static_cast<DWORD>(alpha) << 24) | ((GetRValue(color) * alpha / 255) << 16)
                    | ((GetGValue(color) * alpha / 255) << 8) | (GetBValue(color) * alpha / 255);
                DWORD& pixel = pixels_[y * width_ + x];
                DWORD value = 0;
                for (int shift : {0, 8, 16, 24}) {
                    const int channel = static_cast<int>(((target >> shift) & 255) * coverage + ((pixel >> shift) & 255) * (1 - coverage));
                    value |= static_cast<DWORD>(channel) << shift;
                }
                pixel = value;
            } else blend(x, y, color, static_cast<int>(alpha * coverage));
        }
    }

    void lockIcon(int cx, int cy, float scale, bool locked, COLORREF color) {
        const auto segment = [](float x, float y, float ax, float ay, float bx, float by) {
            const float dx = bx - ax, dy = by - ay;
            const float t = std::clamp(((x - ax) * dx + (y - ay) * dy) / (dx * dx + dy * dy), 0.0f, 1.0f);
            return std::hypot(x - ax - t * dx, y - ay - t * dy);
        };
        const float radius = locked ? 4.0f : 5.0f;
        const float arcX = locked ? 0.0f : 1.0f, arcY = locked ? -5.0f : -6.0f;
        for (int y = cy - static_cast<int>(13 * scale); y <= cy + static_cast<int>(12 * scale); ++y) {
            for (int x = cx - static_cast<int>(10 * scale); x <= cx + static_cast<int>(10 * scale); ++x) {
                const float px = (x + .5f - cx) / scale, py = (y + .5f - cy) / scale;
                const float qx = std::abs(px) - 4.5f, qy = std::abs(py - 4.5f) - 3.0f;
                const float body = std::abs(std::hypot(std::max(qx, 0.0f), std::max(qy, 0.0f))
                                            + std::min(std::max(qx, qy), 0.0f) - 2.5f);
                const float arc = py <= arcY ? std::abs(std::hypot(px - arcX, py - arcY) - radius)
                    : std::min(std::hypot(px - arcX - radius, py - arcY), std::hypot(px - arcX + radius, py - arcY));
                float distance = std::min(body, std::min(arc, segment(px, py, -4, -1, -4, arcY)));
                if (locked) distance = std::min(distance, segment(px, py, 4, -1, 4, arcY));
                const float outline = std::clamp((.85f - distance) * scale + .5f, 0.0f, 1.0f);
                const float hole = std::max(std::clamp((1.05f - std::hypot(px, py - 3)) * scale + .5f, 0.0f, 1.0f),
                                            std::clamp((.65f - segment(px, py, 0, 3, 0, 6)) * scale + .5f, 0.0f, 1.0f));
                blend(x, y, color, static_cast<int>(std::max(outline, hole) * 255));
            }
        }
    }
};

class App {
    HINSTANCE instance_;
    HWND window_ = nullptr, tooltip_ = nullptr;
    Config config_;
    countdown::Timer timer_;
    Speech speech_;
    Surface surface_;
    HFONT font_ = nullptr, hourFont_ = nullptr;
    NOTIFYICONDATAW tray_{};
    wchar_t configPath_[MAX_PATH]{};
    UINT dpi_ = 96, taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
    int lastSecond_ = -1, wheelRemainder_ = 0;
    int hoverUnit_ = 0, wheelUnit_ = 0;
    bool locked_ = false, pressed_ = false, moving_ = false, pressLock_ = false, lockHover_ = false;
    bool trayAdded_ = false, dirty_ = false;
    POINT dragStart_{}, windowStart_{};

    int scale(int value) const { return MulDiv(value, static_cast<int>(dpi_), 96); }
    int width() const { return scale(windowWidth); }
    int height() const { return scale(windowHeight); }
    RECT timeRect() const { return {scale(6), 0, scale(116), height()}; }
    RECT lockRect() const { return {scale(120), scale(10), scale(144), scale(38)}; }
    bool inLock(POINT point) const { auto rect = lockRect(); return PtInRect(&rect, point); }
    static std::uint64_t now() { return GetTickCount64(); }

    bool prepareConfigPath() {
        wchar_t directory[MAX_PATH]{};
        const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", directory, static_cast<DWORD>(std::size(directory)));
        if (length >= std::size(directory)) return false;
        if (!length && FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, directory))) return false;
        if (wcslen(directory) + std::size(L"\\Countdown\\settings.ini") > std::size(configPath_)) return false;
        swprintf_s(configPath_, L"%s\\Countdown\\settings.ini", directory);
        wcscat_s(directory, L"\\Countdown");
        if (!CreateDirectoryW(directory, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
        const DWORD directoryAttributes = GetFileAttributesW(directory);
        if (directoryAttributes == INVALID_FILE_ATTRIBUTES || !(directoryAttributes & FILE_ATTRIBUTE_DIRECTORY)) return false;

        const DWORD attributes = GetFileAttributesW(configPath_);
        if (attributes != INVALID_FILE_ATTRIBUTES) {
            if (attributes & FILE_ATTRIBUTE_DIRECTORY) return false;
            const HANDLE file = CreateFileW(configPath_, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE) return false;
            CloseHandle(file);
            return true;
        }
        const DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
    }

    void load() {
        auto read = [&](const wchar_t* key, int fallback, int low, int high) {
            return std::clamp(static_cast<int>(GetPrivateProfileIntW(L"Settings", key, fallback, configPath_)), low, high);
        };
        config_.minimumSeconds = read(L"MinSecs", config_.minimumSeconds, 1, countdown::Timer::maxSeconds);
        config_.maximumSeconds = read(L"MaxSecs", config_.maximumSeconds, config_.minimumSeconds, countdown::Timer::maxSeconds);
        config_.seconds = read(L"DefaultSecs", config_.seconds, config_.minimumSeconds, config_.maximumSeconds);
        config_.voiceSeconds = read(L"VoiceSeconds", config_.voiceSeconds, 1, 60);
        config_.volume = read(L"Volume", config_.volume, 0, 100);
        config_.background = read(L"Background", config_.background, 0, 100);
        config_.textAlpha = read(L"TextAlpha", config_.textAlpha, 20, 100);
        config_.lockAlpha = read(L"LockedAlpha", config_.lockAlpha, 20, 100);
        config_.color = static_cast<COLORREF>(GetPrivateProfileIntW(L"Settings", L"TimeColor", config_.color, configPath_)) & 0xffffff;
        config_.backgroundColor = static_cast<COLORREF>(GetPrivateProfileIntW(L"Settings", L"BackgroundColor", config_.backgroundColor, configPath_)) & 0xffffff;
        config_.voice = read(L"Voice", config_.voice, 0, 1) != 0;
        config_.topmost = read(L"Topmost", config_.topmost, 0, 1) != 0;
        config_.hint = read(L"ShowHint", config_.hint, 0, 1) != 0;
        config_.x = static_cast<int>(GetPrivateProfileIntW(L"Settings", L"X", config_.x, configPath_));
        config_.y = static_cast<int>(GetPrivateProfileIntW(L"Settings", L"Y", config_.y, configPath_));
        timer_.setRange(config_.minimumSeconds, config_.maximumSeconds, now());
        timer_.reset(config_.seconds);
    }

    void save() {
        if (!dirty_) return;
        RECT rect{};
        GetWindowRect(window_, &rect);
        auto write = [&](const wchar_t* key, int value) {
            wchar_t text[32]{};
            swprintf_s(text, L"%d", value);
            return WritePrivateProfileStringW(L"Settings", key, text, configPath_) != FALSE;
        };
        bool ok = true;
        ok &= write(L"DefaultSecs", config_.seconds);
        ok &= write(L"MinSecs", config_.minimumSeconds);
        ok &= write(L"MaxSecs", config_.maximumSeconds);
        ok &= write(L"VoiceSeconds", config_.voiceSeconds);
        ok &= write(L"Volume", config_.volume);
        ok &= write(L"Background", config_.background);
        ok &= write(L"TextAlpha", config_.textAlpha);
        ok &= write(L"LockedAlpha", config_.lockAlpha);
        ok &= write(L"TimeColor", static_cast<int>(config_.color));
        ok &= write(L"BackgroundColor", static_cast<int>(config_.backgroundColor));
        ok &= write(L"Voice", config_.voice);
        ok &= write(L"Topmost", config_.topmost);
        ok &= write(L"ShowHint", config_.hint);
        ok &= write(L"X", rect.left);
        ok &= write(L"Y", rect.top);
        if (ok) dirty_ = false;
    }

    void keepOnScreen(int x, int y) {
        RECT requested{x, y, x + width(), y + height()};
        MONITORINFO info{sizeof(info)};
        GetMonitorInfoW(MonitorFromRect(&requested, MONITOR_DEFAULTTONEAREST), &info);
        const auto& area = info.rcWork;
        x = static_cast<int>(std::clamp<LONG>(x, area.left, std::max(area.left, area.right - width())));
        y = static_cast<int>(std::clamp<LONG>(y, area.top, std::max(area.top, area.bottom - height())));
        SetWindowPos(window_, nullptr, x, y, width(), height(), SWP_NOZORDER | SWP_NOACTIVATE);
    }

    void makeFont() {
        if (font_) DeleteObject(font_);
        if (hourFont_) DeleteObject(hourFont_);
        font_ = CreateFontW(-scale(32), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                            DEFAULT_PITCH, L"Consolas");
        hourFont_ = CreateFontW(-scale(24), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Consolas");
    }

    void formatTime(wchar_t* buffer, size_t size, int seconds) const {
        if (seconds >= 3600) swprintf_s(buffer, size, L"%02d:%02d:%02d", seconds / 3600, seconds / 60 % 60, seconds % 60);
        else swprintf_s(buffer, size, L"%02d:%02d", seconds / 60, seconds % 60);
    }

    struct TimeLayout { int left, glyph; };
    TimeLayout timeLayout(int seconds) const {
        wchar_t text[24]{};
        formatTime(text, std::size(text), seconds);
        const int length = static_cast<int>(wcslen(text));
        const auto old = SelectObject(surface_.maskDc(), seconds >= 3600 ? hourFont_ : font_);
        SIZE size{};
        GetTextExtentPoint32W(surface_.maskDc(), text, length, &size);
        SelectObject(surface_.maskDc(), old);
        const auto area = timeRect();
        return {static_cast<int>(area.left + (area.right - area.left - size.cx) / 2), std::max(1, static_cast<int>(size.cx) / length)};
    }

    int unitAt(POINT point) const {
        const auto area = timeRect();
        if (!PtInRect(&area, point)) return 0;
        const int seconds = timer_.seconds(now());
        const auto layout = timeLayout(seconds);
        const int character = std::max(0, (static_cast<int>(point.x) - layout.left) / layout.glyph);
        if (seconds >= 3600) return character < 3 ? 3600 : character < 6 ? 60 : 1;
        return character < 3 ? 60 : 1;
    }

    void draw() {
        if (!window_ || !font_ || !surface_.resize(width(), height())) return;
        surface_.clear();
        const int opacity = 255;
        const auto tint = [](COLORREF base, COLORREF color, int amount) {
            return RGB((GetRValue(base) * (100 - amount) + GetRValue(color) * amount) / 100,
                       (GetGValue(base) * (100 - amount) + GetGValue(color) * amount) / 100,
                       (GetBValue(base) * (100 - amount) + GetBValue(color) * amount) / 100);
        };
        const auto background = config_.backgroundColor;
        const bool light = GetRValue(background) * 299 + GetGValue(background) * 587 + GetBValue(background) * 114 > 150000;
        const COLORREF foreground = light ? RGB(42, 48, 56) : RGB(220, 228, 238);
        const COLORREF lockedColor = light ? RGB(150, 90, 20) : RGB(242, 188, 105);
        surface_.rounded(0, 0, width(), height(), static_cast<float>(scale(10)), tint(background, foreground, 17), config_.background * opacity / 100);
        surface_.rounded(scale(1), scale(1), width() - scale(1), height() - scale(1), static_cast<float>(scale(9)), background, config_.background * opacity / 100, true);

        const auto state = timer_.state();
        wchar_t text[24]{};
        const int seconds = timer_.seconds(now());
        POINT screenPoint{};
        GetCursorPos(&screenPoint);
        POINT pointer = screenPoint;
        ScreenToClient(window_, &pointer);
        const bool overWindow = WindowFromPoint(screenPoint) == window_;
        hoverUnit_ = !locked_ && overWindow ? unitAt(pointer) : 0;
        lockHover_ = overWindow && inLock(pointer);
        formatTime(text, std::size(text), seconds);
        surface_.clearMask();
        auto oldFont = SelectObject(surface_.maskDc(), seconds >= 3600 ? hourFont_ : font_);
        auto area = timeRect();
        DrawTextW(surface_.maskDc(), text, -1, &area, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(surface_.maskDc(), oldFont);
        const int textOpacity = config_.textAlpha * opacity / 100;
        surface_.composite(config_.color,
                           state == countdown::State::Paused ? textOpacity * 70 / 100 : textOpacity);

        if (hoverUnit_ && !locked_) {
            const auto layout = timeLayout(seconds);
            const int index = seconds >= 3600 ? (hoverUnit_ == 3600 ? 0 : hoverUnit_ == 60 ? 3 : 6) : (hoverUnit_ == 60 ? 0 : 3);
            const int left = layout.left + index * layout.glyph;
            const COLORREF textColor = config_.color;
            const auto luminance = [](COLORREF color) { return GetRValue(color) * 299 + GetGValue(color) * 587 + GetBValue(color) * 114; };
            const COLORREF underline = std::abs(luminance(textColor) - luminance(background)) < 50000 ? foreground : textColor;
            surface_.rounded(left, height() - scale(6), left + 2 * layout.glyph, height() - scale(4),
                             static_cast<float>(scale(1)), underline, 230);
        }

        auto button = lockRect();
        surface_.lockIcon((button.left + button.right) / 2, (button.top + button.bottom) / 2,
                          static_cast<float>(dpi_) / 96, locked_,
                          locked_ ? lockedColor : lockHover_ ? foreground : tint(background, foreground, 72));

        POINT origin{};
        SIZE size{width(), height()};
        BLENDFUNCTION blend{AC_SRC_OVER, 0, static_cast<BYTE>(locked_ ? config_.lockAlpha * 255 / 100 : 255), AC_SRC_ALPHA};
        UpdateLayeredWindow(window_, nullptr, nullptr, &size, surface_.dc(), &origin, 0, &blend, ULW_ALPHA);
        updateTitle(text);
    }

    void updateTitle(const wchar_t* time) {
        wchar_t text[128]{};
        const auto state = timer_.state();
        const wchar_t* status = state == countdown::State::Running ? L"进行中" : state == countdown::State::Paused ? L"已暂停" : state == countdown::State::Finished ? L"时间到" : L"就绪";
        swprintf_s(text, L"倒计时 %s · %s%s", time, status, locked_ ? L" · 已锁定" : L"");
        SetWindowTextW(window_, text);
        wcscpy_s(tray_.szTip, text);
        if (trayAdded_) Shell_NotifyIconW(NIM_MODIFY, &tray_);
    }

    void addTray() {
        tray_.cbSize = sizeof(tray_);
        tray_.hWnd = window_;
        tray_.uID = 1;
        tray_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        tray_.uCallbackMessage = trayMessage;
        tray_.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_APP));
        wcscpy_s(tray_.szTip, L"倒计时");
        trayAdded_ = Shell_NotifyIconW(NIM_ADD, &tray_) != FALSE;
    }

    void updateTooltip() {
        TOOLINFOW info{sizeof(info)};
        info.uFlags = TTF_SUBCLASS;
        info.hwnd = window_;
        info.uId = 1;
        SendMessageW(tooltip_, TTM_DELTOOLW, 0, reinterpret_cast<LPARAM>(&info));
        info.rect = lockRect();
        info.lpszText = const_cast<wchar_t*>(locked_ ? L"解除锁定" : L"锁定，防止误触");
        SendMessageW(tooltip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&info));
        info.uId = 2;
        SendMessageW(tooltip_, TTM_DELTOOLW, 0, reinterpret_cast<LPARAM>(&info));
        if (config_.hint && !locked_) {
            info.rect = timeRect();
            info.lpszText = const_cast<wchar_t*>(L"单击开始 / 暂停；双击复位\n指向时、分或秒后滚动，调整对应单位\n拖动移动；右键设置");
            SendMessageW(tooltip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&info));
        }
    }

    void stopAudio() { speech_.stop(); }
    void refreshTimer() {
        if (timer_.state() == countdown::State::Running) SetTimer(window_, tickId, 250, nullptr);
        else KillTimer(window_, tickId);
        lastSecond_ = -1;
        draw();
    }
    void toggle() {
        timer_.toggle(now());
        if (timer_.state() != countdown::State::Running) stopAudio();
        refreshTimer();
    }
    void reset() { stopAudio(); timer_.reset(config_.seconds); refreshTimer(); }
    void tick() {
        const auto stamp = now();
        const bool finished = timer_.update(stamp);
        const int seconds = timer_.seconds(stamp);
        if (finished) {
            KillTimer(window_, tickId);
            if (!config_.voice || !speech_.say(L"时间到", config_.volume)) MessageBeep(MB_OK);
        } else if (seconds != lastSecond_ && config_.voice && seconds > 0 && seconds <= config_.voiceSeconds) {
            wchar_t text[16]{};
            swprintf_s(text, L"%d", seconds);
            speech_.say(text, config_.volume);
        }
        if (seconds != lastSecond_ || finished) { lastSecond_ = seconds; draw(); }
    }

    void setLock() { locked_ = !locked_; updateTooltip(); draw(); }
    void setTopmost() { SetWindowPos(window_, config_.topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE); }

    void menu(POINT point) {
        HMENU menu = CreatePopupMenu();
        auto add = [&](UINT id, const wchar_t* label, UINT flags = 0) { AppendMenuW(menu, MF_STRING | flags, id, label); };
        add(Toggle, timer_.state() == countdown::State::Running ? L"暂停\tCtrl+Alt+Space" : L"开始 / 继续\tCtrl+Alt+Space", locked_ ? MF_GRAYED : 0);
        add(Reset, L"复位\tCtrl+Alt+R", locked_ ? MF_GRAYED : 0);
        add(Lock, locked_ ? L"解除锁定\tCtrl+Alt+L" : L"锁定\tCtrl+Alt+L");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        add(Settings, L"设置…", locked_ ? MF_GRAYED : 0);
        add(Topmost, L"始终置顶", config_.topmost ? MF_CHECKED : 0);
        add(Voice, L"语音倒数", config_.voice ? MF_CHECKED : 0);
        add(Hide, IsWindowVisible(window_) ? L"隐藏到托盘" : L"显示计时器", !trayAdded_ ? MF_GRAYED : 0);
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        add(Help, L"操作说明");
        add(Quit, L"退出\tCtrl+Alt+X");
        SetForegroundWindow(window_);
        const UINT selected = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, window_, nullptr);
        DestroyMenu(menu);
        if (selected) command(selected);
        PostMessageW(window_, WM_NULL, 0, 0);
    }

    struct SettingsData { App* app; Config value; };
    static void fillSettings(HWND dialog, const Config& value) {
        SetDlgItemInt(dialog, IDC_HOURS, value.seconds / 3600, FALSE);
        SetDlgItemInt(dialog, IDC_MINUTES, value.seconds / 60 % 60, FALSE);
        SetDlgItemInt(dialog, IDC_SECONDS, value.seconds % 60, FALSE);
        SetDlgItemInt(dialog, IDC_MIN_HOURS, value.minimumSeconds / 3600, FALSE);
        SetDlgItemInt(dialog, IDC_MIN_MINUTES, value.minimumSeconds / 60 % 60, FALSE);
        SetDlgItemInt(dialog, IDC_MIN_SECONDS, value.minimumSeconds % 60, FALSE);
        SetDlgItemInt(dialog, IDC_MAX_HOURS, value.maximumSeconds / 3600, FALSE);
        SetDlgItemInt(dialog, IDC_MAX_MINUTES, value.maximumSeconds / 60 % 60, FALSE);
        SetDlgItemInt(dialog, IDC_MAX_SECONDS, value.maximumSeconds % 60, FALSE);
        SetDlgItemInt(dialog, IDC_VOICE_SECONDS, value.voiceSeconds, FALSE);
        SetDlgItemInt(dialog, IDC_VOLUME, value.volume, FALSE);
        SetDlgItemInt(dialog, IDC_BACKGROUND, value.background, FALSE);
        SetDlgItemInt(dialog, IDC_TEXT_ALPHA, value.textAlpha, FALSE);
        SetDlgItemInt(dialog, IDC_LOCK_ALPHA, value.lockAlpha, FALSE);
        CheckDlgButton(dialog, IDC_VOICE, value.voice ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dialog, IDC_TOPMOST, value.topmost ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dialog, IDC_HINT, value.hint ? BST_CHECKED : BST_UNCHECKED);
    }
    static INT_PTR CALLBACK settingsProc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
        auto data = reinterpret_cast<SettingsData*>(GetWindowLongPtrW(dialog, DWLP_USER));
        if (message == WM_INITDIALOG) {
            data = reinterpret_cast<SettingsData*>(lparam);
            SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(data));
            fillSettings(dialog, data->value);
            for (int id : {IDC_HOURS, IDC_MINUTES, IDC_SECONDS, IDC_MIN_HOURS, IDC_MIN_MINUTES, IDC_MIN_SECONDS,
                           IDC_MAX_HOURS, IDC_MAX_MINUTES, IDC_MAX_SECONDS, IDC_VOICE_SECONDS, IDC_VOLUME, IDC_BACKGROUND, IDC_TEXT_ALPHA, IDC_LOCK_ALPHA})
                SendDlgItemMessageW(dialog, id, EM_SETLIMITTEXT, 3, 0);
            RECT rect{}, owner{};
            GetWindowRect(dialog, &rect); GetWindowRect(data->app->window_, &owner);
            MONITORINFO info{sizeof(info)};
            GetMonitorInfoW(MonitorFromWindow(dialog, MONITOR_DEFAULTTONEAREST), &info);
            const int x = std::clamp(owner.left + (owner.right - owner.left - rect.right + rect.left) / 2,
                                     info.rcWork.left, std::max(info.rcWork.left, info.rcWork.right - rect.right + rect.left));
            const int y = std::clamp(owner.bottom + 8, info.rcWork.top, std::max(info.rcWork.top, info.rcWork.bottom - rect.bottom + rect.top));
            SetWindowPos(dialog, HWND_TOP, x, y, 0, 0, SWP_NOSIZE);
            return TRUE;
        }
        if (!data) return FALSE;
        if (message == WM_COMMAND) {
            const UINT id = LOWORD(wparam);
            if (id == IDCANCEL) { data->app->stopAudio(); EndDialog(dialog, IDCANCEL); return TRUE; }
            if (id == IDC_RESET_SETTINGS) { data->value = Config{}; fillSettings(dialog, data->value); return TRUE; }
            if (id == IDC_COLOR || id == IDC_BG_COLOR) {
                static COLORREF custom[16]{};
                auto& color = id == IDC_BG_COLOR ? data->value.backgroundColor : data->value.color;
                CHOOSECOLORW choose{sizeof(choose)};
                choose.hwndOwner = dialog; choose.rgbResult = color;
                choose.lpCustColors = custom; choose.Flags = CC_FULLOPEN | CC_RGBINIT;
                if (ChooseColorW(&choose)) color = choose.rgbResult;
                return TRUE;
            }
            if (id == IDC_PREVIEW) {
                BOOL valid = FALSE;
                const UINT volume = GetDlgItemInt(dialog, IDC_VOLUME, &valid, FALSE);
                if (valid && volume <= 100) {
                    if (!data->app->speech_.say(L"三、二、一，时间到", static_cast<int>(volume))) MessageBeep(MB_OK);
                } else MessageBoxW(dialog, L"音量请输入 0–100。", L"倒计时设置", MB_OK | MB_ICONINFORMATION);
                return TRUE;
            }
            if (id == IDOK) {
                struct Field { int id, low, high; int* value; };
                int hours = 0, minutes = 0, seconds = 0, minHours = 0, minMinutes = 0, minSeconds = 0,
                    maxHours = 0, maxMinutes = 0, maxSeconds = 0;
                Field fields[] = {{IDC_HOURS, 0, 99, &hours}, {IDC_MINUTES, 0, 59, &minutes}, {IDC_SECONDS, 0, 59, &seconds},
                    {IDC_MIN_HOURS, 0, 99, &minHours}, {IDC_MIN_MINUTES, 0, 59, &minMinutes}, {IDC_MIN_SECONDS, 0, 59, &minSeconds},
                    {IDC_MAX_HOURS, 0, 99, &maxHours}, {IDC_MAX_MINUTES, 0, 59, &maxMinutes}, {IDC_MAX_SECONDS, 0, 59, &maxSeconds},
                    {IDC_VOICE_SECONDS, 1, 60, &data->value.voiceSeconds}, {IDC_VOLUME, 0, 100, &data->value.volume},
                    {IDC_BACKGROUND, 0, 100, &data->value.background}, {IDC_TEXT_ALPHA, 20, 100, &data->value.textAlpha},
                    {IDC_LOCK_ALPHA, 20, 100, &data->value.lockAlpha}};
                for (auto& field : fields) {
                    BOOL valid = FALSE;
                    const UINT value = GetDlgItemInt(dialog, field.id, &valid, FALSE);
                    if (!valid || value < static_cast<UINT>(field.low) || value > static_cast<UINT>(field.high)) {
                        wchar_t text[80]{}; swprintf_s(text, L"请输入 %d–%d 范围内的整数。", field.low, field.high);
                        MessageBoxW(dialog, text, L"倒计时设置", MB_OK | MB_ICONINFORMATION);
                        SetFocus(GetDlgItem(dialog, field.id));
                        SendDlgItemMessageW(dialog, field.id, EM_SETSEL, 0, -1);
                        return TRUE;
                    }
                    *field.value = static_cast<int>(value);
                }
                data->value.seconds = hours * 3600 + minutes * 60 + seconds;
                data->value.minimumSeconds = minHours * 3600 + minMinutes * 60 + minSeconds;
                data->value.maximumSeconds = maxHours * 3600 + maxMinutes * 60 + maxSeconds;
                if (!data->value.minimumSeconds || data->value.minimumSeconds > data->value.maximumSeconds) {
                    MessageBoxW(dialog, L"最小时长至少为 1 秒，且不能大于最大时长。", L"倒计时设置", MB_OK | MB_ICONINFORMATION);
                    SetFocus(GetDlgItem(dialog, IDC_MIN_SECONDS)); return TRUE;
                }
                if (data->value.seconds < data->value.minimumSeconds || data->value.seconds > data->value.maximumSeconds) {
                    MessageBoxW(dialog, L"默认时长必须在最小和最大时长之间。", L"倒计时设置", MB_OK | MB_ICONINFORMATION);
                    SetFocus(GetDlgItem(dialog, IDC_MINUTES)); return TRUE;
                }
                data->value.voice = IsDlgButtonChecked(dialog, IDC_VOICE) == BST_CHECKED;
                data->value.topmost = IsDlgButtonChecked(dialog, IDC_TOPMOST) == BST_CHECKED;
                data->value.hint = IsDlgButtonChecked(dialog, IDC_HINT) == BST_CHECKED;
                data->app->stopAudio();
                EndDialog(dialog, IDOK);
                return TRUE;
            }
        }
        if (message == WM_CLOSE) { data->app->stopAudio(); EndDialog(dialog, IDCANCEL); return TRUE; }
        return FALSE;
    }

    void settings() {
        SettingsData data{this, config_};
        const int oldSeconds = config_.seconds;
        const int oldMinimum = config_.minimumSeconds, oldMaximum = config_.maximumSeconds;
        // A modal settings window has its own message loop; the countdown keeps running.
        if (DialogBoxParamW(instance_, MAKEINTRESOURCEW(IDD_SETTINGS), window_, settingsProc, reinterpret_cast<LPARAM>(&data)) != IDOK) return;
        config_ = data.value;
        if (config_.minimumSeconds != oldMinimum || config_.maximumSeconds != oldMaximum)
            timer_.setRange(config_.minimumSeconds, config_.maximumSeconds, now());
        if (config_.seconds != oldSeconds) {
            if (timer_.state() == countdown::State::Running) {
                timer_.toggle(now());
                timer_.reset(config_.seconds);
                timer_.toggle(now());
            } else timer_.reset(config_.seconds);
        }
        setTopmost(); updateTooltip(); dirty_ = true; save(); refreshTimer();
    }

    void command(UINT id) {
        if (locked_ && (id == Toggle || id == Reset || id == Settings)) return;
        switch (id) {
        case Toggle: toggle(); break;
        case Reset: reset(); break;
        case Lock: setLock(); break;
        case Settings: settings(); break;
        case Topmost: config_.topmost = !config_.topmost; setTopmost(); dirty_ = true; save(); break;
        case Voice: config_.voice = !config_.voice; if (!config_.voice) stopAudio(); dirty_ = true; save(); break;
        case Hide:
            if (IsWindowVisible(window_) && trayAdded_) ShowWindow(window_, SW_HIDE);
            else { ShowWindow(window_, SW_SHOWNOACTIVATE); draw(); }
            break;
        case Help:
            MessageBoxW(window_, L"单击时间：开始 / 暂停\n双击时间：复位\n指向时、分或秒滚动：增减对应单位\n可调时长的最小、最大值在设置中指定\n拖动：移动窗口\n锁定 / 解锁按钮：防止误触\n右键或托盘右键：设置、锁定、隐藏、退出\n锁定时禁止启动、复位、调时和拖动\n\nCtrl+Alt+Space：开始 / 暂停\nCtrl+Alt+R：复位\nCtrl+Alt+L：锁定 / 解锁\nCtrl+Alt+X：退出\n\n到时保持数字颜色并播放系统提示音。\n语音倒数可在设置中启用。", L"倒计时 · 操作说明", MB_OK);
            break;
        case Quit: DestroyWindow(window_); break;
        }
    }

    LRESULT handle(UINT message, WPARAM wparam, LPARAM lparam) {
        if (message == taskbarCreated_) { addTray(); draw(); return 0; }
        switch (message) {
        case WM_PRINT:
        case WM_PRINTCLIENT:
            draw();
            BitBlt(reinterpret_cast<HDC>(wparam), 0, 0, width(), height(), surface_.dc(), 0, 0, SRCCOPY);
            return 0;
        case WM_PAINT: { PAINTSTRUCT paint{}; BeginPaint(window_, &paint); EndPaint(window_, &paint); draw(); return 0; }
        case WM_ERASEBKGND: return 1;
        case WM_TIMER: if (wparam == tickId) tick(); return 0;
        case WM_LBUTTONDOWN: {
            pressLock_ = inLock({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
            if (locked_ && !pressLock_) return 0;
            pressed_ = true; moving_ = false;
            GetCursorPos(&dragStart_);
            RECT rect{}; GetWindowRect(window_, &rect); windowStart_ = {rect.left, rect.top};
            SetCapture(window_); return 0;
        }
        case WM_MOUSEMOVE:
            if (const bool hover = inLock({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)}); hover != lockHover_) {
                lockHover_ = hover; draw();
            }
            if (const int unit = locked_ ? 0 : unitAt({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)}); unit != hoverUnit_) {
                hoverUnit_ = unit; draw();
            }
            { TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window_, 0}; TrackMouseEvent(&track); }
            if (pressed_ && !pressLock_ && !locked_) {
                POINT point{}; GetCursorPos(&point);
                const int dx = point.x - dragStart_.x, dy = point.y - dragStart_.y;
                if (std::abs(dx) >= GetSystemMetricsForDpi(SM_CXDRAG, dpi_) || std::abs(dy) >= GetSystemMetricsForDpi(SM_CYDRAG, dpi_)) moving_ = true;
                if (moving_) {
                    SendMessageW(tooltip_, TTM_POP, 0, 0);
                    SetWindowPos(window_, nullptr, windowStart_.x + dx, windowStart_.y + dy, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                }
            }
            return 0;
        case WM_MOUSELEAVE:
            if (lockHover_ || hoverUnit_) { lockHover_ = false; hoverUnit_ = 0; draw(); }
            return 0;
        case WM_SETCURSOR: {
            POINT point{}; GetCursorPos(&point); ScreenToClient(window_, &point);
            SetCursor(LoadCursorW(nullptr, inLock(point) ? IDC_HAND : IDC_ARROW)); return TRUE;
        }
        case WM_LBUTTONUP:
            if (pressed_) {
                const bool moved = moving_, lock = pressLock_;
                pressed_ = moving_ = false; ReleaseCapture();
                if (moved) {
                    RECT rect{}; GetWindowRect(window_, &rect); keepOnScreen(rect.left, rect.top); dirty_ = true; save();
                } else if (lock && inLock({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)})) setLock();
                else if (!locked_ && !lock) toggle();
            }
            return 0;
        case WM_CAPTURECHANGED: pressed_ = moving_ = false; return 0;
        case WM_LBUTTONDBLCLK:
            if (!locked_ && !inLock({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)})) reset();
            return 0;
        case WM_MOUSEWHEEL:
            if (!locked_) {
                POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                ScreenToClient(window_, &point);
                const int amount = unitAt(point);
                if (!amount) return 0;
                if (amount != wheelUnit_) { wheelUnit_ = amount; wheelRemainder_ = 0; }
                wheelRemainder_ += GET_WHEEL_DELTA_WPARAM(wparam);
                const int steps = wheelRemainder_ / WHEEL_DELTA;
                wheelRemainder_ %= WHEEL_DELTA;
                if (steps) {
                    timer_.adjust(steps * amount, now());
                    if (timer_.state() != countdown::State::Running) {
                        config_.seconds = timer_.preset(); dirty_ = true; save();
                    }
                    stopAudio(); refreshTimer();
                }
            }
            return 0;
        case WM_CONTEXTMENU: {
            POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            if (point.x == -1 && point.y == -1) GetCursorPos(&point);
            menu(point); return 0;
        }
        case WM_COMMAND: command(LOWORD(wparam)); return 0;
        case WM_HOTKEY:
            if (wparam == HotToggle) command(Toggle);
            if (wparam == HotReset) command(Reset);
            if (wparam == HotLock) command(Lock);
            if (wparam == HotQuit) command(Quit);
            return 0;
        case WM_KEYDOWN:
            if (wparam == VK_SPACE) command(Toggle);
            if (wparam == 'R') command(Reset);
            if (wparam == 'L') command(Lock);
            if (wparam == VK_APPS) { POINT point{}; GetCursorPos(&point); menu(point); }
            return 0;
        case trayMessage:
            if (lparam == WM_LBUTTONUP) { ShowWindow(window_, SW_SHOWNOACTIVATE); draw(); }
            if (lparam == WM_RBUTTONUP) { POINT point{}; GetCursorPos(&point); menu(point); }
            return 0;
        case WM_DPICHANGED: {
            dpi_ = HIWORD(wparam); makeFont();
            const auto rect = reinterpret_cast<RECT*>(lparam);
            keepOnScreen(rect->left, rect->top); updateTooltip(); draw(); dirty_ = true; return 0;
        }
        case WM_DISPLAYCHANGE: {
            RECT rect{}; GetWindowRect(window_, &rect); keepOnScreen(rect.left, rect.top); draw(); return 0;
        }
        case WM_QUERYENDSESSION: dirty_ = true; save(); return TRUE;
        case WM_CLOSE: DestroyWindow(window_); return 0;
        case WM_DESTROY:
            stopAudio(); KillTimer(window_, tickId); dirty_ = true; save();
            if (trayAdded_) Shell_NotifyIconW(NIM_DELETE, &tray_);
            for (int id : {HotToggle, HotReset, HotLock, HotQuit}) UnregisterHotKey(window_, id);
            PostQuitMessage(0); return 0;
        }
        return DefWindowProcW(window_, message, wparam, lparam);
    }

    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        auto app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            app = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            app->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        }
        return app ? app->handle(message, wparam, lparam) : DefWindowProcW(window, message, wparam, lparam);
    }

public:
    explicit App(HINSTANCE instance) : instance_(instance) {}
    ~App() { if (font_) DeleteObject(font_); if (hourFont_) DeleteObject(hourFont_); }
    int run() {
        if (!prepareConfigPath()) {
            MessageBoxW(nullptr, L"无法读取或创建配置文件。请检查 %LOCALAPPDATA%\\Countdown 文件夹的访问权限。", L"倒计时", MB_OK | MB_ICONERROR);
            return 1;
        }
        load();
        WNDCLASSEXW klass{sizeof(klass)};
        klass.style = CS_DBLCLKS; klass.lpfnWndProc = windowProc; klass.hInstance = instance_;
        klass.hCursor = LoadCursorW(nullptr, IDC_ARROW); klass.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_APP));
        klass.lpszClassName = windowClass;
        if (!RegisterClassExW(&klass)) return 1;
        MONITORINFO monitor{sizeof(monitor)};
        POINT center{}; GetCursorPos(&center);
        GetMonitorInfoW(MonitorFromPoint(center, MONITOR_DEFAULTTONEAREST), &monitor);
        const int sentinel = std::numeric_limits<int>::min();
        const int x = config_.x == sentinel ? monitor.rcMonitor.left + (monitor.rcMonitor.right - monitor.rcMonitor.left - windowWidth) / 2 : config_.x;
        const int y = config_.y == sentinel ? monitor.rcWork.top + (monitor.rcWork.bottom - monitor.rcWork.top - windowHeight) / 2 : config_.y;
        window_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | (config_.topmost ? WS_EX_TOPMOST : 0), windowClass,
                                 L"倒计时", WS_POPUP, x, y, windowWidth, windowHeight, nullptr, nullptr, instance_, this);
        if (!window_) return 1;
        dpi_ = GetDpiForWindow(window_); makeFont();
        GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST), &monitor);
        const int finalX = config_.x == sentinel ? monitor.rcMonitor.left + (monitor.rcMonitor.right - monitor.rcMonitor.left - width()) / 2 : x;
        keepOnScreen(finalX, y);
        addTray();
        tooltip_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                                  CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, window_, nullptr, instance_, nullptr);
        SendMessageW(tooltip_, TTM_SETMAXTIPWIDTH, 0, scale(340));
        SendMessageW(tooltip_, TTM_SETDELAYTIME, TTDT_INITIAL, 500);
        SendMessageW(tooltip_, TTM_SETDELAYTIME, TTDT_AUTOPOP, 2000);
        updateTooltip();
        const UINT modifiers = MOD_CONTROL | MOD_ALT | MOD_NOREPEAT;
        RegisterHotKey(window_, HotToggle, modifiers, VK_SPACE);
        RegisterHotKey(window_, HotReset, modifiers, 'R');
        RegisterHotKey(window_, HotLock, modifiers, 'L');
        RegisterHotKey(window_, HotQuit, modifiers, 'X');
        draw(); ShowWindow(window_, SW_SHOWNOACTIVATE);
        MSG message{};
        int result;
        while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
        return result == -1 ? 1 : static_cast<int>(message.wParam);
    }
};
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\Countdown.Native.Instance");
    if (!mutex) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (auto window = FindWindowW(windowClass, nullptr)) { ShowWindow(window, SW_SHOWNOACTIVATE); SetForegroundWindow(window); }
        CloseHandle(mutex); return 0;
    }
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int result;
    { App app(instance); result = app.run(); }
    if (SUCCEEDED(com)) CoUninitialize();
    CloseHandle(mutex);
    return result;
}
