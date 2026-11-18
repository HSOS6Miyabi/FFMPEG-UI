#include "ui.hpp"

#include "../core/app_controller.hpp"
#include "../core/process.hpp"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xrender.h>
#include <X11/keysym.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

extern "C" {
struct _XftDraw;
struct _XftFont;
struct XftRenderColor { unsigned short red, green, blue, alpha; };
struct XftColor { unsigned long pixel; XftRenderColor color; };
_XftDraw* XftDrawCreate(Display*, Drawable, Visual*, Colormap);
void XftDrawDestroy(_XftDraw*);
void XftDrawStringUtf8(_XftDraw*, const XftColor*, _XftFont*, int, int, const unsigned char*, int);
void XftTextExtentsUtf8(Display*, _XftFont*, const unsigned char*, int, XGlyphInfo*);
_XftFont* XftFontOpenName(Display*, int, const char*);
void XftFontClose(Display*, _XftFont*);
}

namespace ffmpegui {
namespace {

struct Rect { int x{}, y{}, width{}, height{}; };
bool contains(const Rect& r, int x, int y) {
    return x >= r.x && y >= r.y && x < r.x + r.width && y < r.y + r.height;
}
struct Palette {
    const char* background; const char* surface; const char* card; const char* border;
    const char* text; const char* muted; const char* accent; const char* accentSoft;
    const char* green; const char* red; const char* input; const char* track; const char* white;
};
Palette palette(bool dark) {
    if (dark) return {"#0f131d","#131925","#192130","#2c384c","#eef3fa","#99a6bb",
        "#7e90ff","#2b375b","#2fc197","#f66875","#141c2a","#303c50","#ffffff"};
    return {"#f3f6fb","#fafbfe","#ffffff","#e1e7f0","#1b2538","#69778e",
        "#5065e8","#ebefff","#159b79","#d54355","#f7f9fd","#e7ecf5","#ffffff"};
}
struct Layout {
    Rect addFiles, addFolder, addArchive, importQueue, exportQueue, removeSelected, clearDone;
    Rect outputField, outputBrowse, container[4], codec[5], backend[4], crfTrack, crfValue;
    Rect trimStart, trimEnd, trimOpen, trimReset;
    Rect compressionTrack, compressionValue, audio[6], resolution[6], audioMode[3];
    Rect parallel, recursive, skip, preserve, overwrite;
    Rect queue, overallCard, overallTrack, start, pause, cancel, ssh, theme, advancedToggle;
    Rect focusQueue, failedOnly;
};
std::vector<EncoderBackend> availableBackends(const EncoderCapabilities& capabilities, VideoCodec codec) {
    std::vector<EncoderBackend> result;
    for (const auto backend : {EncoderBackend::CPU, EncoderBackend::NVENC, EncoderBackend::QSV, EncoderBackend::AMF})
        if (capabilities.supports(codec, backend)) result.push_back(backend);
    return result;
}
Layout makeLayout(int width, int height, double scale, int backendCount = 4,
                  bool expanded = false, bool queueFocused = false) {
    const auto s=[scale](int v){return static_cast<int>(std::lround(v*scale));};
    const int pad=s(24),gap=s(14),rightW=std::clamp(width/3,s(330),s(395));
    const int leftW=width-pad*2-gap-rightW,leftX=pad,rightX=leftX+leftW+gap,top=s(112);
    Layout l{};
    const int toolbarGap=s(6),primaryWidth=std::min(s(440),(leftW-s(18))*52/100);
    const int primaryButtonWidth=std::max(s(72),(primaryWidth-toolbarGap*2)/3);
    l.addFiles={leftX,top,primaryButtonWidth,s(42)};
    l.addFolder={leftX+primaryButtonWidth+toolbarGap,top,primaryButtonWidth,s(42)};
    l.addArchive={leftX+(primaryButtonWidth+toolbarGap)*2,top,primaryButtonWidth,s(42)};
    const int secondaryWidth=leftW-primaryWidth-s(18),secondaryInner=std::max(s(4),secondaryWidth-toolbarGap*3);
    const int secondaryBase=secondaryInner/4,secondaryStart=leftX+leftW-secondaryWidth;
    l.importQueue={secondaryStart,top,secondaryBase,s(42)};
    l.exportQueue={secondaryStart+secondaryBase+toolbarGap,top,secondaryBase,s(42)};
    l.removeSelected={secondaryStart+(secondaryBase+toolbarGap)*2,top,secondaryBase,s(42)};
    l.clearDone={secondaryStart+(secondaryBase+toolbarGap)*3,top,secondaryWidth-(secondaryBase+toolbarGap)*3,s(42)};
    const int sy=top+s(56);
    const bool compactAdvanced = expanded && height < s(940);

    const int controlX = leftX + s(132), controlW = leftW - s(154);
    l.advancedToggle = {leftX + leftW - s(146), sy + s(9), s(126), s(28)};
    l.outputField = {controlX, sy + s(47), controlW - s(100), s(28)};
    l.outputBrowse = {controlX + controlW - s(92), sy + s(47), s(92), s(28)};

    const int gridX = leftX + s(18), gridWidth = leftW - s(36);
    const int columnGap = s(18), columnWidth = (gridWidth - columnGap) / 2;
    const int secondX = gridX + columnWidth + columnGap;
    const int buttonGap = s(5);
    const int containerWidth = (columnWidth - buttonGap * 3) / 4;
    for (int index = 0; index < 4; ++index)
        l.container[index] = {gridX + index * (containerWidth + buttonGap),
                              sy + s(112), containerWidth, s(30)};
    const int codecWidth = (columnWidth - buttonGap * 4) / 5;
    for (int index = 0; index < 5; ++index)
        l.codec[index] = {secondX + index * (codecWidth + buttonGap),
                          sy + s(112), codecWidth, s(30)};
    backendCount = std::clamp(backendCount, 1, 4);
    const int backendWidth = (columnWidth - buttonGap * (backendCount - 1)) / backendCount;
    for (int index = 0; index < backendCount; ++index)
        l.backend[index] = {gridX + index * (backendWidth + buttonGap),
                            sy + s(179), backendWidth, s(30)};
    l.crfTrack = {secondX + s(11), sy + s(192), columnWidth - s(111), s(7)};
    l.crfValue = {secondX + columnWidth - s(84), sy + s(179), s(84), s(30)};

    l.compressionTrack = {gridX + s(10), sy + s(compactAdvanced ? 245 : 253), columnWidth - s(20), s(7)};
    l.compressionValue = {gridX + columnWidth - s(92), sy + s(compactAdvanced ? 211 : 219), s(92), s(24)};
    const int audioModeWidth = (columnWidth - buttonGap * 2) / 3;
    for (int index = 0; index < 3; ++index)
        l.audioMode[index] = {secondX + index * (audioModeWidth + buttonGap),
                              sy + s(compactAdvanced ? 232 : 247), audioModeWidth, s(30)};

    const int audioWidth = (columnWidth - buttonGap * 5) / 6;
    for (int index = 0; index < 6; ++index) {
        l.audio[index] = {gridX + index * (audioWidth + buttonGap),
                          sy + s(compactAdvanced ? 298 : 325), audioWidth, s(30)};
        l.resolution[index] = {secondX + index * (audioWidth + buttonGap),
                               sy + s(compactAdvanced ? 298 : 325), audioWidth, s(30)};
    }

    const int optionGap = s(5);
    const int optionWidth = (leftW - s(40) - optionGap * 4) / 5;
    const int optionY = sy + s(compactAdvanced ? 331 : 369);
    l.parallel = {leftX + s(20), optionY, optionWidth, s(28)};
    l.recursive = {leftX + s(20) + optionWidth + optionGap, optionY, optionWidth, s(28)};
    l.skip = {leftX + s(20) + (optionWidth + optionGap) * 2, optionY, optionWidth, s(28)};
    l.preserve = {leftX + s(20) + (optionWidth + optionGap) * 3, optionY, optionWidth, s(28)};
    l.overwrite = {leftX + s(20) + (optionWidth + optionGap) * 4, optionY, optionWidth, s(28)};

    const int trimY = sy + s(expanded ? (compactAdvanced ? 366 : 424) : 232);
    const int trimGap = s(7), openW = s(111), resetW = s(76);
    const int trimW = (gridWidth - trimGap * 3 - openW - resetW) / 2;
    l.trimStart = {gridX, trimY, trimW, s(31)};
    l.trimEnd = {gridX + trimW + trimGap, trimY, trimW, s(31)};
    l.trimOpen = {gridX + (trimW + trimGap) * 2, trimY, openW, s(31)};
    l.trimReset = {gridX + gridWidth - resetW, trimY, resetW, s(31)};
    const int queueY = sy + s(expanded ? (compactAdvanced ? 428 : 486) : 294);
    const int actualQueueY = queueFocused ? sy : queueY;
    l.queue = {leftX, actualQueueY, leftW, std::max(0, height - actualQueueY - pad)};
    l.failedOnly = {leftX + s(142), actualQueueY + s(10), s(102), s(28)};
    const int availableRightHeight = std::max(0, height - top - pad);
    const int statusHeight = std::clamp(availableRightHeight * 55 / 100, s(185), s(270));
    const bool compactStatus = statusHeight < s(260);
    const int actionY = compactStatus ? top + statusHeight - s(48) : top + s(206);
    const int actionHeight = compactStatus ? s(38) : s(44);
    l.overallCard = {rightX, top, rightW, statusHeight};
    l.overallTrack = {rightX + s(22), compactStatus
        ? top + statusHeight - s(90) : top + s(126), rightW - s(44), s(12)};
    l.start = {rightX + s(22), actionY, rightW - s(44), actionHeight};
    const int actionGap = s(8);
    const int actionWidth = (rightW - s(52) - actionGap) / 2;
    l.pause = {rightX + s(22), actionY, actionWidth, actionHeight};
    l.cancel = {rightX + s(22) + actionWidth + actionGap, actionY,
                rightW - s(44) - actionWidth - actionGap, actionHeight};
    l.focusQueue = {width - s(377), s(28), s(135), s(36)};
    l.ssh={width-s(224),s(28),s(100),s(36)};
    l.theme={width-s(112),s(28),s(82),s(36)};
    return l;
}

class XftCanvas {
public:
    XftCanvas(Display* display, Window window, double scale)
        : display_(display),
          window_(window),
          screen_(DefaultScreen(display)),
          colormap_(DefaultColormap(display, screen_)),
          visual_(DefaultVisual(display, screen_)) {
        draw_ = XftDrawCreate(display_, window_, visual_, colormap_);

        const int normalSize = std::max(
            13, static_cast<int>(std::lround(14 * scale)));
        const int smallSize = std::max(
            11, static_cast<int>(std::lround(12 * scale)));
        const int titleSize = std::max(
            20, static_cast<int>(std::lround(25 * scale)));

        const std::string normalPattern =
            "Noto Sans CJK SC:pixelsize=" + std::to_string(normalSize);
        const std::string smallPattern =
            "Noto Sans CJK SC:pixelsize=" + std::to_string(smallSize);
        const std::string titlePattern =
            "Noto Sans CJK SC:style=Bold:pixelsize=" + std::to_string(titleSize);

        normal_ = XftFontOpenName(display_, screen_, normalPattern.c_str());
        small_ = XftFontOpenName(display_, screen_, smallPattern.c_str());
        title_ = XftFontOpenName(display_, screen_, titlePattern.c_str());

        if (normal_ == nullptr) {
            normal_ = XftFontOpenName(display_, screen_, "sans:pixelsize=14");
        }
        if (small_ == nullptr) {
            small_ = normal_;
        }
        if (title_ == nullptr) {
            title_ = normal_;
        }
    }

    ~XftCanvas() {
        if (draw_ != nullptr) {
            XftDrawDestroy(draw_);
        }
        if (gc_ != nullptr) {
            XFreeGC(display_, gc_);
        }
        if (title_ != nullptr && title_ != normal_ && title_ != small_) {
            XftFontClose(display_, title_);
        }
        if (small_ != nullptr && small_ != normal_) {
            XftFontClose(display_, small_);
        }
        if (normal_ != nullptr) {
            XftFontClose(display_, normal_);
        }
    }

    void fill(Rect rectangle, const char* color) {
        XSetForeground(display_, gc(), pixel(color));
        XFillRectangle(
            display_, window_, gc(),
            rectangle.x, rectangle.y,
            static_cast<unsigned int>(std::max(0, rectangle.width)),
            static_cast<unsigned int>(std::max(0, rectangle.height)));
    }

    void line(int x1, int y1, int x2, int y2, const char* color) {
        XSetForeground(display_, gc(), pixel(color));
        XDrawLine(display_, window_, gc(), x1, y1, x2, y2);
    }

    void rounded(Rect rectangle, const char* color, int radius = 9) {
        if (rectangle.width <= 0 || rectangle.height <= 0) {
            return;
        }

        const int diameter = radius * 2;
        XSetForeground(display_, gc(), pixel(color));
        XFillRectangle(
            display_, window_, gc(),
            rectangle.x + radius, rectangle.y,
            static_cast<unsigned int>(std::max(0, rectangle.width - diameter)),
            static_cast<unsigned int>(rectangle.height));
        XFillRectangle(
            display_, window_, gc(),
            rectangle.x, rectangle.y + radius,
            static_cast<unsigned int>(rectangle.width),
            static_cast<unsigned int>(std::max(0, rectangle.height - diameter)));

        XFillArc(display_, window_, gc(),
                 rectangle.x, rectangle.y, diameter, diameter, 0, 360 * 64);
        XFillArc(display_, window_, gc(),
                 rectangle.x + rectangle.width - diameter, rectangle.y,
                 diameter, diameter, 0, 360 * 64);
        XFillArc(display_, window_, gc(),
                 rectangle.x, rectangle.y + rectangle.height - diameter,
                 diameter, diameter, 0, 360 * 64);
        XFillArc(display_, window_, gc(),
                 rectangle.x + rectangle.width - diameter,
                 rectangle.y + rectangle.height - diameter,
                 diameter, diameter, 0, 360 * 64);
    }

    void card(Rect rectangle, const Palette& colors) {
        rounded(
            {rectangle.x, rectangle.y + 2, rectangle.width, rectangle.height},
            colors.border, 12);
        rounded(rectangle, colors.card, 12);
    }

    void text(int x, int baseline, const std::string& value,
              const char* color, int size = 1) {
        XColor parsedColor{};
        XParseColor(display_, colormap_, color, &parsedColor);

        XftColor foreground{};
        foreground.pixel = pixel(color);
        foreground.color = {
            parsedColor.red, parsedColor.green, parsedColor.blue, 65535
        };

        _XftFont* font = size == 0 ? small_ : size == 2 ? title_ : normal_;
        if (font == nullptr) {
            return;
        }

        XftDrawStringUtf8(
            draw_, &foreground, font, x, baseline,
            reinterpret_cast<const unsigned char*>(value.data()),
            static_cast<int>(value.size()));
    }

    int textWidth(const std::string& value, int size = 0) const {
        _XftFont* currentFont = size == 0 ? small_ : size == 2 ? title_ : normal_;
        if (currentFont == nullptr || value.empty()) return 0;
        XGlyphInfo metrics{};
        XftTextExtentsUtf8(display_, currentFont,
                           reinterpret_cast<const unsigned char*>(value.data()),
                           static_cast<int>(value.size()), &metrics);
        return static_cast<int>(metrics.xOff);
    }

    void textFit(int x, int baseline, const std::string& value, int maxWidth,
                 const char* color, int size = 0) {
        if (maxWidth <= 0) return;
        if (textWidth(value, size) <= maxWidth) {
            text(x, baseline, value, color, size);
            return;
        }
        const std::string ellipsis = "…";
        if (textWidth(ellipsis, size) > maxWidth) return;
        std::vector<std::size_t> offsets{0};
        for (std::size_t pos = 0; pos < value.size();) {
            const auto byte = static_cast<unsigned char>(value[pos]);
            const std::size_t width = (byte & 0xf8) == 0xf0 ? 4 :
                                      (byte & 0xf0) == 0xe0 ? 3 :
                                      (byte & 0xe0) == 0xc0 ? 2 : 1;
            pos = std::min(value.size(), pos + width);
            offsets.push_back(pos);
        }
        std::size_t lo = 0, hi = offsets.size() - 1;
        while (lo < hi) {
            const std::size_t mid = lo + (hi - lo + 1) / 2;
            if (textWidth(value.substr(0, offsets[mid]) + ellipsis, size) <= maxWidth)
                lo = mid;
            else
                hi = mid - 1;
        }
        text(x, baseline, value.substr(0, offsets[lo]) + ellipsis, color, size);
    }

    void button(Rect rectangle, const std::string& label,
                const Palette& colors, bool active = false, bool disabled = false) {
        const char* background =
            disabled ? colors.track : active ? colors.accent : colors.input;
        rounded(rectangle, background, 8);

        if (!active && !disabled) {
            line(
                rectangle.x + 8, rectangle.y + rectangle.height - 1,
                rectangle.x + rectangle.width - 8, rectangle.y + rectangle.height - 1,
                colors.border);
        }

        const int measuredWidth = textWidth(label, 0);
        const int availableWidth = std::max(0, rectangle.width - 16);
        const int textX = rectangle.x + std::max(8, (rectangle.width - measuredWidth) / 2);
        const int textY = rectangle.y + rectangle.height / 2 + 5;
        textFit(measuredWidth <= availableWidth ? textX : rectangle.x + 8,
                textY, label, availableWidth,
                active && !disabled ? colors.white : colors.text, 0);
    }

    void input(Rect rectangle, const std::string& value,
               const Palette& colors, bool focused = false, bool secret = false) {
        rounded(rectangle, colors.input, 8);

        const char* borderColor = focused ? colors.accent : colors.border;
        line(
            rectangle.x + 7, rectangle.y,
            rectangle.x + rectangle.width - 7, rectangle.y,
            borderColor);
        line(
            rectangle.x + 7, rectangle.y + rectangle.height - 1,
            rectangle.x + rectangle.width - 7, rectangle.y + rectangle.height - 1,
            borderColor);
        line(
            rectangle.x, rectangle.y + 7,
            rectangle.x, rectangle.y + rectangle.height - 7,
            borderColor);
        line(
            rectangle.x + rectangle.width - 1, rectangle.y + 7,
            rectangle.x + rectangle.width - 1, rectangle.y + rectangle.height - 7,
            borderColor);

        const std::string displayValue =
            secret ? std::string(value.size(), '*') : value;
        text(
            rectangle.x + 10, rectangle.y + rectangle.height / 2 + 5,
            displayValue, colors.text, 0);
    }

    void progress(Rect rectangle, double value, const Palette& colors) {
        rounded(rectangle, colors.track, 5);
        const int filledWidth = static_cast<int>(
            std::lround(rectangle.width * std::clamp(value, 0.0, 1.0)));
        if (filledWidth > 0) {
            rounded(
                {rectangle.x, rectangle.y, filledWidth, rectangle.height},
                colors.accent, 5);
        }
    }

    void image(Rect rectangle, const PreviewFrame& frame) {
        if (frame.width < 1 || frame.height < 1 ||
            frame.rgb.size() != static_cast<std::size_t>(frame.width * frame.height * 3))
            return;
        const int depth = DefaultDepth(display_, screen_);
        XImage* ximage = XCreateImage(display_, visual_, depth, ZPixmap,
                                      0, nullptr, frame.width, frame.height, 32, 0);
        if (ximage == nullptr) return;
        const std::size_t bytes = static_cast<std::size_t>(ximage->bytes_per_line) *
                                  static_cast<std::size_t>(frame.height);
        ximage->data = static_cast<char*>(std::calloc(bytes, 1));
        if (!ximage->data) {
            XDestroyImage(ximage);
            return;
        }
        const auto maskPixel = [](unsigned char sample, unsigned long mask) {
            if (mask == 0) return 0UL;
            unsigned int shift = 0;
            while (((mask >> shift) & 1UL) == 0UL) ++shift;
            const unsigned long maximum = mask >> shift;
            return ((static_cast<unsigned long>(sample) * maximum + 127UL) / 255UL << shift) & mask;
        };
        for (int y = 0; y < frame.height; ++y) {
            for (int x = 0; x < frame.width; ++x) {
                const std::size_t index = static_cast<std::size_t>((y * frame.width + x) * 3);
                const unsigned long pixelValue =
                    maskPixel(frame.rgb[index], ximage->red_mask) |
                    maskPixel(frame.rgb[index + 1], ximage->green_mask) |
                    maskPixel(frame.rgb[index + 2], ximage->blue_mask);
                XPutPixel(ximage, x, y, pixelValue);
            }
        }
        XPutImage(display_, window_, gc(), ximage, 0, 0, rectangle.x, rectangle.y,
                  static_cast<unsigned int>(frame.width),
                  static_cast<unsigned int>(frame.height));
        XDestroyImage(ximage);
    }

    void flush() {
        XFlush(display_);
    }

private:
    GC gc() {
        if (gc_ == nullptr) {
            gc_ = XCreateGC(display_, window_, 0, nullptr);
        }
        return gc_;
    }

    unsigned long pixel(const char* name) {
        const auto cached = pixels_.find(name);
        if (cached != pixels_.end()) {
            return cached->second;
        }

        XColor color{};
        XParseColor(display_, colormap_, name, &color);
        XAllocColor(display_, colormap_, &color);
        pixels_[name] = color.pixel;
        return color.pixel;
    }

    Display* display_{};
    Window window_{};
    int screen_{};
    Colormap colormap_{};
    Visual* visual_{};
    _XftDraw* draw_{};
    _XftFont* normal_{};
    _XftFont* small_{};
    _XftFont* title_{};
    GC gc_{};
    std::map<std::string, unsigned long> pixels_;
};

std::string basename(const std::string& path) {
    return std::filesystem::path(path).filename().string();
}

std::string formatTime(const std::optional<double>& value) {
    if (!value || *value < 0) {
        return "--:--:--";
    }

    const auto totalSeconds = static_cast<unsigned long long>(*value);
    char buffer[32]{};
    std::snprintf(
        buffer, sizeof(buffer), "%02llu:%02llu:%02llu",
        totalSeconds / 3600,
        (totalSeconds % 3600) / 60,
        totalSeconds % 60);
    return buffer;
}

std::string formatTaskSize(std::uint64_t inputBytes, std::uint64_t outputBytes) {
    if (inputBytes == 0) {
        return {};
    }

    const auto formatBytes = [](std::uint64_t value) {
        char buffer[40]{};
        const double bytes = static_cast<double>(value);

        if (bytes >= 1024.0 * 1024.0 * 1024.0) {
            std::snprintf(
                buffer, sizeof(buffer), "%.2f GiB",
                bytes / (1024.0 * 1024.0 * 1024.0));
        } else if (bytes >= 1024.0 * 1024.0) {
            std::snprintf(
                buffer, sizeof(buffer), "%.1f MiB",
                bytes / (1024.0 * 1024.0));
        } else {
            std::snprintf(buffer, sizeof(buffer), "%.0f KiB", bytes / 1024.0);
        }

        return std::string(buffer);
    };

    std::string result = "输入 " + formatBytes(inputBytes);
    if (outputBytes > 0) {
        result += " → 输出 " + formatBytes(outputBytes);

        if (inputBytes > outputBytes) {
            const double savedPercent =
                (1.0 - static_cast<double>(outputBytes) /
                           static_cast<double>(inputBytes)) * 100.0;
            char buffer[32]{};
            std::snprintf(buffer, sizeof(buffer), " · 节省 %.1f%%", savedPercent);
            result += buffer;
        }
    }

    if (result.size() > 136) {
        result.resize(133);
        result += "...";
    }
    return result;
}

std::string formatProgressLine(const ConversionProgress& progress,
                               const std::string& fallback) {
    if (!progress.frame &&
        !progress.currentTimeSeconds &&
        !progress.fps &&
        !progress.speed &&
        !progress.bitrate &&
        !progress.totalSizeBytes) {
        return fallback;
    }

    std::string result;
    const auto append = [&](const std::string& value) {
        if (!result.empty()) {
            result += " · ";
        }
        result += value;
    };

    if (progress.frame) {
        append("帧 " + std::to_string(*progress.frame));
    }
    if (progress.currentTimeSeconds || progress.durationSeconds) {
        append(
            formatTime(progress.currentTimeSeconds) + " / " +
            formatTime(progress.durationSeconds));
    }
    if (progress.fps) {
        char buffer[32]{};
        std::snprintf(buffer, sizeof(buffer), "%.1f FPS", *progress.fps);
        append(buffer);
    }
    if (progress.speed) {
        char buffer[32]{};
        std::snprintf(buffer, sizeof(buffer), "%.2fx", *progress.speed);
        append(buffer);
    }
    if (progress.etaSeconds) {
        append("剩余 " + formatTime(progress.etaSeconds));
    }
    if (progress.bitrate) {
        append("码率 " + *progress.bitrate);
    }
    if (progress.totalSizeBytes) {
        append("已写入 " + std::to_string(*progress.totalSizeBytes / 1024) + " KiB");
    }

    if (result.size() > 136) {
        result.resize(133);
        result += "...";
    }
    return result;
}

std::string trim(const std::string& value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }

    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string percentDecode(const std::string& value) {
    std::string result;
    result.reserve(value.size());

    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] == '%' && index + 2 < value.size()) {
            try {
                const int decoded = std::stoi(value.substr(index + 1, 2), nullptr, 16);
                result.push_back(static_cast<char>(decoded));
                index += 2;
            } catch (const std::exception&) {
                result.push_back(value[index]);
            }
        } else {
            result.push_back(value[index]);
        }
    }

    return result;
}

class LinuxWindow {
public:
    explicit LinuxWindow(AppController& controller) : controller_(controller),
        settings_(controller.snapshot().settings) {}
    int run() {
        display_ = XOpenDisplay(nullptr);
        if (display_ == nullptr) {
            return 1;
        }

        screen_ = DefaultScreen(display_);
        scale_ = detectScale();
        width_ = static_cast<int>(1500 * scale_);
        height_ = static_cast<int>(1020 * scale_);

        window_ = XCreateSimpleWindow(
            display_, RootWindow(display_, screen_),
            0, 0, static_cast<unsigned int>(width_),
            static_cast<unsigned int>(height_),
            0, BlackPixel(display_, screen_), WhitePixel(display_, screen_));
        const std::string windowTitle = "视频压缩工作台";
        XStoreName(display_, window_, windowTitle.c_str());
        const Atom utf8String = XInternAtom(display_, "UTF8_STRING", False);
        const Atom netWmName = XInternAtom(display_, "_NET_WM_NAME", False);
        XChangeProperty(display_, window_, netWmName, utf8String, 8, PropModeReplace,
                        reinterpret_cast<const unsigned char*>(windowTitle.data()),
                        static_cast<int>(windowTitle.size()));

        XClassHint classHint{};
        classHint.res_name = const_cast<char*>("video-compression-workbench");
        classHint.res_class = const_cast<char*>("VideoCompressionWorkbench");
        XSetClassHint(display_, window_, &classHint);

        XSelectInput(
            display_, window_,
            ExposureMask | StructureNotifyMask | ButtonPressMask |
            ButtonReleaseMask | PointerMotionMask | KeyPressMask | PropertyChangeMask);

        wmDelete_ = XInternAtom(display_, "WM_DELETE_WINDOW", False);
        XSetWMProtocols(display_, window_, &wmDelete_, 1);
        setDnd();

        XSizeHints sizeHints{};
        sizeHints.flags = PMinSize;
        sizeHints.min_width = static_cast<int>(1180 * scale_);
        sizeHints.min_height = static_cast<int>(820 * scale_);
        XSetWMNormalHints(display_, window_, &sizeHints);

        XMapWindow(display_, window_);
        canvas_ = std::make_unique<XftCanvas>(display_, window_, scale_);

        bool quit = false;
        while (!quit) {
            while (XPending(display_)) {
                XEvent event{};
                XNextEvent(display_, &event);
                if (handle(event)) {
                    quit = true;
                }
            }

            pollfd descriptor{ConnectionNumber(display_), POLLIN, 0};
            poll(&descriptor, 1, 120);
            if ((descriptor.revents & POLLIN) != 0) {
                continue;
            }
            draw();
        }

        controller_.shutdown();
        canvas_.reset();
        XDestroyWindow(display_, window_);
        XCloseDisplay(display_);
        return 0;
    }

private:
    double detectScale() {
        double dpi = 96.0;
        const char* resources = XResourceManagerString(display_);
        if (resources != nullptr) {
            const std::string resourceText(resources);
            const auto position = resourceText.find("Xft.dpi:");
            if (position != std::string::npos) {
                try {
                    dpi = std::stod(resourceText.substr(position + 8));
                } catch (const std::exception&) {
                    // Use the standard DPI when the X resource value is invalid.
                }
            }
        }
        return std::clamp(dpi / 96.0, 0.8, 2.5);
    }

    int s(int value) const {
        return static_cast<int>(std::lround(value * scale_));
    }

    void setDnd() {
        const Atom aware = XInternAtom(display_, "XdndAware", False);
        unsigned long version = 5;
        XChangeProperty(
            display_, window_, aware, XA_ATOM, 32, PropModeReplace,
            reinterpret_cast<unsigned char*>(&version), 1);

        xdndEnter_ = XInternAtom(display_, "XdndEnter", False);
        xdndPosition_ = XInternAtom(display_, "XdndPosition", False);
        xdndStatus_ = XInternAtom(display_, "XdndStatus", False);
        xdndDrop_ = XInternAtom(display_, "XdndDrop", False);
        xdndFinished_ = XInternAtom(display_, "XdndFinished", False);
        xdndSelection_ = XInternAtom(display_, "XdndSelection", False);
        uriList_ = XInternAtom(display_, "text/uri-list", False);
    }

    std::vector<std::string> chooseFiles(bool archiveOnly = false) {
        const std::string filter = archiveOnly
            ? "--file-filter=ZIP and PowerPoint | *.zip *.pptx"
            : "--file-filter=Media and archives | "
              "*.mp4 *.m4v *.mov *.mkv *.avi *.webm *.wmv *.flv "
              "*.mpg *.mpeg *.3gp *.ts *.m2ts *.mts *.zip *.pptx";

        ProcessResult result = runProcess({
            "zenity", "--file-selection", "--multiple", "--separator=\n",
            "--title=添加媒体文件", filter
        });
        if (result.exitCode != 0) {
            const std::string fallbackFilter = archiveOnly
                ? "*.zip *.pptx"
                : "*.mp4 *.mkv *.mov *.avi *.webm *.zip *.pptx";
            result = runProcess({
                "kdialog", "--getopenfilename", ".", fallbackFilter,
                "--multiple", "--separate-output"
            });
        }

        std::vector<std::string> paths;
        std::istringstream input(result.output);
        std::string line;
        while (std::getline(input, line)) {
            line = trim(line);
            if (!line.empty()) {
                paths.push_back(std::move(line));
            }
        }
        return paths;
    }

    void importQueueDialog() {
        ProcessResult result = runProcess({
            "zenity", "--file-selection", "--title=导入任务清单",
            "--file-filter=视频压缩工作台任务清单 | *.ffq *.txt"
        });
        if (result.exitCode != 0) {
            result = runProcess({
                "kdialog", "--getopenfilename", ".", "*.ffq *.txt", "导入任务清单"
            });
        }

        const std::string path = trim(result.output);
        if (path.empty()) {
            return;
        }

        std::string error;
        const std::size_t added = controller_.importQueue(path, error);
        if (!error.empty()) {
            std::cerr << error << '\n';
        } else if (added == 0) {
            std::cerr << "任务清单中没有新增任务。\n";
        }
    }

    void exportQueueDialog() {
        ProcessResult result = runProcess({
            "zenity", "--file-selection", "--save", "--confirm-overwrite",
            "--title=导出任务清单", "--file-filter=视频压缩工作台任务清单 | *.ffq"
        });
        if (result.exitCode != 0) {
            result = runProcess({
                "kdialog", "--getsavefilename", ".", "*.ffq", "导出任务清单"
            });
        }

        std::string path = trim(result.output);
        if (path.empty()) {
            return;
        }
        if (std::filesystem::path(path).extension().empty()) {
            path += ".ffq";
        }

        std::string error;
        if (!controller_.exportQueue(path, error)) {
            std::cerr << error << '\n';
        }
    }

    void chooseFolder() {
        ProcessResult result = runProcess({
            "zenity", "--file-selection", "--directory", "--title=选择输入文件夹"
        });
        if (result.exitCode != 0) {
            result = runProcess({
                "kdialog", "--getexistingdirectory", ".", "选择输入文件夹"
            });
        }

        const std::string path = trim(result.output);
        if (!path.empty()) {
            controller_.addInputs({path});
        }
    }

    void chooseOutput() {
        ProcessResult result = runProcess({
            "zenity", "--file-selection", "--directory", "--title=选择输出文件夹"
        });
        if (result.exitCode != 0) {
            result = runProcess({
                "kdialog", "--getexistingdirectory", ".", "选择输出文件夹"
            });
        }

        const std::string path = trim(result.output);
        if (!path.empty()) {
            settings_.outputDirectory = path;
            updateSettings();
        }
    }

    void updateSettings() {
        controller_.updateSettings(settings_);
    }

    void manageSshServers() {
        const int dialogWidth = s(980);
        const int dialogHeight = s(700);

        XWindowAttributes parentAttributes{};
        XGetWindowAttributes(display_, window_, &parentAttributes);
        const int dialogX = std::max(0, (parentAttributes.width - dialogWidth) / 2);
        const int dialogY = std::max(0, (parentAttributes.height - dialogHeight) / 2);

        const Window dialog = XCreateSimpleWindow(
            display_, window_, dialogX, dialogY,
            static_cast<unsigned int>(dialogWidth),
            static_cast<unsigned int>(dialogHeight),
            1, BlackPixel(display_, screen_), WhitePixel(display_, screen_));
        XStoreName(display_, dialog, "SSH 服务器管理");
        XSetTransientForHint(display_, dialog, window_);

        XSizeHints sizeHints{};
        sizeHints.flags = PMinSize;
        sizeHints.min_width = dialogWidth;
        sizeHints.min_height = dialogHeight;
        XSetWMNormalHints(display_, dialog, &sizeHints);

        const Atom deleteMessage = XInternAtom(display_, "WM_DELETE_WINDOW", False);
        XSetWMProtocols(display_, dialog, const_cast<Atom*>(&deleteMessage), 1);
        XSelectInput(
            display_, dialog,
            ExposureMask | StructureNotifyMask | ButtonPressMask | KeyPressMask);
        XMapRaised(display_, dialog);

        auto canvas = std::make_unique<XftCanvas>(display_, dialog, scale_);
        const Palette colors = palette(dark_);
        const auto scaled = [&](int value) {
            return s(value);
        };

        const Rect listCard{
            scaled(20), scaled(70), scaled(260), scaled(500)
        };
        const std::array<Rect, 7> fields{{
            {scaled(320), scaled(92), scaled(280), scaled(36)},
            {scaled(630), scaled(92), scaled(320), scaled(36)},
            {scaled(320), scaled(172), scaled(280), scaled(36)},
            {scaled(630), scaled(172), scaled(130), scaled(36)},
            {scaled(320), scaled(252), scaled(630), scaled(36)},
            {scaled(320), scaled(332), scaled(630), scaled(36)},
            {scaled(320), scaled(412), scaled(630), scaled(36)}
        }};
        const Rect authenticationButton{
            scaled(320), scaled(492), scaled(250), scaled(36)
        };

        std::array<std::string, 7> values{};
        int focusedField = 0;
        bool passwordAuthentication = false;
        int selectedServer = -1;
        std::string status = "服务器列表来自本机 SSH 配置文件。";
        std::vector<SshServer> servers = controller_.sshServers();

        const auto clearFields = [&]() {
            values = {};
            values[3] = "22";
            passwordAuthentication = false;
            focusedField = 0;
            selectedServer = -1;
        };

        const auto populateFields = [&](int index) {
            if (index < 0 || index >= static_cast<int>(servers.size())) {
                return;
            }

            selectedServer = index;
            const auto& server = servers[static_cast<std::size_t>(index)];
            values[0] = server.alias;
            values[1] = server.hostName;
            values[2] = server.user;
            values[3] = std::to_string(server.port);
            values[4] = server.identityFile;
            values[5] = server.proxyJump;
            values[6].clear();

            passwordAuthentication = server.authMethod == SshAuthMethod::Password;
            focusedField = -1;
        };

        const auto makeServer = [&]() {
            SshServer server;
            server.alias = trim(values[0]);
            server.hostName = trim(values[1]);
            server.user = trim(values[2]);

            try {
                const auto port = std::stoul(values[3].empty() ? "22" : values[3]);
                server.port = port <= 65535 ? static_cast<unsigned int>(port) : 0;
            } catch (const std::exception&) {
                server.port = 0;
            }

            server.identityFile = trim(values[4]);
            server.proxyJump = trim(values[5]);
            server.authMethod = passwordAuthentication
                ? SshAuthMethod::Password
                : SshAuthMethod::PublicKey;
            return server;
        };

        const auto refreshServers = [&]() {
            servers = controller_.sshServers();
        };

        const auto saveServer = [&]() {
            const SshServer server = makeServer();
            std::string error;
            if (!controller_.saveSshServer(server, values[6], error)) {
                status = error;
                return false;
            }

            refreshServers();
            const auto savedServer = std::find_if(
                servers.begin(), servers.end(),
                [&](const SshServer& item) {
                    return item.alias == server.alias;
                });
            if (savedServer != servers.end()) {
                populateFields(static_cast<int>(
                    std::distance(servers.begin(), savedServer)));
            }

            status = "SSH 配置已保存，原有的其他 SSH 选项已保留。";
            return true;
        };

        const auto drawDialog = [&]() {
            canvas->fill(
                {0, 0, dialogWidth, dialogHeight}, colors.background);
            canvas->text(
                scaled(24), scaled(36), "SSH 服务器", colors.text, 2);
            canvas->text(
                scaled(24), scaled(57),
                "选择现有配置，或添加服务器。密码只保存在安全凭据存储中。",
                colors.muted, 0);

            canvas->card(listCard, colors);
            canvas->text(
                scaled(38), scaled(96), "本机 SSH 配置", colors.text, 1);

            if (servers.empty()) {
                canvas->text(
                    scaled(38), scaled(145),
                    "没有可用的服务器配置。", colors.muted, 0);
            }

            for (std::size_t index = 0; index < servers.size(); ++index) {
                const int rowY = scaled(112) +
                    static_cast<int>(index) * scaled(54);
                if (rowY + scaled(48) > listCard.y + listCard.height - scaled(8)) {
                    break;
                }

                const Rect row{
                    scaled(30), rowY, scaled(240), scaled(46)
                };
                const bool selected = static_cast<int>(index) == selectedServer;
                if (selected) {
                    canvas->rounded(row, colors.accentSoft, 7);
                }

                const auto& server = servers[index];
                canvas->text(
                    row.x + scaled(10), row.y + scaled(19),
                    server.alias, selected ? colors.accent : colors.text, 0);
                canvas->text(
                    row.x + scaled(10), row.y + scaled(37),
                    server.user + "@" + server.hostName,
                    colors.muted, 0);
            }

            const std::array<std::string, 7> fieldLabels{
                "服务器别名",
                "主机名或 IP 地址",
                "用户名",
                "SSH 端口",
                "私钥文件（可留空以使用 SSH Agent）",
                "跳转服务器（可选）",
                "密码（仅密码认证时填写）"
            };

            for (std::size_t index = 0; index < 6; ++index) {
                const Rect& field = fields[index];
                canvas->text(
                    field.x, field.y - scaled(10),
                    fieldLabels[index], colors.muted, 0);
                canvas->input(
                    field, values[index], colors,
                    focusedField == static_cast<int>(index));
            }

            if (passwordAuthentication) {
                const Rect& passwordField = fields[6];
                canvas->text(
                    passwordField.x, passwordField.y - scaled(10),
                    fieldLabels[6], colors.muted, 0);
                canvas->input(
                    passwordField, values[6], colors,
                    focusedField == 6, true);
            }

            canvas->text(
                authenticationButton.x,
                authenticationButton.y - scaled(10),
                "认证方式", colors.muted, 0);
            canvas->button(
                authenticationButton,
                passwordAuthentication ? "密码认证" : "SSH 私钥 / Agent 认证",
                colors, passwordAuthentication);

            const int actionY = scaled(590);
            const int buttonWidth = scaled(112);
            const int buttonGap = scaled(8);
            const std::array<std::string, 7> actionLabels{
                "新增", "保存并同步", "测试连接", "使用此服务器",
                "删除", "本机编码", "关闭"
            };

            for (std::size_t index = 0; index < actionLabels.size(); ++index) {
                const Rect button{
                    scaled(20) + static_cast<int>(index) * (buttonWidth + buttonGap),
                    actionY,
                    buttonWidth,
                    scaled(42)
                };
                canvas->button(button, actionLabels[index], colors, index == 3);
            }

            canvas->text(scaled(24), scaled(670), status, colors.muted, 0);
            canvas->flush();
        };

        clearFields();
        drawDialog();

        bool done = false;
        while (!done) {
            if (!XPending(display_)) {
                pollfd descriptor{ConnectionNumber(display_), POLLIN, 0};
                poll(&descriptor, 1, 120);
                if ((descriptor.revents & POLLIN) == 0) {
                    continue;
                }
            }

            XEvent event{};
            XNextEvent(display_, &event);

            if (event.type == Expose) {
                drawDialog();
                continue;
            }
            if (event.type == ClientMessage &&
                static_cast<Atom>(event.xclient.data.l[0]) == deleteMessage) {
                done = true;
                continue;
            }

            if (event.type == ButtonPress) {
                const int x = event.xbutton.x;
                const int y = event.xbutton.y;

                if (contains(listCard, x, y)) {
                    const int index = (y - scaled(112)) / scaled(54);
                    if (index >= 0 && index < static_cast<int>(servers.size())) {
                        populateFields(index);
                    }
                } else {
                    for (std::size_t index = 0; index < fields.size(); ++index) {
                        if (contains(fields[index], x, y)) {
                            focusedField = static_cast<int>(index);
                        }
                    }

                    if (contains(authenticationButton, x, y)) {
                        passwordAuthentication = !passwordAuthentication;
                        values[6].clear();
                        focusedField = passwordAuthentication ? 6 : -1;
                    }

                    const int actionY = scaled(590);
                    const int buttonWidth = scaled(112);
                    const int buttonGap = scaled(8);
                    int action = -1;

                    for (int index = 0; index < 7; ++index) {
                        const Rect button{
                            scaled(20) + index * (buttonWidth + buttonGap),
                            actionY,
                            buttonWidth,
                            scaled(42)
                        };
                        if (contains(button, x, y)) {
                            action = index;
                            break;
                        }
                    }

                    if (action == 0) {
                        clearFields();
                        status = "填写服务器信息，然后保存。";
                    } else if (action == 1) {
                        (void)saveServer();
                    } else if (action == 2) {
                        if (saveServer()) {
                            std::string error;
                            (void)controller_.testSshServer(values[0], error);
                            status = error;
                        }
                    } else if (action == 3) {
                        if (saveServer()) {
                            std::string error;
                            if (controller_.selectRemoteServer(values[0], error)) {
                                settings_ = controller_.snapshot().settings;
                                done = true;
                            } else {
                                status = error;
                            }
                        }
                    } else if (action == 4) {
                        if (selectedServer >= 0 &&
                            selectedServer < static_cast<int>(servers.size())) {
                            std::string error;
                            const std::string alias =
                                servers[static_cast<std::size_t>(selectedServer)].alias;
                            if (controller_.removeSshServer(alias, error)) {
                                refreshServers();
                                clearFields();
                                status = "服务器已删除，SSH 配置已同步更新。";
                            } else {
                                status = error;
                            }
                        } else {
                            status = "请先选择要删除的服务器。";
                        }
                    } else if (action == 5) {
                        controller_.selectLocalEncoder();
                        settings_ = controller_.snapshot().settings;
                        done = true;
                    } else if (action == 6) {
                        done = true;
                    }
                }

                drawDialog();
                continue;
            }

            if (event.type == KeyPress) {
                KeySym key{};
                char buffer[64]{};
                const int count = XLookupString(
                    &event.xkey, buffer, sizeof(buffer), &key, nullptr);

                if (key == XK_Escape) {
                    done = true;
                    continue;
                }
                if (key == XK_Tab) {
                    focusedField = (focusedField + 1) % 7;
                    drawDialog();
                    continue;
                }

                if (focusedField >= 0 &&
                    focusedField < static_cast<int>(values.size())) {
                    auto& value = values[static_cast<std::size_t>(focusedField)];

                    if (key == XK_BackSpace || key == XK_Delete) {
                        if (!value.empty()) {
                            value.pop_back();
                        }
                    } else if (key == XK_Return || key == XK_KP_Enter) {
                        focusedField = focusedField < 6 ? focusedField + 1 : -1;
                    } else if (count > 0 &&
                               buffer[0] >= 32 && buffer[0] != 127 &&
                               value.size() < 1024) {
                        value.append(buffer, static_cast<std::size_t>(count));
                    }

                    drawDialog();
                }
            }
        }

        canvas.reset();
        XDestroyWindow(display_, dialog);
        XFlush(display_);
    }

    void commitCrfText() {
        if (!editingCrf_) {
            return;
        }

        if (!crfEditText_.empty()) {
            try {
                settings_.crf = std::clamp(
                    std::stoi(crfEditText_), 0, crfMaximum(settings_.codec));
                updateSettings();
            } catch (const std::exception&) {
                // Invalid text is discarded; the last valid CRF value remains active.
            }
        }

        editingCrf_ = false;
        crfEditText_.clear();
    }

    void openVideoTrimmer() {
        const auto snapshot = controller_.snapshot();
        if (snapshot.running) return;
        const auto selected = std::find_if(snapshot.tasks.begin(), snapshot.tasks.end(),
            [this](const BatchTaskSnapshot& task) {
                return task.kind == TaskKind::Video && task.id == selectedTask_;
            });
        const auto candidate = selected == snapshot.tasks.end()
            ? std::find_if(snapshot.tasks.begin(), snapshot.tasks.end(),
                [](const BatchTaskSnapshot& task) { return task.kind == TaskKind::Video; })
            : selected;
        if (candidate == snapshot.tasks.end()) {
            trimError_ = "请先添加独立视频；ZIP 和 PPTX 任务可以直接填写裁剪时间。";
            return;
        }
        const auto sourceDuration = controller_.videoDuration(candidate->inputPath);
        if (!sourceDuration || *sourceDuration <= 0.0) {
            trimError_ = "无法读取视频时长。请检查输入文件和 ffprobe。";
            return;
        }
        const std::string source = candidate->inputPath;
        const double duration = *sourceDuration;
        double start = settings_.trimStartSeconds < duration ? settings_.trimStartSeconds : 0.0;
        double end = settings_.trimEndSeconds < 0.0 ? -1.0 :
            std::min(settings_.trimEndSeconds, duration);
        if (!validTrimRange(start, end)) end = -1.0;
        const double maximumCursor = std::max(0.0, duration - 0.05);
        double cursor = std::clamp(start, 0.0, maximumCursor);
        int editing = -1;
        std::string entry;
        std::string status;
        bool accepted = false;

        const double dialogScale = std::max(0.65, std::min({
            scale_,
            static_cast<double>(DisplayWidth(display_, screen_) - s(40)) / 946.0,
            static_cast<double>(DisplayHeight(display_, screen_) - s(48)) / 794.0
        }));
        const auto d = [dialogScale](int value) {
            return static_cast<int>(std::lround(value * dialogScale));
        };
        const int dialogWidth = d(946), dialogHeight = d(794);
        const int dialogX = std::max(0, (DisplayWidth(display_, screen_) - dialogWidth) / 2);
        const int dialogY = std::max(0, (DisplayHeight(display_, screen_) - dialogHeight) / 2);
        Window dialog = XCreateSimpleWindow(display_, RootWindow(display_, screen_),
            dialogX, dialogY, static_cast<unsigned int>(dialogWidth),
            static_cast<unsigned int>(dialogHeight), 0,
            BlackPixel(display_, screen_), WhitePixel(display_, screen_));
        const std::string dialogTitle = "可视化视频裁剪";
        XStoreName(display_, dialog, dialogTitle.c_str());
        const Atom utf8String = XInternAtom(display_, "UTF8_STRING", False);
        const Atom netWmName = XInternAtom(display_, "_NET_WM_NAME", False);
        XChangeProperty(display_, dialog, netWmName, utf8String, 8, PropModeReplace,
                        reinterpret_cast<const unsigned char*>(dialogTitle.data()),
                        static_cast<int>(dialogTitle.size()));
        XClassHint dialogClass{};
        dialogClass.res_name = const_cast<char*>("video-compression-workbench-trimmer");
        dialogClass.res_class = const_cast<char*>("VideoCompressionWorkbenchTrimmer");
        XSetClassHint(display_, dialog, &dialogClass);
        XSetTransientForHint(display_, dialog, window_);
        const Atom deleteMessage = XInternAtom(display_, "WM_DELETE_WINDOW", False);
        XSetWMProtocols(display_, dialog, const_cast<Atom*>(&deleteMessage), 1);
        XSelectInput(display_, dialog, ExposureMask | StructureNotifyMask |
                     ButtonPressMask | KeyPressMask);
        XMapRaised(display_, dialog);
        auto canvas = std::make_unique<XftCanvas>(display_, dialog, dialogScale);
        const Palette colors = palette(dark_);
        const Rect previewRect{d(35), d(82), d(875), d(392)};
        const Rect seekRect{d(45), d(506), d(840), d(12)};
        const Rect backRect{d(45), d(556), d(134), d(38)};
        const Rect nextRect{d(191), d(556), d(134), d(38)};
        const Rect markInRect{d(410), d(556), d(185), d(38)};
        const Rect markOutRect{d(607), d(556), d(185), d(38)};
        const Rect startRect{d(45), d(656), d(270), d(39)};
        const Rect endRect{d(327), d(656), d(270), d(39)};
        const Rect clearRect{d(608), d(656), d(88), d(39)};
        const Rect applyRect{d(708), d(656), d(102), d(39)};
        const Rect cancelRect{d(822), d(656), d(88), d(39)};
        std::optional<PreviewFrame> frame;
        const auto refresh = [&]() {
            frame = controller_.videoPreview(source, cursor,
                std::min(1600, d(835)), std::min(900, d(355)));
            if (!frame) status = "当前时间位置无法提取画面，可尝试向前跳转。";
            else status.clear();
        };
        const auto jump = [&](double seconds) {
            const double target = std::clamp(cursor + seconds, 0.0, maximumCursor);
            if (std::abs(target - cursor) < 0.001) return;
            cursor = target;
            refresh();
        };
        const auto commit = [&]() -> bool {
            if (editing == -1) return true;
            if (entry.empty() && editing == 0) {
                editing = -1;
                status.clear();
                return true;
            }
            const auto time = entry.empty() && editing == 1
                ? std::optional<double>{-1.0} : parseTrimTime(entry.empty() ? "0" : entry);
            if (!time) {
                status = "请输入秒数或 时:分:秒.毫秒，例如 00:01:23.500。";
                return false;
            }
            const double newStart = editing == 0 ? *time : start;
            const double newEnd = editing == 1 ? *time : end;
            if (!validTrimRange(newStart, newEnd) || newStart >= duration ||
                (newEnd >= 0.0 && newEnd > duration + 0.05)) {
                status = "裁剪时间必须位于视频时长内，且结束时间必须晚于起始时间。";
                return false;
            }
            start = newStart;
            end = newEnd;
            editing = -1;
            entry.clear();
            status.clear();
            return true;
        };
        const auto drawDialog = [&]() {
            canvas->fill({0, 0, dialogWidth, dialogHeight}, colors.background);
            canvas->text(d(35), d(41), "可视化视频裁剪", colors.text, 2);
            canvas->textFit(d(36), d(69),
                basename(source) + "  ·  时长 " + formatTrimTime(duration),
                d(864), colors.muted, 0);
            canvas->rounded(previewRect, "#0c111c", 12);
            if (frame) {
                const int previewX = previewRect.x + (previewRect.width - frame->width) / 2;
                const int previewY = previewRect.y + (previewRect.height - frame->height) / 2;
                canvas->image({previewX, previewY, frame->width, frame->height}, *frame);
            } else {
                canvas->text(d(360), d(275), "当前画面不可用", colors.white, 1);
            }
            canvas->progress(seekRect, cursor / duration, colors);
            const int indicator = seekRect.x + static_cast<int>(
                std::lround(seekRect.width * (cursor / duration)));
            canvas->rounded({indicator - d(7), seekRect.y - d(5), d(14), d(22)},
                            colors.accent, d(7));
            canvas->text(d(45), d(543), "当前位置 " + formatTrimTime(cursor), colors.text, 1);
            canvas->text(d(523), d(543), "← / → 跳转 1 秒；Shift 跳转 10 秒",
                         colors.muted, 0);
            canvas->button(backRect, "< 后退 1 秒", colors);
            canvas->button(nextRect, "前进 1 秒 >", colors);
            canvas->button(markInRect, "当前位置设为起点", colors);
            canvas->button(markOutRect, "当前位置设为终点", colors);
            canvas->text(d(45), d(640), "裁剪起始时间", colors.muted, 0);
            canvas->text(d(327), d(640), "裁剪结束时间", colors.muted, 0);
            canvas->button(startRect, editing == 0 ? entry + "|" : formatTrimTime(start),
                           colors, editing == 0);
            canvas->button(endRect, editing == 1 ? entry + "|" :
                           end < 0 ? "直到片尾" : formatTrimTime(end),
                           colors, editing == 1);
            canvas->button(clearRect, "重置", colors);
            canvas->button(applyRect, "应用裁剪", colors, true);
            canvas->button(cancelRect, "取消", colors);
            canvas->textFit(d(45), d(741),
                status.empty() ? "点击时间可直接输入，结束时间留空表示保留到片尾。" : status,
                d(855), status.empty() ? colors.muted : colors.red, 0);
            canvas->flush();
        };
        refresh();
        drawDialog();
        bool finished = false;
        while (!finished) {
            if (!XPending(display_)) {
                pollfd descriptor{ConnectionNumber(display_), POLLIN, 0};
                poll(&descriptor, 1, 120);
                if ((descriptor.revents & POLLIN) == 0) continue;
            }
            XEvent event{};
            XNextEvent(display_, &event);
            if (event.xany.window != dialog) continue;
            if (event.type == ClientMessage &&
                static_cast<Atom>(event.xclient.data.l[0]) == deleteMessage) {
                finished = true;
            } else if (event.type == Expose) {
                drawDialog();
            } else if (event.type == ButtonPress && event.xbutton.button == Button1) {
                const int x = event.xbutton.x, y = event.xbutton.y;
                if (contains(startRect, x, y) || contains(endRect, x, y)) {
                    if (commit()) {
                        editing = contains(startRect, x, y) ? 0 : 1;
                        entry.clear();
                    }
                } else if (commit()) {
                    const Rect trackHit{seekRect.x, seekRect.y - d(14),
                                        seekRect.width, seekRect.height + d(28)};
                    if (contains(trackHit, x, y)) {
                        cursor = std::clamp(duration * (x - seekRect.x) /
                                            std::max(1, seekRect.width), 0.0, maximumCursor);
                        refresh();
                    } else if (contains(backRect, x, y)) jump(-1.0);
                    else if (contains(nextRect, x, y)) jump(1.0);
                    else if (contains(markInRect, x, y)) {
                        if (end >= 0.0 && cursor >= end)
                            status = "新的起始时间必须早于结束时间。";
                        else { start = cursor; status.clear(); }
                    } else if (contains(markOutRect, x, y)) {
                        if (cursor <= start)
                            status = "结束时间必须晚于起始时间。";
                        else { end = cursor; status.clear(); }
                    } else if (contains(clearRect, x, y)) {
                        start = 0.0; end = -1.0; status.clear();
                    } else if (contains(applyRect, x, y)) {
                        accepted = true;
                        finished = true;
                    } else if (contains(cancelRect, x, y)) finished = true;
                }
                if (!finished) drawDialog();
            } else if (event.type == KeyPress) {
                const KeySym key = XLookupKeysym(&event.xkey, 0);
                if (editing != -1) {
                    if (key == XK_Return || key == XK_KP_Enter) commit();
                    else if (key == XK_Escape) {
                        editing = -1; entry.clear(); status.clear();
                    } else if (key == XK_BackSpace && !entry.empty()) entry.pop_back();
                    else {
                        char text[16]{};
                        KeySym symbol{};
                        const int count = XLookupString(&event.xkey, text,
                                                        sizeof(text), &symbol, nullptr);
                        for (int i = 0; i < count && entry.size() < 20; ++i) {
                            const char ch = text[i];
                            if ((ch >= '0' && ch <= '9') || ch == ':' || ch == '.')
                                entry.push_back(ch);
                        }
                    }
                } else if (key == XK_Left || key == XK_Right ||
                           key == XK_less || key == XK_greater) {
                    const double seconds = (event.xkey.state & ShiftMask) &&
                        (key == XK_Left || key == XK_Right) ? 10.0 : 1.0;
                    jump(key == XK_Left || key == XK_less ? -seconds : seconds);
                } else if (key == XK_Home) {
                    cursor = 0.0; refresh();
                } else if (key == XK_End) {
                    cursor = maximumCursor; refresh();
                } else if (key == XK_Return || key == XK_KP_Enter) {
                    accepted = true; finished = true;
                } else if (key == XK_Escape) finished = true;
                if (!finished) drawDialog();
            }
        }
        canvas.reset();
        XDestroyWindow(display_, dialog);
        XFlush(display_);
        if (accepted) {
            settings_.trimStartSeconds = start;
            settings_.trimEndSeconds = end;
            trimError_.clear();
            updateSettings();
        }
    }

    bool commitTrimText() {
        if (editingTrim_ == -1) return true;
        const std::string raw = trimEditText_;
        if (raw.empty() && editingTrim_ == 0) {
            editingTrim_ = -1;
            trimError_.clear();
            return true;
        }
        const auto parsed = raw.empty() && editingTrim_ == 1
            ? std::optional<double>{-1.0} : parseTrimTime(raw.empty() ? "0" : raw);
        if (!parsed) {
            trimError_ = "请输入秒数或 时:分:秒.毫秒，例如 00:01:23.500。";
            return false;
        }
        const double start = editingTrim_ == 0 ? *parsed : settings_.trimStartSeconds;
        const double end = editingTrim_ == 1 ? *parsed : settings_.trimEndSeconds;
        if (!validTrimRange(start, end)) {
            trimError_ = "裁剪结束时间必须晚于起始时间。";
            return false;
        }
        settings_.trimStartSeconds = start;
        settings_.trimEndSeconds = end;
        editingTrim_ = -1;
        trimEditText_.clear();
        trimError_.clear();
        updateSettings();
        return true;
    }

    void addFiles(bool archiveOnly = false) {
        const auto files = chooseFiles(archiveOnly);
        if (!files.empty()) {
            controller_.addInputs(files);
        }
    }

    void startOrCancel() {
        const auto snapshot = controller_.snapshot();
        if (!snapshot.running) {
            controller_.startBatch();
        } else if (snapshot.paused) {
            controller_.resumeBatch();
        } else {
            controller_.pauseBatch();
        }
    }

    std::vector<std::pair<BatchTaskSnapshot, BatchItemSnapshot>> rows(
        const AppSnapshot& snapshot, bool failedOnly = false) const {
        std::vector<std::pair<BatchTaskSnapshot, BatchItemSnapshot>> result;

        for (const auto& task : snapshot.tasks) {
            if (failedOnly && task.state != TaskState::Failed) continue;
            if (task.kind == TaskKind::Video) {
                BatchItemSnapshot item;
                if (!task.items.empty()) {
                    item = task.items.front();
                } else {
                    item.name = basename(task.inputPath);
                    item.state = task.state;
                    item.progress = task.progress;
                    item.detail = task.detail;
                }
                result.emplace_back(task, std::move(item));
                continue;
            }

            BatchItemSnapshot parent;
            parent.name = basename(task.inputPath);
            parent.state = task.state;
            parent.progress = task.progress;
            parent.detail = task.detail;
            result.emplace_back(task, std::move(parent));

            for (const auto& item : task.items) {
                result.emplace_back(task, item);
            }
        }

        return result;
    }

    void draw() {
        const auto snapshot = controller_.snapshot();
        const auto backends = availableBackends(snapshot.capabilities, settings_.codec);
        const auto original = makeLayout(
            width_, height_, scale_, static_cast<int>(backends.size()),
            advancedExpanded_, queueFocused_);
        if (!queueFocused_ && !autoFocusUsed_ && !snapshot.tasks.empty() &&
            original.queue.height < s(112)) {
            queueFocused_ = true;
            autoFocusUsed_ = true;
        }
        const auto layout = makeLayout(
            width_, height_, scale_, static_cast<int>(backends.size()),
            advancedExpanded_, queueFocused_);
        const auto colors = palette(dark_);

        canvas_->fill({0, 0, width_, height_}, colors.background);
        canvas_->text(s(30), s(50), "视频压缩工作台", colors.text, 2);
        canvas_->text(
            s(31), s(82),
            "批量视频压缩 · ZIP / PPTX 媒体处理 · CPU 与硬件编码",
            colors.muted, 0);
        if (!trimError_.empty())
            canvas_->textFit(s(31), s(104), trimError_,
                             width_ - s(450), colors.red, 0);
        canvas_->button(
            layout.focusQueue, queueFocused_ ? "返回编码设置" : "专注处理队列",
            colors, queueFocused_);
        canvas_->button(
            layout.ssh, "SSH 服务器", colors, !settings_.remoteHost.empty());
        canvas_->button(
            layout.theme, dark_ ? "浅色外观" : "深色外观", colors);

        const bool compactToolbar = layout.importQueue.width < s(76);
        canvas_->button(layout.addFiles, "添加文件", colors, true);
        canvas_->button(layout.addFolder, "添加文件夹", colors);
        canvas_->button(layout.addArchive,
                        layout.addArchive.width < s(118) ? "ZIP/PPTX" : "添加 ZIP / PPTX",
                        colors);
        canvas_->button(layout.importQueue, compactToolbar ? "导入" : "导入清单", colors);
        canvas_->button(layout.exportQueue, compactToolbar ? "导出" : "导出清单", colors);
        canvas_->button(layout.removeSelected, compactToolbar ? "移除" : "移除选中", colors);
        canvas_->button(layout.clearDone, compactToolbar ? "清除" : "清除已完成", colors);

        if (!queueFocused_) {
        const int left = layout.queue.x, top = s(168);
        const bool compactAdvanced = advancedExpanded_ && height_ < s(940);
        canvas_->card({left, top, layout.queue.width,
                       s(advancedExpanded_ ? (compactAdvanced ? 416 : 474) : 282)}, colors);
        canvas_->text(left + s(20), top + s(30), "编码设置", colors.text, 1);
        canvas_->button(layout.advancedToggle,
                        advancedExpanded_ ? "收起高级设置" : "展开高级设置", colors);
        canvas_->text(left + s(18), top + s(66), "输出文件夹", colors.muted, 0);
        const std::string outputDirectory = settings_.outputDirectory.empty()
            ? "默认：与每个源文件保存在同一文件夹"
            : settings_.outputDirectory;
        canvas_->rounded(layout.outputField, colors.input, 8);
        canvas_->textFit(layout.outputField.x + s(10),
                         layout.outputField.y + layout.outputField.height / 2 + s(5),
                         outputDirectory, layout.outputField.width - s(20),
                         colors.text, 0);
        canvas_->button(layout.outputBrowse, "选择目录", colors);

        const int firstColumn = layout.container[0].x;
        const int secondColumn = layout.codec[0].x;
        const int firstWidth = layout.container[1].x +
            layout.container[1].width - firstColumn;
        canvas_->text(firstColumn, top + s(102), "输出容器", colors.muted, 0);
        canvas_->text(secondColumn, top + s(102), "视频编码", colors.muted, 0);
        const std::array<OutputContainer, 4> containers{
            OutputContainer::MP4, OutputContainer::MKV,
            OutputContainer::WebM, OutputContainer::SameAsSource
        };
        const std::array<std::string, 4> containerLabels{
            "MP4", "MKV", "WebM", "原格式"
        };
        for (std::size_t index = 0; index < containers.size(); ++index)
            canvas_->button(layout.container[index], containerLabels[index], colors,
                            settings_.outputContainer == containers[index]);
        const std::array<VideoCodec, 5> codecChoices{
            VideoCodec::H264, VideoCodec::H265, VideoCodec::VP9,
            VideoCodec::AV1, VideoCodec::H266
        };
        const auto supported = availableVideoCodecs(snapshot.capabilities);
        for (std::size_t index = 0; index < codecChoices.size(); ++index)
            canvas_->button(layout.codec[index], codecName(codecChoices[index]), colors,
                            settings_.codec == codecChoices[index],
                            std::find(supported.begin(), supported.end(),
                                      codecChoices[index]) == supported.end());

        canvas_->text(firstColumn, top + s(169), "编码设备", colors.muted, 0);
        canvas_->text(secondColumn, top + s(169),
                      settings_.codec == VideoCodec::H266 ? "目标质量 (QP)" :
                          settings_.codec == VideoCodec::VP9 ? "目标质量 (VP9 CRF)" :
                          "目标质量 (CRF)", colors.muted, 0);
        const std::array<std::string, 4> backendNames{
            "CPU", "NVENC", "Intel QSV", "AMD AMF"
        };
        for (std::size_t index = 0; index < backends.size(); ++index) {
            std::size_t labelIndex = 0;
            switch (backends[index]) {
            case EncoderBackend::CPU: labelIndex = 0; break;
            case EncoderBackend::NVENC: labelIndex = 1; break;
            case EncoderBackend::QSV: labelIndex = 2; break;
            case EncoderBackend::AMF: labelIndex = 3; break;
            }
            canvas_->button(layout.backend[index], backendNames[labelIndex], colors,
                            settings_.backend == backends[index]);
        }
        if (backends.empty())
            canvas_->text(firstColumn, top + s(205),
                          "当前 FFmpeg 环境没有可用的编码器", colors.red, 0);
        const int maximumCrf = crfMaximum(settings_.codec);
        const double crfFraction = static_cast<double>(settings_.crf) / maximumCrf;
        canvas_->progress(layout.crfTrack, crfFraction, colors);
        const int knobPosition = layout.crfTrack.x +
            static_cast<int>(std::lround(layout.crfTrack.width * crfFraction));
        canvas_->rounded({knobPosition - s(7), layout.crfTrack.y - s(6),
                          s(14), s(19)}, colors.accent, 7);
        const std::string crfLabel = editingCrf_
            ? (crfEditText_.empty() ? "输入" : crfEditText_)
            : std::to_string(settings_.crf);
        canvas_->button(layout.crfValue, crfLabel, colors, true);

        if (advancedExpanded_) {
            const int compressionIndex = std::clamp(
                static_cast<int>(settings_.compression), 0, 7);
            canvas_->text(firstColumn, top + s(compactAdvanced ? 227 : 235), "压缩级别", colors.muted, 0);
            canvas_->rounded(layout.compressionValue, colors.accentSoft, 7);
            canvas_->text(layout.compressionValue.x + s(10),
                          layout.compressionValue.y + s(17),
                          compressionName(settings_.compression), colors.accent, 0);
            canvas_->progress(layout.compressionTrack,
                              static_cast<double>(compressionIndex) / 7.0, colors);
            for (int index = 0; index < 8; ++index) {
                const int tickX = layout.compressionTrack.x +
                    static_cast<int>(std::lround(
                        static_cast<double>(layout.compressionTrack.width) * index / 7.0));
                canvas_->line(tickX, layout.compressionTrack.y + s(10),
                              tickX, layout.compressionTrack.y + s(14), colors.muted);
            }
            const int compressionX = layout.compressionTrack.x +
                static_cast<int>(std::lround(
                    static_cast<double>(layout.compressionTrack.width) * compressionIndex / 7.0));
            canvas_->rounded({compressionX - s(8), layout.compressionTrack.y - s(6),
                              s(16), s(19)}, colors.accent, 8);
            canvas_->text(firstColumn, top + s(compactAdvanced ? 281 : 289), "极速", colors.muted, 0);
            canvas_->text(firstColumn + firstWidth - s(70), top + s(compactAdvanced ? 281 : 289),
                          "极致压缩", colors.muted, 0);

            canvas_->text(secondColumn, top + s(compactAdvanced ? 220 : 235), "音频处理", colors.muted, 0);
            const std::array<std::string, 3> audioModes{
                "自动编码", "保留原音频", "移除音频"
            };
            for (int index = 0; index < 3; ++index)
                canvas_->button(layout.audioMode[index], audioModes[index], colors,
                                static_cast<int>(settings_.audioMode) == index);

            canvas_->text(firstColumn, top + s(compactAdvanced ? 285 : 312), "音频码率 (kbps)", colors.muted, 0);
            canvas_->text(secondColumn, top + s(compactAdvanced ? 285 : 312), "输出分辨率", colors.muted, 0);
            const std::array<int, 6> audioRates{96, 128, 160, 192, 256, 320};
            const std::array<std::string, 6> resolutionLabels{
                "原始", "2160p", "1440p", "1080p", "720p", "480p"
            };
            for (int index = 0; index < 6; ++index) {
                canvas_->button(layout.audio[index], std::to_string(audioRates[index]), colors,
                                settings_.audioBitrateKbps == audioRates[index],
                                settings_.audioMode != AudioMode::Encode);
                canvas_->button(layout.resolution[index], resolutionLabels[index], colors,
                                static_cast<int>(settings_.outputResolution) == index);
            }
            canvas_->button(layout.parallel,
                            settings_.maxParallelJobs == 0 ? "并行：自动" :
                                "并行：" + std::to_string(settings_.maxParallelJobs),
                            colors);
            canvas_->button(layout.recursive,
                            settings_.recursive ? "递归扫描✓" : "递归扫描",
                            colors, settings_.recursive);
            canvas_->button(layout.skip,
                            settings_.skipAlreadyProcessed ? "跳过重复✓" : "跳过重复",
                            colors, settings_.skipAlreadyProcessed);
            canvas_->button(layout.preserve,
                            settings_.preserveDirectoryStructure ? "保留目录✓" : "保留目录",
                            colors, settings_.preserveDirectoryStructure);
            canvas_->button(layout.overwrite,
                            settings_.overwrite ? "覆盖文件✓" : "保留文件",
                            colors, settings_.overwrite);
        }
        canvas_->text(layout.trimStart.x, layout.trimStart.y - s(4),
                      "裁剪起始时间", colors.muted, 0);
        canvas_->text(layout.trimEnd.x, layout.trimEnd.y - s(4),
                      "裁剪结束时间", colors.muted, 0);
        const std::string startLabel = editingTrim_ == 0
            ? trimEditText_ + "|" : formatTrimTime(settings_.trimStartSeconds);
        const std::string endLabel = editingTrim_ == 1
            ? trimEditText_ + "|" : settings_.trimEndSeconds < 0.0
                ? "直到片尾" : formatTrimTime(settings_.trimEndSeconds);
        canvas_->button(layout.trimStart, startLabel, colors,
                        editingTrim_ == 0, snapshot.running);
        canvas_->button(layout.trimEnd, endLabel, colors,
                        editingTrim_ == 1, snapshot.running);
        canvas_->button(layout.trimOpen, "可视化裁剪", colors, false, snapshot.running);
        canvas_->button(layout.trimReset, "重置", colors, false, snapshot.running);

        }

        canvas_->card(layout.queue, colors);
        const bool compactQueueHeader =
            layout.queue.width < s(820) && layout.queue.height >= s(170);
        const int queueHeaderHeight = compactQueueHeader ? s(83) : s(54);
        canvas_->text(layout.queue.x + s(20), layout.queue.y + s(34),
                      "处理队列", colors.text, 1);
        canvas_->button(layout.failedOnly,
                        failedOnly_ ? "显示全部" : "仅看失败",
                        colors, failedOnly_);
        const std::string queueSummary =
            "总计 " + std::to_string(snapshot.totalTasks) +
            " · 运行 " + std::to_string(snapshot.runningTasks) +
            " · 完成 " + std::to_string(snapshot.completedTasks) +
            " · 失败 " + std::to_string(snapshot.failedTasks);
        if (compactQueueHeader) {
            canvas_->textFit(layout.queue.x + s(20), layout.queue.y + s(69),
                             queueSummary, layout.queue.width - s(40), colors.muted, 0);
        } else if (layout.queue.width >= s(600)) {
            canvas_->textFit(layout.queue.x + layout.queue.width - s(330),
                             layout.queue.y + s(34), queueSummary,
                             s(310), colors.muted, 0);
        }

        const auto taskRows = rows(snapshot, failedOnly_);
        const int rowHeight = s(70);
        const int listTop = layout.queue.y + queueHeaderHeight;
        const int visibleRows = std::max(
            0, (layout.queue.height - queueHeaderHeight - s(14)) / rowHeight);
        const int maximumScroll = std::max(
            0, static_cast<int>(taskRows.size()) - visibleRows);
        scrollOffset_ = std::clamp(scrollOffset_, 0, maximumScroll);

        if (taskRows.empty()) {
            canvas_->text(
                layout.queue.x + s(30), listTop + s(55),
                failedOnly_ ? "当前没有失败的任务，点击“显示全部”查看其他任务。" :
                    "将视频、ZIP 或 PPTX 文件拖到窗口中，或使用上方按钮添加文件和文件夹。",
                colors.muted, 1);
        } else {
            const int rowCount = std::min(
                visibleRows,
                static_cast<int>(taskRows.size()) - scrollOffset_);
            for (int index = 0; index < rowCount; ++index) {
                const auto& [task, item] =
                    taskRows[static_cast<std::size_t>(index + scrollOffset_)];
                const int rowY = listTop + index * rowHeight;
                const bool childRow =
                    task.kind != TaskKind::Video && item.name != basename(task.inputPath);

                if (selectedTask_ == task.id) {
                    canvas_->rounded(
                        {layout.queue.x + s(10), rowY + s(2),
                         layout.queue.width - s(20), rowHeight - s(4)},
                        colors.accentSoft, 7);
                }

                const std::string title = childRow
                    ? "↳ " + item.name
                    : item.name + "  ·  " + taskKindName(task.kind);
                const int titleIndent = childRow ? s(38) : s(20);
                canvas_->textFit(
                    layout.queue.x + titleIndent, rowY + s(20),
                    title, layout.queue.width - titleIndent - s(160),
                    colors.text, childRow ? 0 : 1);

                const TaskState state = childRow ? item.state : task.state;
                const double progress = childRow ? item.progress : task.progress;
                const char* stateColor =
                    state == TaskState::Failed ? colors.red :
                    (state == TaskState::Completed || state == TaskState::Skipped)
                        ? colors.green : colors.muted;
                canvas_->text(
                    layout.queue.x + layout.queue.width - s(150),
                    rowY + s(20), taskStateName(state), stateColor, 0);
                if (state != TaskState::Failed) {
                    canvas_->progress(
                        {layout.queue.x + s(20), rowY + s(34),
                         layout.queue.width - s(40), s(7)},
                        progress, colors);
                }

                std::string detail;
                if (childRow) {
                    detail = formatProgressLine(item.progressInfo, item.detail);
                } else if (task.kind == TaskKind::Video && !task.items.empty()) {
                    detail = formatProgressLine(task.items.front().progressInfo, task.detail);
                } else {
                    detail = task.detail;
                }

                const std::string sizeSummary = childRow
                    ? formatTaskSize(item.inputSizeBytes, item.outputSizeBytes)
                    : formatTaskSize(task.inputSizeBytes, task.outputSizeBytes);
                if (!sizeSummary.empty() &&
                    (state == TaskState::Completed || state == TaskState::Skipped)) {
                    detail = sizeSummary;
                }

                const std::string message = state == TaskState::Failed
                    ? (childRow && !item.detail.empty()
                        ? item.detail : (task.error.empty() ? task.detail : task.error))
                    : detail;
                canvas_->textFit(
                    layout.queue.x + s(20), rowY + s(59),
                    message, layout.queue.width - s(40),
                    state == TaskState::Failed ? colors.red : colors.muted, 0);
                canvas_->line(
                    layout.queue.x + s(18), rowY + rowHeight - s(2),
                    layout.queue.x + layout.queue.width - s(18),
                    rowY + rowHeight - s(2), colors.border);
            }
        }

        canvas_->card(layout.overallCard, colors);
        const bool compactStatus = layout.overallCard.height < s(260);
        const int statusX = layout.overallCard.x + s(22);
        const int statusWidth = layout.overallCard.width - s(44);
        canvas_->textFit(statusX,
                         layout.overallCard.y + s(compactStatus ? 26 : 36),
                         "批量处理进度", statusWidth, colors.text, 1);
        canvas_->textFit(statusX,
                         layout.overallCard.y + s(compactStatus ? 55 : 76),
                         snapshot.status, statusWidth, colors.text, 0);
        canvas_->textFit(statusX,
                         layout.overallCard.y + s(compactStatus ? 77 : 102),
                         snapshot.detail, statusWidth, colors.muted, 0);
        canvas_->progress(layout.overallTrack, snapshot.overallProgress, colors);

        std::ostringstream percentage;
        percentage.setf(std::ios::fixed);
        percentage.precision(1);
        percentage << snapshot.overallProgress * 100.0 << "%";
        canvas_->text(
            statusX, compactStatus
                ? layout.overallCard.y + layout.overallCard.height - s(65)
                : layout.overallCard.y + s(178),
            percentage.str(), colors.accent, 1);
        canvas_->textFit(
            layout.overallCard.x + s(130), compactStatus
                ? layout.overallCard.y + layout.overallCard.height - s(65)
                : layout.overallCard.y + s(178),
            "完成 " + std::to_string(snapshot.completedTasks + snapshot.skippedTasks) +
                " / " + std::to_string(snapshot.totalTasks),
            layout.overallCard.width - s(152), colors.muted, 0);

        if (!snapshot.running) {
            const bool canStart = !snapshot.tasks.empty() &&
                snapshot.capabilities.supports(settings_.codec, settings_.backend);
            canvas_->button(layout.start, "开始批量处理", colors, true, !canStart);
        } else {
            canvas_->button(
                layout.pause, snapshot.paused ? "继续处理" : "暂停处理",
                colors, true);
            canvas_->button(layout.cancel, "取消任务", colors);
        }

        const int infoY = layout.overallCard.y + layout.overallCard.height + s(18);
        const Rect information{
            layout.overallCard.x,
            infoY,
            layout.overallCard.width,
            std::max(0, height_ - infoY - s(28))
        };
        canvas_->card(information, colors);
        const auto selected = std::find_if(
            snapshot.tasks.begin(), snapshot.tasks.end(),
            [this](const BatchTaskSnapshot& task) { return task.id == selectedTask_; });
        const bool compactInformation = information.height < s(340);
        if (selected != snapshot.tasks.end() && compactInformation) {
            const int labelX = information.x + s(22);
            const int maximumWidth = information.width - s(44);
            const int baseY = information.y + s(33);
            const int step = std::clamp(
                (information.height - s(55)) / 4, s(18), s(27));
            if (information.height >= s(70)) {
                canvas_->textFit(labelX, baseY, "当前选中任务", maximumWidth, colors.text, 1);
                canvas_->textFit(labelX, baseY + step, basename(selected->inputPath),
                                 maximumWidth, colors.text, 0);
            }
            if (information.height >= s(115)) {
                canvas_->textFit(labelX, baseY + step * 2,
                    taskStateName(selected->state) + " · " + taskKindName(selected->kind),
                    maximumWidth, selected->state == TaskState::Failed ? colors.red : colors.muted, 0);
            }
            if (information.height >= s(111)) {
                canvas_->textFit(labelX, baseY + step * 3,
                                 "来源：" + selected->inputPath, maximumWidth,
                                 colors.muted, 0);
            }
            if (information.height >= s(128)) {
                canvas_->textFit(labelX, baseY + step * 4,
                                 "输出：" + selected->outputPath, maximumWidth,
                                 selected->state == TaskState::Failed ? colors.red : colors.text, 0);
            }
        } else if (selected != snapshot.tasks.end()) {
            const int labelX = information.x + s(22);
            const int maxWidth = information.width - s(44);
            canvas_->text(labelX, information.y + s(38),
                          "当前选中任务", colors.text, 1);
            canvas_->textFit(labelX, information.y + s(76),
                             basename(selected->inputPath), maxWidth, colors.text, 1);
            const char* stateColor = selected->state == TaskState::Failed ? colors.red :
                selected->state == TaskState::Completed ? colors.green : colors.muted;
            canvas_->textFit(labelX, information.y + s(104),
                             taskStateName(selected->state) + " · " +
                                 taskKindName(selected->kind),
                             maxWidth, stateColor, 0);
            const std::string sizeLine = formatTaskSize(
                selected->inputSizeBytes, selected->outputSizeBytes);
            if (!sizeLine.empty())
                canvas_->textFit(labelX, information.y + s(133),
                                 sizeLine, maxWidth, colors.muted, 0);
            if (information.height >= s(214)) {
                canvas_->text(labelX, information.y + s(173),
                              "来源文件", colors.muted, 0);
                canvas_->textFit(labelX, information.y + s(198),
                                 selected->inputPath, maxWidth, colors.text, 0);
            }
            if (information.height >= s(291)) {
                canvas_->text(labelX, information.y + s(243),
                              "输出位置", colors.muted, 0);
                canvas_->textFit(labelX, information.y + s(268),
                                 selected->outputPath, maxWidth, colors.text, 0);
            }
            const std::string details =
                selected->error.empty() ? selected->detail : selected->error;
            if (!details.empty() && information.height > s(355)) {
                canvas_->text(labelX, information.y + s(324),
                              selected->error.empty() ? "当前状态" : "失败原因",
                              colors.muted, 0);
                canvas_->textFit(labelX, information.y + s(355), details,
                                 maxWidth, selected->error.empty() ?
                                     colors.text : colors.red, 0);
            }
        } else if (compactInformation) {
            const int labelX = information.x + s(22);
            const int columnWidth = (information.width - s(58)) / 2;
            const int secondX = labelX + columnWidth + s(14);
            const int baseY = information.y + s(34);
            const int startY = information.y + s(74);
            const int step = std::clamp(
                (information.height - s(83)) / 2, s(19), s(30));
            if (information.height >= s(50))
                canvas_->textFit(labelX, baseY, "设备与编码摘要",
                                 information.width - s(44), colors.text, 1);
            if (information.height >= s(102)) {
                canvas_->textFit(labelX, startY,
                    "格式 " + codecName(settings_.codec) + " · " +
                    outputContainerName(settings_.outputContainer),
                    columnWidth, colors.text, 0);
                canvas_->textFit(secondX, startY,
                    "编码器 " + backendName(settings_.backend), columnWidth, colors.text, 0);
            }
            if (information.height >= s(132)) {
                canvas_->textFit(labelX, startY + step,
                    "质量 " + std::to_string(settings_.crf) + " · " +
                    compressionName(settings_.compression), columnWidth, colors.text, 0);
                canvas_->textFit(secondX, startY + step,
                    "音频 " + audioModeName(settings_.audioMode), columnWidth, colors.text, 0);
            }
            if (information.height >= s(128)) {
                canvas_->textFit(labelX, startY + step * 2,
                    "分辨率 " + outputResolutionName(settings_.outputResolution),
                    columnWidth, colors.text, 0);
                canvas_->textFit(secondX, startY + step * 2,
                    settings_.remoteHost.empty() ? "计算设备 本机"
                        : "计算设备 " + settings_.remoteHost,
                    columnWidth, colors.text, 0);
            }
        } else {
        canvas_->text(
            information.x + s(22), information.y + s(38),
            "设备与编码摘要", colors.text, 1);
        canvas_->text(
            information.x + s(22), information.y + s(78),
            "格式  " + codecName(settings_.codec) + " · " +
                outputContainerName(settings_.outputContainer),
            colors.text, 0);
        canvas_->text(
            information.x + s(22), information.y + s(108),
            "编码器  " + backendName(settings_.backend), colors.text, 0);
        canvas_->text(
            information.x + s(22), information.y + s(138),
            "CRF  " + std::to_string(settings_.crf) + " · " +
                compressionName(settings_.compression),
            colors.text, 0);
        canvas_->text(
            information.x + s(22), information.y + s(168),
            "音频  " + audioModeName(settings_.audioMode) +
                (settings_.audioMode == AudioMode::Encode
                    ? " · " + std::to_string(settings_.audioBitrateKbps) + " kbps"
                    : ""),
            colors.text, 0);
        canvas_->text(
            information.x + s(22), information.y + s(198),
            "分辨率  " + outputResolutionName(settings_.outputResolution),
            colors.text, 0);
        canvas_->text(
            information.x + s(22), information.y + s(228),
            settings_.remoteHost.empty()
                ? "计算设备  本机"
                : "计算设备  远程 · " + settings_.remoteHost,
            colors.text, 0);
        if (!snapshot.error.empty()) {
            canvas_->textFit(
                information.x + s(22),
                information.y + information.height - s(42),
                snapshot.error, information.width - s(44), colors.red, 0);
        } else if (information.height >= s(310)) {
            canvas_->textFit(
                information.x + s(22), information.y + s(258),
                snapshot.resourcePlan.explanation,
                information.width - s(44), colors.muted, 0);
        }
        if (information.height >= s(385)) {
            canvas_->textFit(
                information.x + s(22), information.y + s(330),
                "点击处理队列中的任务，可查看来源文件、输出位置和处理状态。",
                information.width - s(44), colors.muted, 0);
        }
        }

        canvas_->flush();
    }

    void commitCompression(int x, const Layout& layout) {
        if (controller_.snapshot().running) return;
        const double fraction = static_cast<double>(x - layout.compressionTrack.x) /
            std::max(1, layout.compressionTrack.width);
        const int step = std::clamp(
            static_cast<int>(std::lround(std::clamp(fraction, 0.0, 1.0) * 7.0)), 0, 7);
        if (step == static_cast<int>(settings_.compression)) return;
        settings_.compression = static_cast<CompressionLevel>(step);
        updateSettings();
    }

    void commitCrf(int x, const Layout& layout) {
        editingCrf_ = false;
        crfEditText_.clear();

        const double fraction = std::clamp(
            static_cast<double>(x - layout.crfTrack.x) /
                static_cast<double>(std::max(1, layout.crfTrack.width)),
            0.0, 1.0);
        settings_.crf = static_cast<int>(
            std::lround(fraction * crfMaximum(settings_.codec)));
        updateSettings();
    }

    void click(int x, int y) {
        const auto snapshot = controller_.snapshot();
        const auto visibleBackends =
            availableBackends(snapshot.capabilities, settings_.codec);
        const auto layout = makeLayout(
            width_, height_, scale_, static_cast<int>(visibleBackends.size()),
            advancedExpanded_, queueFocused_);
        compressionFocused_ = false;

        if (editingCrf_ && !contains(layout.crfValue, x, y)) {
            commitCrfText();
        }

        if (contains(layout.ssh, x, y)) {
            if (!snapshot.running) {
                manageSshServers();
            }
            return;
        }
        if (contains(layout.theme, x, y)) {
            dark_ = !dark_;
            return;
        }
        if (contains(layout.addFiles, x, y)) {
            addFiles();
            return;
        }
        if (contains(layout.addFolder, x, y)) {
            chooseFolder();
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
            if (selectedTask_ != 0 && !snapshot.running) {
                controller_.removeTask(selectedTask_);
                selectedTask_ = 0;
            }
            return;
        }
        if (contains(layout.clearDone, x, y)) {
            if (!snapshot.running) {
                controller_.clearCompleted();
                selectedTask_ = 0;
                scrollOffset_ = 0;
            }
            return;
        }
        if (contains(layout.focusQueue, x, y)) {
            queueFocused_ = !queueFocused_;
            autoFocusUsed_ = true;
            scrollOffset_ = 0;
            compressionFocused_ = false;
            return;
        }
        if (contains(layout.failedOnly, x, y)) {
            failedOnly_ = !failedOnly_;
            selectedTask_ = 0;
            scrollOffset_ = 0;
            return;
        }
        if (!snapshot.running && contains(layout.start, x, y)) {
            controller_.startBatch();
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
        if (!queueFocused_) {
        if (!snapshot.running &&
            (contains(layout.trimStart, x, y) || contains(layout.trimEnd, x, y))) {
            if (!commitTrimText()) return;
            editingTrim_ = contains(layout.trimStart, x, y) ? 0 : 1;
            trimEditText_.clear();
            XSetInputFocus(display_, window_, RevertToParent, CurrentTime);
            return;
        }
        if (!commitTrimText()) return;
        if (!snapshot.running && contains(layout.trimOpen, x, y)) {
            openVideoTrimmer();
            return;
        }
        if (!snapshot.running && contains(layout.trimReset, x, y)) {
            settings_.trimStartSeconds = 0.0;
            settings_.trimEndSeconds = -1.0;
            updateSettings();
            return;
        }
        if (contains(layout.outputBrowse, x, y)) {
            chooseOutput();
            return;
        }
        if (contains(layout.advancedToggle, x, y)) {
            advancedExpanded_ = !advancedExpanded_;
            compressionFocused_ = false;
            scrollOffset_ = 0;
            return;
        }
        if (contains(layout.crfValue, x, y)) {
            editingCrf_ = true;
            crfEditText_.clear();
            XSetInputFocus(display_, window_, RevertToParent, CurrentTime);
            return;
        }

        const std::array<OutputContainer, 4> containerChoices{
            OutputContainer::MP4, OutputContainer::MKV,
            OutputContainer::WebM, OutputContainer::SameAsSource
        };
        const std::array<VideoCodec, 5> codecChoices{
            VideoCodec::H264, VideoCodec::H265, VideoCodec::VP9,
            VideoCodec::AV1, VideoCodec::H266
        };
        const auto supported = availableVideoCodecs(snapshot.capabilities);
        for (std::size_t index = 0; index < containerChoices.size(); ++index) {
            if (!contains(layout.container[index], x, y)) continue;
            const OutputContainer selected = containerChoices[index];
            const std::string extension = outputVideoExtension(selected, "sample.mp4");
            if (selected != OutputContainer::SameAsSource &&
                !containerSupportsVideoCodec(extension, settings_.codec)) {
                const auto compatible = std::find_if(
                    supported.begin(), supported.end(),
                    [&extension](VideoCodec codec) {
                        return containerSupportsVideoCodec(extension, codec);
                    });
                if (compatible == supported.end()) return;
                settings_.codec = *compatible;
                settings_.backend = EncoderBackend::CPU;
                settings_.crf = defaultCrf(settings_.codec);
            }
            settings_.outputContainer = selected;
            updateSettings();
            return;
        }

        for (std::size_t index = 0; index < codecChoices.size(); ++index) {
            if (!contains(layout.codec[index], x, y)) continue;
            const VideoCodec codec = codecChoices[index];
            if (std::find(supported.begin(), supported.end(), codec) == supported.end())
                return;
            settings_.codec = codec;
            settings_.crf = defaultCrf(codec);
            const std::string extension =
                outputVideoExtension(settings_.outputContainer, "sample.mp4");
            if (settings_.outputContainer != OutputContainer::SameAsSource &&
                !containerSupportsVideoCodec(extension, codec))
                settings_.outputContainer = codec == VideoCodec::VP9
                    ? OutputContainer::WebM : OutputContainer::MKV;
            if (!snapshot.capabilities.supports(codec, settings_.backend)) {
                for (const auto backend : {EncoderBackend::CPU, EncoderBackend::NVENC,
                                           EncoderBackend::QSV, EncoderBackend::AMF}) {
                    if (snapshot.capabilities.supports(codec, backend)) {
                        settings_.backend = backend;
                        break;
                    }
                }
            }
            updateSettings();
            return;
        }

        for (std::size_t index = 0; index < visibleBackends.size(); ++index) {
            if (contains(layout.backend[index], x, y)) {
                settings_.backend = visibleBackends[index];
                updateSettings();
                return;
            }
        }

        if (advancedExpanded_ && !snapshot.running) {
            const Rect hitArea{layout.compressionTrack.x - s(10),
                               layout.compressionTrack.y - s(15),
                               layout.compressionTrack.width + s(20), s(43)};
            if (contains(hitArea, x, y)) {
                compressionFocused_ = true;
                draggingCompression_ = true;
                commitCompression(x, layout);
                return;
            }
        }
        compressionFocused_ = false;

        if (advancedExpanded_) {
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
            settings_.maxParallelJobs = choices[(currentIndex + 1) % choices.size()];
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
            settings_.preserveDirectoryStructure = !settings_.preserveDirectoryStructure;
            updateSettings();
            return;
        }
        if (contains(layout.overwrite, x, y)) {
            settings_.overwrite = !settings_.overwrite;
            updateSettings();
            return;
        }

        }

        const int crfTop = layout.crfTrack.y - s(10);
        const int crfBottom = layout.crfTrack.y + s(18);
        const int crfRight = layout.crfTrack.x + layout.crfTrack.width;
        if (y >= crfTop && y <= crfBottom &&
            x >= layout.crfTrack.x && x <= crfRight) {
            draggingCrf_ = true;
            commitCrf(x, layout);
            return;
        }

        }
        const bool compactQueueHeader =
            layout.queue.width < s(820) && layout.queue.height >= s(170);
        const int headerHeight = compactQueueHeader ? s(83) : s(54);
        const int listTop = layout.queue.y + headerHeight;
        const int rowHeight = s(70);
        const int visibleCount = std::max(
            0, (layout.queue.height - headerHeight - s(14)) / rowHeight);
        if (y >= listTop && y < listTop + visibleCount * rowHeight &&
            x >= layout.queue.x && x < layout.queue.x + layout.queue.width) {
            const int rowIndex = (y - listTop) / rowHeight + scrollOffset_;
            const auto taskRows = rows(controller_.snapshot(), failedOnly_);
            if (rowIndex >= 0 && rowIndex < static_cast<int>(taskRows.size()))
                selectedTask_ = taskRows[static_cast<std::size_t>(rowIndex)].first.id;
        }
    }

    std::vector<std::string> readDropSelection() {
        Atom actualType{};
        int actualFormat = 0;
        unsigned long itemCount = 0;
        unsigned long bytesAfter = 0;
        unsigned char* data = nullptr;
        std::vector<std::string> paths;

        const Atom property = XInternAtom(display_, "XdndSelection", False);
        const int status = XGetWindowProperty(
            display_, window_, property, 0, 65536, True, uriList_,
            &actualType, &actualFormat, &itemCount, &bytesAfter, &data);
        if (status != Success || data == nullptr) {
            return paths;
        }

        const std::string content(reinterpret_cast<const char*>(data), itemCount);
        XFree(data);

        std::istringstream input(content);
        std::string line;
        while (std::getline(input, line)) {
            line = trim(std::move(line));
            if (line.empty() || line.front() == '#') {
                continue;
            }
            if (line.starts_with("file://")) {
                line = percentDecode(line.substr(7));
            }
            if (!line.empty()) {
                paths.push_back(std::move(line));
            }
        }

        return paths;
    }

    bool handle(XEvent& event) {
        if (event.type == Expose) {
            draw();
            return false;
        }
        if (event.type == ConfigureNotify) {
            width_ = event.xconfigure.width;
            height_ = event.xconfigure.height;
            return false;
        }

        if (event.type == ClientMessage) {
            if (static_cast<Atom>(event.xclient.data.l[0]) == wmDelete_) {
                return true;
            }

            const Atom messageType = static_cast<Atom>(event.xclient.message_type);
            if (messageType == xdndEnter_) {
                dragSource_ = static_cast<Window>(event.xclient.data.l[0]);
                return false;
            }
            if (messageType == xdndPosition_) {
                XEvent reply{};
                reply.xclient.type = ClientMessage;
                reply.xclient.display = display_;
                reply.xclient.window = dragSource_;
                reply.xclient.message_type = xdndStatus_;
                reply.xclient.format = 32;
                reply.xclient.data.l[0] = window_;
                reply.xclient.data.l[1] = 1;
                reply.xclient.data.l[4] = static_cast<long>(
                    XInternAtom(display_, "XdndActionCopy", False));
                XSendEvent(display_, dragSource_, False, NoEventMask, &reply);
                XFlush(display_);
                return false;
            }
            if (messageType == xdndDrop_) {
                const Atom property = XInternAtom(display_, "XdndSelection", False);
                XConvertSelection(
                    display_, xdndSelection_, uriList_, property,
                    window_, static_cast<Time>(event.xclient.data.l[2]));
                return false;
            }
        }

        if (event.type == SelectionNotify) {
            const auto paths = readDropSelection();
            if (!paths.empty()) {
                controller_.addInputs(paths);
            }

            XEvent reply{};
            reply.xclient.type = ClientMessage;
            reply.xclient.display = display_;
            reply.xclient.window = dragSource_;
            reply.xclient.message_type = xdndFinished_;
            reply.xclient.format = 32;
            reply.xclient.data.l[0] = window_;
            reply.xclient.data.l[1] = paths.empty() ? 0 : 1;
            reply.xclient.data.l[2] = static_cast<long>(
                XInternAtom(display_, "XdndActionCopy", False));
            XSendEvent(display_, dragSource_, False, NoEventMask, &reply);
            XFlush(display_);
            return false;
        }

        if (event.type == ButtonPress) {
            if (event.xbutton.button == Button4 || event.xbutton.button == Button5) {
                const auto backends = availableBackends(
                    controller_.snapshot().capabilities, settings_.codec);
                const Layout layout = makeLayout(
                    width_, height_, scale_, static_cast<int>(backends.size()),
                    advancedExpanded_, queueFocused_);
                if (contains(layout.queue, event.xbutton.x, event.xbutton.y)) {
                    const int offset = event.xbutton.button == Button4 ? -3 : 3;
                    scrollOffset_ = std::max(0, scrollOffset_ + offset);
                }
                return false;
            }

            click(event.xbutton.x, event.xbutton.y);
            return false;
        }

        if (event.type == MotionNotify && (draggingCrf_ || draggingCompression_)) {
            const auto capabilities = controller_.snapshot().capabilities;
            const auto backends = availableBackends(capabilities, settings_.codec);
            const Layout layout = makeLayout(
                width_, height_, scale_, static_cast<int>(backends.size()),
                advancedExpanded_, queueFocused_);
            if (draggingCrf_) commitCrf(event.xmotion.x, layout);
            if (draggingCompression_) commitCompression(event.xmotion.x, layout);
            return false;
        }
        if (event.type == ButtonRelease) {
            draggingCrf_ = false;
            draggingCompression_ = false;
            return false;
        }

        if (event.type == KeyPress) {
            const KeySym key = XLookupKeysym(&event.xkey, 0);
            const bool controlPressed = (event.xkey.state & ControlMask) != 0;

            if (editingTrim_ != -1) {
                if (key == XK_Return || key == XK_KP_Enter) {
                    commitTrimText();
                } else if (key == XK_BackSpace && !trimEditText_.empty()) {
                    trimEditText_.pop_back();
                } else if (key == XK_Escape) {
                    editingTrim_ = -1;
                    trimEditText_.clear();
                    trimError_.clear();
                } else {
                    char buffer[16]{};
                    KeySym symbol{};
                    const int count = XLookupString(&event.xkey, buffer,
                                                    sizeof(buffer), &symbol, nullptr);
                    for (int index = 0; index < count && trimEditText_.size() < 20; ++index) {
                        const char ch = buffer[index];
                        if ((ch >= '0' && ch <= '9') || ch == ':' || ch == '.')
                            trimEditText_.push_back(ch);
                    }
                }
                return false;
            }

            if (editingCrf_) {
                if (key == XK_Return || key == XK_KP_Enter) {
                    commitCrfText();
                } else if (key == XK_BackSpace && !crfEditText_.empty()) {
                    crfEditText_.pop_back();
                } else if (key == XK_Escape) {
                    editingCrf_ = false;
                    crfEditText_.clear();
                } else {
                    char text[8]{};
                    KeySym symbol{};
                    const int length = XLookupString(
                        &event.xkey, text, sizeof(text), &symbol, nullptr);
                    for (int index = 0;
                         index < length && crfEditText_.size() < 3;
                         ++index) {
                        if (text[index] >= '0' && text[index] <= '9') {
                            crfEditText_.push_back(text[index]);
                        }
                    }
                }
                return false;
            }

            if (compressionFocused_ && advancedExpanded_ &&
                (key == XK_Left || key == XK_Right || key == XK_Home || key == XK_End) &&
                !controller_.snapshot().running) {
                int step = static_cast<int>(settings_.compression);
                if (key == XK_Left) --step;
                if (key == XK_Right) ++step;
                if (key == XK_Home) step = 0;
                if (key == XK_End) step = 7;
                step = std::clamp(step, 0, 7);
                if (step != static_cast<int>(settings_.compression)) {
                    settings_.compression = static_cast<CompressionLevel>(step);
                    updateSettings();
                }
                return false;
            }
            if (controlPressed && (key == XK_q || key == XK_Q)) {
                queueFocused_ = !queueFocused_;
                autoFocusUsed_ = true;
                compressionFocused_ = false;
                scrollOffset_ = 0;
                return false;
            }
            if (controlPressed && key == XK_o) {
                addFiles();
                return false;
            }
            if (controlPressed && key == XK_d) {
                if (!controller_.snapshot().running) {
                    controller_.clearCompleted();
                    selectedTask_ = 0;
                    scrollOffset_ = 0;
                }
                return false;
            }
            if (controlPressed && key == XK_Return) {
                startOrCancel();
                return false;
            }
            if (key == XK_Delete && selectedTask_ != 0 &&
                !controller_.snapshot().running) {
                controller_.removeTask(selectedTask_);
                selectedTask_ = 0;
                return false;
            }
            if (key == XK_Escape) {
                selectedTask_ = 0;
                compressionFocused_ = false;
                return false;
            }
        }

        return false;
    }
    AppController& controller_;
    Display* display_{};
    int screen_{};
    Window window_{};
    double scale_{1.0};
    int width_{1500};
    int height_{1020};
    std::unique_ptr<XftCanvas> canvas_;
    BatchSettings settings_;
    bool dark_{false};
    bool draggingCrf_{false};
    bool draggingCompression_{false};
    bool compressionFocused_{false};
    bool advancedExpanded_{false};
    bool queueFocused_{false};
    bool autoFocusUsed_{false};
    bool failedOnly_{false};
    bool editingCrf_{false};
    std::string crfEditText_;
    int editingTrim_{-1};
    std::string trimEditText_;
    std::string trimError_;
    int scrollOffset_{0};
    std::uint64_t selectedTask_{0};
    Atom wmDelete_{};
    Atom xdndEnter_{};
    Atom xdndPosition_{};
    Atom xdndStatus_{};
    Atom xdndDrop_{};
    Atom xdndFinished_{};
    Atom xdndSelection_{};
    Atom uriList_{};
    Window dragSource_{};
};

} // namespace

int runNativeUi(AppController& controller){LinuxWindow window(controller);return window.run();}

} // namespace ffmpegui
