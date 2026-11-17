#include "ui.hpp"

#include "../core/app_controller.hpp"
#include "../core/process.hpp"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cmath>
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
    Rect outputField, outputBrowse, container[2], codec[3], backend[4], crfTrack, crfValue;
    Rect compression[8], audio[6], parallel, recursive, skip, preserve, overwrite;
    Rect queue, overallCard, overallTrack, start, pause, cancel, ssh, theme;
};
std::vector<EncoderBackend> availableBackends(const EncoderCapabilities& capabilities, VideoCodec codec) {
    std::vector<EncoderBackend> result;
    for (const auto backend : {EncoderBackend::CPU, EncoderBackend::NVENC, EncoderBackend::QSV, EncoderBackend::AMF})
        if (capabilities.supports(codec, backend)) result.push_back(backend);
    return result;
}
Layout makeLayout(int width, int height, double scale, int backendCount = 4) {
    const auto s=[scale](int v){return static_cast<int>(std::lround(v*scale));};
    const int pad=s(28),gap=s(18),rightW=std::clamp(width/3,s(370),s(450));
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
    l.outputField={leftX+s(20),sy+s(50),leftW-s(144),s(38)}; l.outputBrowse={leftX+leftW-s(112),sy+s(50),s(92),s(38)};
    const int containerW=(leftW-s(52)-s(6))/2;
    for(int i=0;i<2;++i)l.container[i]={leftX+s(20)+i*(containerW+s(6)),sy+s(112),containerW,s(32)};
    const int codecW=(leftW-s(52))/3;
    for(int i=0;i<3;++i)l.codec[i]={leftX+s(20)+i*(codecW+s(6)),sy+s(160),codecW,s(38)};
    backendCount=std::clamp(backendCount,1,4);const int backendW=(leftW-s(52)-s(6)*(backendCount-1))/backendCount;
    for(int i=0;i<backendCount;++i)l.backend[i]={leftX+s(20)+i*(backendW+s(6)),sy+s(210),backendW,s(34)};
    l.crfTrack={leftX+s(20),sy+s(276),leftW-s(144),s(10)};l.crfValue={leftX+leftW-s(112),sy+s(261),s(92),s(38)};
    const int compW=(leftW-s(52)-s(18))/4;
    for(int i=0;i<8;++i){const int row=i/4,col=i%4;l.compression[i]={leftX+s(20)+col*(compW+s(6)),sy+s(324)+row*s(39),compW,s(32)};}
    const int audioW=(leftW-s(52)-s(30))/6;
    for(int i=0;i<6;++i)l.audio[i]={leftX+s(20)+i*(audioW+s(6)),sy+s(414),audioW,s(32)};
    const int optionGap = s(6);
    const int optionWidth = (leftW - s(40) - optionGap * 4) / 5;
    const int optionY = sy + s(482);
    l.parallel={leftX+s(20),optionY,optionWidth,s(32)};
    l.recursive={leftX+s(20)+optionWidth+optionGap,optionY,optionWidth,s(32)};
    l.skip={leftX+s(20)+(optionWidth+optionGap)*2,optionY,optionWidth,s(32)};
    l.preserve={leftX+s(20)+(optionWidth+optionGap)*3,optionY,optionWidth,s(32)};
    l.overwrite={leftX+s(20)+(optionWidth+optionGap)*4,optionY,optionWidth,s(32)};
    l.queue = {leftX, sy + s(530), leftW, height - (sy + s(530)) - pad};
    l.overallCard={rightX,top,rightW,s(270)};l.overallTrack={rightX+s(22),top+s(126),rightW-s(44),s(12)};
    l.start={rightX+s(22),top+s(206),rightW-s(44),s(44)};
    const int actionGap=s(8),actionWidth=(rightW-s(52)-actionGap)/2;
    l.pause={rightX+s(22),top+s(206),actionWidth,s(44)};
    l.cancel={rightX+s(22)+actionWidth+actionGap,top+s(206),rightW-s(44)-actionWidth-actionGap,s(44)};
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

        const int estimatedTextWidth = static_cast<int>(label.size()) * 7 / 2;
        const int textX = rectangle.x +
            std::max(8, (rectangle.width - estimatedTextWidth) / 2);
        const int textY = rectangle.y + rectangle.height / 2 + 5;
        text(textX, textY, label,
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
    explicit LinuxWindow(AppController& controller):controller_(controller){}
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
        XStoreName(display_, window_, "FFmpeg Studio");

        XClassHint classHint{};
        classHint.res_name = const_cast<char*>("ffmpeg-studio");
        classHint.res_class = const_cast<char*>("FFmpegStudio");
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
            "--file-filter=FFmpeg Studio Queue | *.ffq *.txt"
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
            "--title=导出任务清单", "--file-filter=FFmpeg Studio Queue | *.ffq"
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
        const AppSnapshot& snapshot) const {
        std::vector<std::pair<BatchTaskSnapshot, BatchItemSnapshot>> result;

        for (const auto& task : snapshot.tasks) {
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
        const auto layout = makeLayout(
            width_, height_, scale_, static_cast<int>(backends.size()));
        const auto colors = palette(dark_);

        canvas_->fill({0, 0, width_, height_}, colors.background);
        canvas_->text(s(30), s(50), "FFmpeg Studio", colors.text, 2);
        canvas_->text(
            s(31), s(82),
            "批量视频压缩 · ZIP / PPTX 媒体处理 · CPU 与硬件编码",
            colors.muted, 0);
        canvas_->button(
            layout.ssh, "SSH 服务器", colors, !settings_.remoteHost.empty());
        canvas_->button(
            layout.theme, dark_ ? "浅色外观" : "深色外观", colors);

        canvas_->button(layout.addFiles, "添加文件", colors, true);
        canvas_->button(layout.addFolder, "添加文件夹", colors);
        canvas_->button(layout.addArchive, "添加 ZIP / PPTX", colors);
        canvas_->button(layout.importQueue, "导入清单", colors);
        canvas_->button(layout.exportQueue, "导出清单", colors);
        canvas_->button(layout.removeSelected, "移除选中", colors);
        canvas_->button(layout.clearDone, "清除已完成", colors);

        const int left = s(28);
        const int top = s(168);
        canvas_->card({left, top, layout.queue.width, s(520)}, colors);
        canvas_->text(left + s(20), top + s(32), "编码设置", colors.text, 1);
        canvas_->text(left + s(20), top + s(64), "输出文件夹", colors.muted, 0);

        const std::string outputDirectory = settings_.outputDirectory.empty()
            ? "默认：与每个源文件保存在同一文件夹"
            : settings_.outputDirectory;
        canvas_->rounded(layout.outputField, colors.input, 8);
        canvas_->text(
            layout.outputField.x + s(12),
            layout.outputField.y + layout.outputField.height / 2 + s(5),
            outputDirectory, colors.text, 0);
        canvas_->button(layout.outputBrowse, "选择目录", colors);

        canvas_->text(left + s(20), top + s(98), "输出容器格式", colors.muted, 0);
        const std::array<std::string, 2> containers{"MP4", "与源文件相同"};
        for (std::size_t index = 0; index < containers.size(); ++index) {
            canvas_->button(
                layout.container[index], containers[index], colors,
                static_cast<int>(settings_.outputContainer) == static_cast<int>(index));
        }

        canvas_->text(left + s(20), top + s(149), "视频编码格式", colors.muted, 0);
        const std::array<std::string, 3> codecs{"H.264", "H.265", "AV1"};
        for (std::size_t index = 0; index < codecs.size(); ++index) {
            canvas_->button(
                layout.codec[index], codecs[index], colors,
                static_cast<int>(settings_.codec) == static_cast<int>(index));
        }

        canvas_->text(left + s(20), top + s(197), "编码设备", colors.muted, 0);
        const std::array<std::string, 4> backendNames{
            "CPU", "NVIDIA NVENC", "Intel QSV", "AMD AMF"
        };
        for (std::size_t index = 0; index < backends.size(); ++index) {
            std::size_t labelIndex = 0;
            switch (backends[index]) {
            case EncoderBackend::CPU: labelIndex = 0; break;
            case EncoderBackend::NVENC: labelIndex = 1; break;
            case EncoderBackend::QSV: labelIndex = 2; break;
            case EncoderBackend::AMF: labelIndex = 3; break;
            }
            canvas_->button(
                layout.backend[index], backendNames[labelIndex], colors,
                settings_.backend == backends[index]);
        }
        if (backends.empty()) {
            canvas_->text(
                left + s(20), top + s(181),
                "当前 FFmpeg 环境没有可用的视频编码器", colors.red, 0);
        }

        canvas_->text(left + s(20), top + s(245), "CRF / 目标质量", colors.muted, 0);
        const int maximumCrf = crfMaximum(settings_.codec);
        const double crfFraction =
            static_cast<double>(settings_.crf) / static_cast<double>(maximumCrf);
        canvas_->progress(layout.crfTrack, crfFraction, colors);
        const int knobPosition = layout.crfTrack.x +
            static_cast<int>(std::lround(layout.crfTrack.width * crfFraction));
        canvas_->rounded(
            {knobPosition - s(7), layout.crfTrack.y - s(5), s(14), s(20)},
            colors.accent, 7);
        const std::string crfLabel = editingCrf_
            ? (crfEditText_.empty() ? "输入" : crfEditText_)
            : std::to_string(settings_.crf);
        canvas_->button(layout.crfValue, crfLabel, colors, true);

        canvas_->text(left + s(20), top + s(296), "压缩级别", colors.muted, 0);
        const std::array<std::string, 8> compressionLevels{
            "极速", "非常快", "快速", "均衡", "慢速", "较慢", "很慢", "极致压缩"
        };
        for (std::size_t index = 0; index < compressionLevels.size(); ++index) {
            canvas_->button(
                layout.compression[index], compressionLevels[index], colors,
                static_cast<int>(settings_.compression) == static_cast<int>(index));
        }

        canvas_->text(left + s(20), top + s(383), "音频码率", colors.muted, 0);
        const std::array<int, 6> audioRates{96, 128, 160, 192, 256, 320};
        for (std::size_t index = 0; index < audioRates.size(); ++index) {
            canvas_->button(
                layout.audio[index], std::to_string(audioRates[index]) + " kbps",
                colors, settings_.audioBitrateKbps == audioRates[index]);
        }

        canvas_->button(
            layout.parallel,
            settings_.maxParallelJobs == 0
                ? "并行：自动"
                : "并行：" + std::to_string(settings_.maxParallelJobs),
            colors);
        canvas_->button(
            layout.recursive, settings_.recursive ? "递归扫描✓" : "递归扫描",
            colors, settings_.recursive);
        canvas_->button(
            layout.skip, settings_.skipAlreadyProcessed ? "跳过重复✓" : "跳过重复",
            colors, settings_.skipAlreadyProcessed);
        canvas_->button(
            layout.preserve,
            settings_.preserveDirectoryStructure ? "保留目录✓" : "保留目录",
            colors, settings_.preserveDirectoryStructure);
        canvas_->button(
            layout.overwrite, settings_.overwrite ? "覆盖文件✓" : "保留文件",
            colors, settings_.overwrite);

        canvas_->card(layout.queue, colors);
        canvas_->text(
            layout.queue.x + s(20), layout.queue.y + s(34),
            "处理队列", colors.text, 1);
        const std::string queueSummary =
            "总计 " + std::to_string(snapshot.totalTasks) +
            " · 运行 " + std::to_string(snapshot.runningTasks) +
            " · 完成 " + std::to_string(snapshot.completedTasks) +
            " · 失败 " + std::to_string(snapshot.failedTasks);
        canvas_->text(
            layout.queue.x + layout.queue.width - s(330),
            layout.queue.y + s(34), queueSummary, colors.muted, 0);

        const auto taskRows = rows(snapshot);
        const int rowHeight = s(68);
        const int listTop = layout.queue.y + s(54);
        const int visibleRows = std::max(
            1, (layout.queue.height - s(68)) / rowHeight);
        const int maximumScroll = std::max(
            0, static_cast<int>(taskRows.size()) - visibleRows);
        scrollOffset_ = std::clamp(scrollOffset_, 0, maximumScroll);

        if (taskRows.empty()) {
            canvas_->text(
                layout.queue.x + s(30), listTop + s(55),
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
                canvas_->text(
                    layout.queue.x + titleIndent, rowY + s(20),
                    title, colors.text, childRow ? 0 : 1);

                const TaskState state = childRow ? item.state : task.state;
                const double progress = childRow ? item.progress : task.progress;
                const char* stateColor =
                    state == TaskState::Failed ? colors.red :
                    (state == TaskState::Completed || state == TaskState::Skipped)
                        ? colors.green : colors.muted;
                canvas_->text(
                    layout.queue.x + layout.queue.width - s(150),
                    rowY + s(20), taskStateName(state), stateColor, 0);
                canvas_->progress(
                    {layout.queue.x + s(20), rowY + s(36),
                     layout.queue.width - s(180), s(7)},
                    progress, colors);

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

                canvas_->text(
                    layout.queue.x + layout.queue.width - s(148),
                    rowY + s(56), detail, colors.muted, 0);
                canvas_->line(
                    layout.queue.x + s(18), rowY + rowHeight - s(2),
                    layout.queue.x + layout.queue.width - s(18),
                    rowY + rowHeight - s(2), colors.border);
            }
        }

        canvas_->card(layout.overallCard, colors);
        canvas_->text(
            layout.overallCard.x + s(22), layout.overallCard.y + s(36),
            "批量处理进度", colors.text, 1);
        canvas_->text(
            layout.overallCard.x + s(22), layout.overallCard.y + s(76),
            snapshot.status, colors.text, 0);
        canvas_->text(
            layout.overallCard.x + s(22), layout.overallCard.y + s(102),
            snapshot.detail, colors.muted, 0);
        canvas_->progress(layout.overallTrack, snapshot.overallProgress, colors);

        std::ostringstream percentage;
        percentage.setf(std::ios::fixed);
        percentage.precision(1);
        percentage << snapshot.overallProgress * 100.0 << "%";
        canvas_->text(
            layout.overallCard.x + s(22), layout.overallCard.y + s(178),
            percentage.str(), colors.accent, 1);
        canvas_->text(
            layout.overallCard.x + s(130), layout.overallCard.y + s(178),
            "完成 " + std::to_string(snapshot.completedTasks + snapshot.skippedTasks) +
                " / " + std::to_string(snapshot.totalTasks),
            colors.muted, 0);

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
            height_ - infoY - s(28)
        };
        canvas_->card(information, colors);
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
            "音频码率  " + std::to_string(settings_.audioBitrateKbps) +
                " kbps（按容器选择编码）",
            colors.text, 0);
        canvas_->text(
            information.x + s(22), information.y + s(198),
            settings_.remoteHost.empty()
                ? "计算设备  本机"
                : "计算设备  远程 · " + settings_.remoteHost,
            colors.text, 0);
        canvas_->text(
            information.x + s(22), information.y + s(228),
            snapshot.resourcePlan.explanation, colors.muted, 0);
        canvas_->text(
            information.x + s(22), information.y + s(270),
            "支持 H.264、H.265、AV1。ZIP 与 PPTX 会保留其他文件并更新视频引用。",
            colors.muted, 0);

        if (!snapshot.error.empty()) {
            canvas_->text(
                information.x + s(22),
                information.y + information.height - s(45),
                snapshot.error, colors.red, 0);
        }

        canvas_->flush();
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
            width_, height_, scale_, static_cast<int>(visibleBackends.size()));

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
        if (contains(layout.crfValue, x, y)) {
            editingCrf_ = true;
            crfEditText_.clear();
            XSetInputFocus(display_, window_, RevertToParent, CurrentTime);
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
            const auto capabilities = controller_.snapshot().capabilities;
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

        for (std::size_t index = 0; index < visibleBackends.size(); ++index) {
            if (contains(layout.backend[index], x, y)) {
                settings_.backend = visibleBackends[index];
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
        for (std::size_t index = 0; index < audioRates.size(); ++index) {
            if (contains(layout.audio[index], x, y)) {
                settings_.audioBitrateKbps = audioRates[index];
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

        const int crfTop = layout.crfTrack.y - s(10);
        const int crfBottom = layout.crfTrack.y + s(18);
        const int crfRight = layout.crfTrack.x + layout.crfTrack.width;
        if (y >= crfTop && y <= crfBottom &&
            x >= layout.crfTrack.x && x <= crfRight) {
            draggingCrf_ = true;
            commitCrf(x, layout);
            return;
        }

        if (y >= layout.queue.y + s(54) &&
            x >= layout.queue.x &&
            x <= layout.queue.x + layout.queue.width) {
            const int row = (y - (layout.queue.y + s(54))) / s(68) + scrollOffset_;
            const auto taskRows = rows(controller_.snapshot());
            if (row >= 0 && row < static_cast<int>(taskRows.size())) {
                selectedTask_ = taskRows[static_cast<std::size_t>(row)].first.id;
            }
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
                const int offset = event.xbutton.button == Button4 ? -3 : 3;
                scrollOffset_ = std::max(0, scrollOffset_ + offset);
                return false;
            }

            click(event.xbutton.x, event.xbutton.y);
            return false;
        }

        if (event.type == MotionNotify && draggingCrf_) {
            commitCrf(event.xmotion.x, makeLayout(width_, height_, scale_));
            return false;
        }
        if (event.type == ButtonRelease) {
            draggingCrf_ = false;
            return false;
        }

        if (event.type == KeyPress) {
            const KeySym key = XLookupKeysym(&event.xkey, 0);
            const bool controlPressed = (event.xkey.state & ControlMask) != 0;

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

            if (controlPressed && key == XK_o) {
                addFiles();
                return false;
            }
            if (controlPressed && key == XK_d) {
                controller_.clearCompleted();
                return false;
            }
            if (controlPressed && key == XK_Return) {
                startOrCancel();
                return false;
            }
            if (key == XK_Delete && selectedTask_ != 0) {
                controller_.removeTask(selectedTask_);
                return false;
            }
            if (key == XK_Escape) {
                return true;
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
    bool editingCrf_{false};
    std::string crfEditText_;
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
