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
    const int optionGap=s(6),optionWidth=(leftW-s(40)-optionGap*4)/5,optionY=sy+s(462);
    l.parallel={leftX+s(20),optionY,optionWidth,s(32)};
    l.recursive={leftX+s(20)+optionWidth+optionGap,optionY,optionWidth,s(32)};
    l.skip={leftX+s(20)+(optionWidth+optionGap)*2,optionY,optionWidth,s(32)};
    l.preserve={leftX+s(20)+(optionWidth+optionGap)*3,optionY,optionWidth,s(32)};
    l.overwrite={leftX+s(20)+(optionWidth+optionGap)*4,optionY,optionWidth,s(32)};
    l.queue={leftX,sy+s(510),leftW,height-(sy+s(510))-pad};
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
    XftCanvas(Display* d, Window w, double scale)
        : display_(d),window_(w),screen_(DefaultScreen(d)),colormap_(DefaultColormap(d,screen_)),visual_(DefaultVisual(d,screen_)) {
        draw_=XftDrawCreate(display_,window_,visual_,colormap_);
        const int normal=std::max(13,static_cast<int>(std::lround(14*scale)));
        const int small=std::max(11,static_cast<int>(std::lround(12*scale)));
        const int title=std::max(20,static_cast<int>(std::lround(25*scale)));
        const std::string np="Noto Sans CJK SC:pixelsize="+std::to_string(normal);
        const std::string sp="Noto Sans CJK SC:pixelsize="+std::to_string(small);
        const std::string tp="Noto Sans CJK SC:style=Bold:pixelsize="+std::to_string(title);
        normal_=XftFontOpenName(display_,screen_,np.c_str());small_=XftFontOpenName(display_,screen_,sp.c_str());title_=XftFontOpenName(display_,screen_,tp.c_str());
        if(!normal_)normal_=XftFontOpenName(display_,screen_,"sans:pixelsize=14");
        if(!small_) small_=normal_;
        if(!title_) title_=normal_;
    }
    ~XftCanvas(){if(draw_)XftDrawDestroy(draw_);if(gc_)XFreeGC(display_,gc_);if(title_&&title_!=normal_&&title_!=small_)XftFontClose(display_,title_);if(small_&&small_!=normal_)XftFontClose(display_,small_);if(normal_)XftFontClose(display_,normal_);}
    void fill(Rect r,const char* color){XSetForeground(display_,gc(),pixel(color));XFillRectangle(display_,window_,gc(),r.x,r.y,std::max(0,r.width),std::max(0,r.height));}
    void line(int x1,int y1,int x2,int y2,const char* color){XSetForeground(display_,gc(),pixel(color));XDrawLine(display_,window_,gc(),x1,y1,x2,y2);}
    void rounded(Rect r,const char* color,int radius=9){if(r.width<=0||r.height<=0)return;const int d=radius*2;XSetForeground(display_,gc(),pixel(color));XFillRectangle(display_,window_,gc(),r.x+radius,r.y,std::max(0,r.width-d),r.height);XFillRectangle(display_,window_,gc(),r.x,r.y+radius,r.width,std::max(0,r.height-d));XFillArc(display_,window_,gc(),r.x,r.y,d,d,0,360*64);XFillArc(display_,window_,gc(),r.x+r.width-d,r.y,d,d,0,360*64);XFillArc(display_,window_,gc(),r.x,r.y+r.height-d,d,d,0,360*64);XFillArc(display_,window_,gc(),r.x+r.width-d,r.y+r.height-d,d,d,0,360*64);}
    void card(Rect r,const Palette& c){rounded({r.x,r.y+2,r.width,r.height},c.border,12);rounded(r,c.card,12);}
    void text(int x,int baseline,const std::string& value,const char* color,int size=1){XColor xc{};XParseColor(display_,colormap_,color,&xc);XftColor ink{};ink.pixel=pixel(color);ink.color={xc.red,xc.green,xc.blue,65535};_XftFont* font=size==0?small_:size==2?title_:normal_;if(!font)return;XftDrawStringUtf8(draw_,&ink,font,x,baseline,reinterpret_cast<const unsigned char*>(value.data()),static_cast<int>(value.size()));}
    void button(Rect r,const std::string& label,const Palette& c,bool active=false,bool disabled=false){rounded(r,disabled?c.track:active?c.accent:c.input,8);if(!active&&!disabled)line(r.x+8,r.y+r.height-1,r.x+r.width-8,r.y+r.height-1,c.border);const int estimate=static_cast<int>(label.size())*7/2;text(r.x+std::max(8,(r.width-estimate)/2),r.y+r.height/2+5,label,active&&!disabled?c.white:c.text,0);}
    void input(Rect r,const std::string& value,const Palette& c,bool focused=false,bool secret=false){
        rounded(r,c.input,8);const char* border=focused?c.accent:c.border;
        line(r.x+7,r.y,r.x+r.width-7,r.y,border);line(r.x+7,r.y+r.height-1,r.x+r.width-7,r.y+r.height-1,border);
        line(r.x,r.y+7,r.x,r.y+r.height-7,border);line(r.x+r.width-1,r.y+7,r.x+r.width-1,r.y+r.height-7,border);
        text(r.x+10,r.y+r.height/2+5,secret?std::string(value.size(),'*'):value,c.text,0);
    }
    void progress(Rect r,double value,const Palette& c){rounded(r,c.track,5);const int fill=static_cast<int>(std::lround(r.width*std::clamp(value,0.0,1.0)));if(fill>0)rounded({r.x,r.y,fill,r.height},c.accent,5);}
    void flush(){XFlush(display_);}
private:
    GC gc(){if(!gc_)gc_=XCreateGC(display_,window_,0,nullptr);return gc_;}
    unsigned long pixel(const char* name){auto it=pixels_.find(name);if(it!=pixels_.end())return it->second;XColor color{};XParseColor(display_,colormap_,name,&color);XAllocColor(display_,colormap_,&color);pixels_[name]=color.pixel;return color.pixel;}
    Display* display_{};Window window_{};int screen_{};Colormap colormap_{};Visual* visual_{};_XftDraw* draw_{};_XftFont* normal_{};_XftFont* small_{};_XftFont* title_{};GC gc_{};std::map<std::string,unsigned long> pixels_;
};

std::string basename(const std::string& path){return std::filesystem::path(path).filename().string();}
std::string formatTime(const std::optional<double>& value){if(!value||*value<0)return "--:--:--";const auto total=static_cast<unsigned long long>(*value);char buffer[32]{};std::snprintf(buffer,sizeof(buffer),"%02llu:%02llu:%02llu",total/3600,(total%3600)/60,total%60);return buffer;}
std::string formatTaskSize(std::uint64_t inputBytes,std::uint64_t outputBytes){if(inputBytes==0)return {};auto size=[](std::uint64_t value){char b[40]{};const double n=static_cast<double>(value);if(n>=1024.0*1024.0*1024.0)std::snprintf(b,sizeof(b),"%.2f GiB",n/(1024.0*1024.0*1024.0));else if(n>=1024.0*1024.0)std::snprintf(b,sizeof(b),"%.1f MiB",n/(1024.0*1024.0));else std::snprintf(b,sizeof(b),"%.0f KiB",n/1024.0);return std::string(b);};std::string result="输入 "+size(inputBytes);if(outputBytes>0){result+=" → 输出 "+size(outputBytes);if(inputBytes>outputBytes){const double saved=(1.0-static_cast<double>(outputBytes)/static_cast<double>(inputBytes))*100.0;char b[32]{};std::snprintf(b,sizeof(b)," · 节省 %.1f%%",saved);result+=b;}}if(result.size()>136){result.resize(133);result+="...";}return result;}
std::string formatProgressLine(const ConversionProgress& p,const std::string& fallback){if(!p.frame&&!p.currentTimeSeconds&&!p.fps&&!p.speed&&!p.bitrate&&!p.totalSizeBytes)return fallback;std::string line;auto add=[&](const std::string& v){if(!line.empty())line+=" · ";line+=v;};if(p.frame)add("帧 "+std::to_string(*p.frame));if(p.currentTimeSeconds||p.durationSeconds)add(formatTime(p.currentTimeSeconds)+" / "+formatTime(p.durationSeconds));if(p.fps){char b[32]{};std::snprintf(b,sizeof(b),"%.1f FPS",*p.fps);add(b);}if(p.speed){char b[32]{};std::snprintf(b,sizeof(b),"%.2fx",*p.speed);add(b);}if(p.etaSeconds)add("剩余 "+formatTime(p.etaSeconds));if(p.bitrate)add("码率 "+*p.bitrate);if(p.totalSizeBytes)add("已写入 "+std::to_string(*p.totalSizeBytes/1024)+" KiB");if(line.size()>136){line.resize(133);line+="...";}return line;}
std::string trim(const std::string& value){const auto first=value.find_first_not_of(" \t\r\n");if(first==std::string::npos)return {};const auto last=value.find_last_not_of(" \t\r\n");return value.substr(first,last-first+1);}
std::string percentDecode(std::string value){std::string out;for(std::size_t i=0;i<value.size();++i){if(value[i]=='%'&&i+2<value.size()){try{const int v=std::stoi(value.substr(i+1,2),nullptr,16);out.push_back(static_cast<char>(v));i+=2;}catch(...){out.push_back(value[i]);}}else out.push_back(value[i]);}return out;}

class LinuxWindow {
public:
    explicit LinuxWindow(AppController& controller):controller_(controller){}
    int run(){display_=XOpenDisplay(nullptr);if(!display_)return 1;screen_=DefaultScreen(display_);scale_=detectScale();width_=1500*scale_;height_=1020*scale_;
        window_=XCreateSimpleWindow(display_,RootWindow(display_,screen_),0,0,width_,height_,0,BlackPixel(display_,screen_),WhitePixel(display_,screen_));
        XStoreName(display_,window_,"FFmpeg Studio");XClassHint hint{};hint.res_name=const_cast<char*>("ffmpeg-studio");hint.res_class=const_cast<char*>("FFmpegStudio");XSetClassHint(display_,window_,&hint);
        XSelectInput(display_,window_,ExposureMask|StructureNotifyMask|ButtonPressMask|ButtonReleaseMask|PointerMotionMask|KeyPressMask|PropertyChangeMask);
        wmDelete_=XInternAtom(display_,"WM_DELETE_WINDOW",False);XSetWMProtocols(display_,window_,&wmDelete_,1);
        setDnd();XSizeHints sizeHints{};sizeHints.flags=PMinSize;sizeHints.min_width=static_cast<int>(1180*scale_);sizeHints.min_height=static_cast<int>(820*scale_);XSetWMNormalHints(display_,window_,&sizeHints);
        XMapWindow(display_,window_);canvas_=std::make_unique<XftCanvas>(display_,window_,scale_);
        bool quit=false;while(!quit){while(XPending(display_)){XEvent event{};XNextEvent(display_,&event);if(handle(event))quit=true;}pollfd fd{ConnectionNumber(display_),POLLIN,0};poll(&fd,1,120);if(fd.revents&POLLIN)continue;draw();}
        controller_.shutdown();canvas_.reset();XDestroyWindow(display_,window_);XCloseDisplay(display_);return 0;}
private:
    double detectScale(){double dpi=96.0;const char* resources=XResourceManagerString(display_);if(resources){std::string text(resources);auto pos=text.find("Xft.dpi:");if(pos!=std::string::npos){try{dpi=std::stod(text.substr(pos+8));}catch(...){}}}return std::clamp(dpi/96.0,0.8,2.5);}
    int s(int v)const{return static_cast<int>(std::lround(v*scale_));}
    void setDnd(){Atom aware=XInternAtom(display_,"XdndAware",False);unsigned long version=5;XChangeProperty(display_,window_,aware,XA_ATOM,32,PropModeReplace,reinterpret_cast<unsigned char*>(&version),1);xdndEnter_=XInternAtom(display_,"XdndEnter",False);xdndPosition_=XInternAtom(display_,"XdndPosition",False);xdndStatus_=XInternAtom(display_,"XdndStatus",False);xdndDrop_=XInternAtom(display_,"XdndDrop",False);xdndFinished_=XInternAtom(display_,"XdndFinished",False);xdndSelection_=XInternAtom(display_,"XdndSelection",False);uriList_=XInternAtom(display_,"text/uri-list",False);}
    std::vector<std::string> chooseFiles(bool archiveOnly=false){
        const std::string filter=archiveOnly?"--file-filter=ZIP and PowerPoint | *.zip *.pptx":"--file-filter=Media and archives | *.mp4 *.m4v *.mov *.mkv *.avi *.webm *.wmv *.flv *.mpg *.mpeg *.3gp *.ts *.m2ts *.mts *.zip *.pptx";
        ProcessResult result=runProcess({"zenity","--file-selection","--multiple","--separator=\n","--title=添加媒体文件",filter});
        if(result.exitCode!=0){result=runProcess({"kdialog","--getopenfilename",".",archiveOnly?"*.zip *.pptx":"*.mp4 *.mkv *.mov *.avi *.webm *.zip *.pptx","--multiple","--separate-output"});}
        std::vector<std::string> paths;std::istringstream stream(result.output);std::string line;while(std::getline(stream,line)){line=trim(line);if(!line.empty())paths.push_back(line);}return paths;
    }
    void importQueueDialog(){ProcessResult result=runProcess({"zenity","--file-selection","--title=导入任务清单","--file-filter=FFmpeg Studio Queue | *.ffq *.txt"});if(result.exitCode!=0)result=runProcess({"kdialog","--getopenfilename",".","*.ffq *.txt","导入任务清单"});const auto path=trim(result.output);if(path.empty())return;std::string error;const auto added=controller_.importQueue(path,error);if(!error.empty())std::cerr<<error<<'\n';else if(added==0)std::cerr<<"任务清单中没有新增任务。\n";}
    void exportQueueDialog(){ProcessResult result=runProcess({"zenity","--file-selection","--save","--confirm-overwrite","--title=导出任务清单","--file-filter=FFmpeg Studio Queue | *.ffq"});if(result.exitCode!=0)result=runProcess({"kdialog","--getsavefilename",".","*.ffq","导出任务清单"});auto path=trim(result.output);if(path.empty())return;if(std::filesystem::path(path).extension().empty())path+=".ffq";std::string error;if(!controller_.exportQueue(path,error))std::cerr<<error<<'\n';}
    void chooseFolder(){ProcessResult result=runProcess({"zenity","--file-selection","--directory","--title=选择输入文件夹"});if(result.exitCode!=0)result=runProcess({"kdialog","--getexistingdirectory",".","选择输入文件夹"});const auto path=trim(result.output);if(!path.empty())controller_.addInputs({path});}
    void chooseOutput(){ProcessResult result=runProcess({"zenity","--file-selection","--directory","--title=选择输出文件夹"});if(result.exitCode!=0)result=runProcess({"kdialog","--getexistingdirectory",".","选择输出文件夹"});const auto path=trim(result.output);if(!path.empty()){settings_.outputDirectory=path;updateSettings();}}
    void updateSettings(){controller_.updateSettings(settings_);}
    void commitCrfText(){if(!editingCrf_)return;if(!crfEditText_.empty()){try{settings_.crf=std::clamp(std::stoi(crfEditText_),0,crfMaximum(settings_.codec));updateSettings();}catch(...){}}editingCrf_=false;crfEditText_.clear();}
    void addFiles(bool archiveOnly=false){const auto files=chooseFiles(archiveOnly);if(!files.empty())controller_.addInputs(files);}
    void startOrCancel(){const auto snap=controller_.snapshot();if(!snap.running)controller_.startBatch();else if(snap.paused)controller_.resumeBatch();else controller_.pauseBatch();}
    std::vector<std::pair<BatchTaskSnapshot,BatchItemSnapshot>> rows(const AppSnapshot& snap)const{std::vector<std::pair<BatchTaskSnapshot,BatchItemSnapshot>> out;for(const auto& task:snap.tasks){if(task.kind==TaskKind::Video){BatchItemSnapshot item;if(!task.items.empty())item=task.items.front();else{item.name=basename(task.inputPath);item.state=task.state;item.progress=task.progress;item.detail=task.detail;}out.emplace_back(task,item);}else{BatchItemSnapshot parent;parent.name=basename(task.inputPath);parent.state=task.state;parent.progress=task.progress;parent.detail=task.detail;out.emplace_back(task,parent);for(const auto& item:task.items)out.emplace_back(task,item);}}return out;}
    void draw(){const auto snap=controller_.snapshot();const auto backends=availableBackends(snap.capabilities,settings_.codec);const auto l=makeLayout(width_,height_,scale_,static_cast<int>(backends.size()));const auto c=palette(dark_);canvas_->fill({0,0,width_,height_},c.background);
        canvas_->text(s(30),s(50),"FFmpeg Studio",c.text,2);canvas_->text(s(31),s(82),"批量视频压缩 · ZIP / PPTX 媒体处理 · CPU 与硬件编码",c.muted,0);canvas_->button(l.ssh,"SSH 服务器",c,!settings_.remoteHost.empty());canvas_->button(l.theme,dark_?"浅色外观":"深色外观",c);
        canvas_->button(l.addFiles,"添加文件",c,true);canvas_->button(l.addFolder,"添加文件夹",c);canvas_->button(l.addArchive,"添加 ZIP / PPTX",c);canvas_->button(l.importQueue,"导入清单",c);canvas_->button(l.exportQueue,"导出清单",c);canvas_->button(l.removeSelected,"移除选中",c);canvas_->button(l.clearDone,"清除已完成",c);
        const int leftX=s(28),top=s(168);canvas_->card({leftX,top,l.queue.width,s(500)},c);canvas_->text(leftX+s(20),top+s(32),"编码设置",c.text,1);canvas_->text(leftX+s(20),top+s(64),"输出文件夹",c.muted,0);
        const std::string output=settings_.outputDirectory.empty()?"默认：与每个源文件保存在同一文件夹":settings_.outputDirectory;
        canvas_->rounded(l.outputField,c.input,8);canvas_->text(l.outputField.x+s(12),l.outputField.y+l.outputField.height/2+s(5),output,c.text,0);canvas_->button(l.outputBrowse,"选择目录",c);
        canvas_->text(leftX+s(20),top+s(98),"输出容器格式",c.muted,0);const std::array<std::string,2> containers{"MP4","与源文件相同"};for(int i=0;i<2;++i)canvas_->button(l.container[i],containers[i],c,static_cast<int>(settings_.outputContainer)==i);
        canvas_->text(leftX+s(20),top+s(149),"视频编码格式",c.muted,0);const std::array<std::string,3> codecs{"H.264","H.265","AV1"};for(int i=0;i<3;++i)canvas_->button(l.codec[i],codecs[i],c,static_cast<int>(settings_.codec)==i);
        canvas_->text(leftX+s(20),top+s(197),"编码设备",c.muted,0);const std::array<std::string,4> backendNames{"CPU","NVIDIA NVENC","Intel QSV","AMD AMF"};
        for(std::size_t i=0;i<backends.size();++i){const int labelIndex=backends[i]==EncoderBackend::CPU?0:backends[i]==EncoderBackend::NVENC?1:backends[i]==EncoderBackend::QSV?2:3;canvas_->button(l.backend[i],backendNames[labelIndex],c,settings_.backend==backends[i]);}
        if(backends.empty())canvas_->text(leftX+s(20),top+s(181),"当前 FFmpeg 环境没有可用的视频编码器",c.red,0);
        canvas_->text(leftX+s(20),top+s(245),"CRF / 目标质量",c.muted,0);canvas_->progress(l.crfTrack,static_cast<double>(settings_.crf)/crfMaximum(settings_.codec),c);const int knob=l.crfTrack.x+static_cast<int>(std::lround(l.crfTrack.width*static_cast<double>(settings_.crf)/crfMaximum(settings_.codec)));canvas_->rounded({knob-s(7),l.crfTrack.y-s(5),s(14),s(20)},c.accent,7);canvas_->button(l.crfValue,editingCrf_?(crfEditText_.empty()?"输入":crfEditText_):std::to_string(settings_.crf),c,true);
        canvas_->text(leftX+s(20),top+s(296),"压缩级别",c.muted,0);const std::array<std::string,8> levels{"极速","非常快","快速","均衡","慢速","较慢","很慢","极致压缩"};for(int i=0;i<8;++i)canvas_->button(l.compression[i],levels[i],c,static_cast<int>(settings_.compression)==i);
        canvas_->text(leftX+s(20),top+s(383),"音频码率",c.muted,0);const std::array<int,6> rates{96,128,160,192,256,320};for(int i=0;i<6;++i)canvas_->button(l.audio[i],std::to_string(rates[i])+" kbps",c,settings_.audioBitrateKbps==rates[i]);
        canvas_->button(l.parallel,settings_.maxParallelJobs==0?"并行：自动":"并行："+std::to_string(settings_.maxParallelJobs),c);canvas_->button(l.recursive,settings_.recursive?"递归扫描✓":"递归扫描",c,settings_.recursive);canvas_->button(l.skip,settings_.skipAlreadyProcessed?"跳过重复✓":"跳过重复",c,settings_.skipAlreadyProcessed);canvas_->button(l.preserve,settings_.preserveDirectoryStructure?"保留目录✓":"保留目录",c,settings_.preserveDirectoryStructure);canvas_->button(l.overwrite,settings_.overwrite?"覆盖文件✓":"保留文件",c,settings_.overwrite);
        canvas_->card(l.queue,c);canvas_->text(l.queue.x+s(20),l.queue.y+s(34),"处理队列",c.text,1);canvas_->text(l.queue.x+l.queue.width-s(330),l.queue.y+s(34),"总计 "+std::to_string(snap.totalTasks)+" · 运行 "+std::to_string(snap.runningTasks)+" · 完成 "+std::to_string(snap.completedTasks)+" · 失败 "+std::to_string(snap.failedTasks),c.muted,0);
        const auto taskRows=rows(snap);const int rowH=s(68),listTop=l.queue.y+s(54),visible=std::max(1,(l.queue.height-s(68))/rowH);const int maxScroll=std::max(0,static_cast<int>(taskRows.size())-visible);scrollOffset_=std::clamp(scrollOffset_,0,maxScroll);
        if(taskRows.empty())canvas_->text(l.queue.x+s(30),listTop+s(55),"将视频、ZIP 或 PPTX 文件拖到窗口中，或使用上方按钮添加文件和文件夹。",c.muted,1);
        else for(int i=0;i<visible&&i+scrollOffset_<static_cast<int>(taskRows.size());++i){const auto& [task,item]=taskRows[static_cast<std::size_t>(i+scrollOffset_)];const int y=listTop+i*rowH;const bool child=task.kind!=TaskKind::Video&&item.name!=basename(task.inputPath);if(selectedTask_==task.id)canvas_->rounded({l.queue.x+s(10),y+s(2),l.queue.width-s(20),rowH-s(4)},c.accentSoft,7);const std::string title=child?"↳ "+item.name:item.name+"  ·  "+taskKindName(task.kind);canvas_->text(l.queue.x+(child?s(38):s(20)),y+s(20),title,c.text,child?0:1);const auto state=child?item.state:task.state;const double progress=child?item.progress:task.progress;canvas_->text(l.queue.x+l.queue.width-s(150),y+s(20),taskStateName(state),state==TaskState::Failed?c.red:(state==TaskState::Completed||state==TaskState::Skipped)?c.green:c.muted,0);canvas_->progress({l.queue.x+s(20),y+s(36),l.queue.width-s(180),s(7)},progress,c);std::string detail=child?formatProgressLine(item.progressInfo,item.detail):(task.kind==TaskKind::Video&&!task.items.empty()?formatProgressLine(task.items[0].progressInfo,task.detail):task.detail);const std::string sizeLine=child?formatTaskSize(item.inputSizeBytes,item.outputSizeBytes):formatTaskSize(task.inputSizeBytes,task.outputSizeBytes);if(!sizeLine.empty()&&(state==TaskState::Completed||state==TaskState::Skipped))detail=sizeLine;canvas_->text(l.queue.x+l.queue.width-s(148),y+s(56),detail,c.muted,0);canvas_->line(l.queue.x+s(18),y+rowH-s(2),l.queue.x+l.queue.width-s(18),y+rowH-s(2),c.border);}
        canvas_->card(l.overallCard,c);canvas_->text(l.overallCard.x+s(22),l.overallCard.y+s(36),"批量处理进度",c.text,1);canvas_->text(l.overallCard.x+s(22),l.overallCard.y+s(76),snap.status,c.text,0);canvas_->text(l.overallCard.x+s(22),l.overallCard.y+s(102),snap.detail,c.muted,0);canvas_->progress(l.overallTrack,snap.overallProgress,c);std::ostringstream percent;percent.setf(std::ios::fixed);percent.precision(1);percent<<snap.overallProgress*100.0<<"%";canvas_->text(l.overallCard.x+s(22),l.overallCard.y+s(178),percent.str(),c.accent,1);canvas_->text(l.overallCard.x+s(130),l.overallCard.y+s(178),"完成 "+std::to_string(snap.completedTasks+snap.skippedTasks)+" / "+std::to_string(snap.totalTasks),c.muted,0);if(!snap.running)canvas_->button(l.start,"开始批量处理",c,true,snap.tasks.empty()||!snap.capabilities.supports(settings_.codec,settings_.backend));else{canvas_->button(l.pause,snap.paused?"继续处理":"暂停处理",c,true);canvas_->button(l.cancel,"取消任务",c);}
        const int infoY=l.overallCard.y+l.overallCard.height+s(18);const Rect info{l.overallCard.x,infoY,l.overallCard.width,height_-infoY-s(28)};canvas_->card(info,c);canvas_->text(info.x+s(22),info.y+s(38),"设备与编码摘要",c.text,1);canvas_->text(info.x+s(22),info.y+s(78),"格式  "+codecName(settings_.codec)+" · "+outputContainerName(settings_.outputContainer),c.text,0);canvas_->text(info.x+s(22),info.y+s(108),"编码器  "+backendName(settings_.backend),c.text,0);canvas_->text(info.x+s(22),info.y+s(138),"CRF  "+std::to_string(settings_.crf)+" · "+compressionName(settings_.compression),c.text,0);canvas_->text(info.x+s(22),info.y+s(168),"音频码率  "+std::to_string(settings_.audioBitrateKbps)+" kbps（按容器选择编码）",c.text,0);
        canvas_->text(info.x+s(22),info.y+s(198),settings_.remoteHost.empty()?"计算设备  本机":"计算设备  远程 · "+settings_.remoteHost,c.text,0);
        canvas_->text(info.x+s(22),info.y+s(228),snap.resourcePlan.explanation,c.muted,0);canvas_->text(info.x+s(22),info.y+s(270),"支持 H.264、H.265、AV1。ZIP 与 PPTX 会保留其他文件并更新视频引用。",c.muted,0);if(!snap.error.empty())canvas_->text(info.x+s(22),info.y+info.height-s(45),snap.error,c.red,0);canvas_->flush();}
    void commitCrf(int x,const Layout& l){editingCrf_=false;crfEditText_.clear();const double f=std::clamp(static_cast<double>(x-l.crfTrack.x)/std::max(1,l.crfTrack.width),0.0,1.0);settings_.crf=static_cast<int>(std::lround(f*crfMaximum(settings_.codec)));updateSettings();}
    void click(int x,int y){const auto snap=controller_.snapshot();const auto visibleBackends=availableBackends(snap.capabilities,settings_.codec);const auto l=makeLayout(width_,height_,scale_,static_cast<int>(visibleBackends.size()));if(editingCrf_&&!contains(l.crfValue,x,y))commitCrfText();if(contains(l.ssh,x,y)){if(!snap.running)manageSshServers();return;}if(contains(l.theme,x,y)){dark_=!dark_;return;}if(contains(l.addFiles,x,y)){addFiles();return;}if(contains(l.addFolder,x,y)){chooseFolder();return;}if(contains(l.addArchive,x,y)){addFiles(true);return;}if(contains(l.importQueue,x,y)){importQueueDialog();return;}if(contains(l.exportQueue,x,y)){exportQueueDialog();return;}if(contains(l.removeSelected,x,y)){if(selectedTask_)controller_.removeTask(selectedTask_);return;}if(contains(l.clearDone,x,y)){controller_.clearCompleted();return;}if(contains(l.outputBrowse,x,y)){chooseOutput();return;}if(!snap.running&&contains(l.start,x,y)){startOrCancel();return;}if(snap.running&&contains(l.pause,x,y)){if(snap.paused)controller_.resumeBatch();else controller_.pauseBatch();return;}if(snap.running&&contains(l.cancel,x,y)){controller_.cancelBatch();return;}if(contains(l.crfValue,x,y)){editingCrf_=true;crfEditText_.clear();XSetInputFocus(display_,window_,RevertToParent,CurrentTime);return;}
        for(int i=0;i<2;++i)if(contains(l.container[i],x,y)){settings_.outputContainer=static_cast<OutputContainer>(i);updateSettings();return;}
        for(int i=0;i<3;++i)if(contains(l.codec[i],x,y)){settings_.codec=static_cast<VideoCodec>(i);settings_.crf=defaultCrf(settings_.codec);const auto caps=controller_.snapshot().capabilities;if(!caps.supports(settings_.codec,settings_.backend)){for(const auto candidate:{EncoderBackend::CPU,EncoderBackend::NVENC,EncoderBackend::QSV,EncoderBackend::AMF})if(caps.supports(settings_.codec,candidate)){settings_.backend=candidate;break;}}updateSettings();return;}
        for(std::size_t i=0;i<visibleBackends.size();++i)if(contains(l.backend[i],x,y)){settings_.backend=visibleBackends[i];updateSettings();return;}
        for(int i=0;i<8;++i)if(contains(l.compression[i],x,y)){settings_.compression=static_cast<CompressionLevel>(i);updateSettings();return;}
        const std::array<int,6> rates{96,128,160,192,256,320};for(int i=0;i<6;++i)if(contains(l.audio[i],x,y)){settings_.audioBitrateKbps=rates[i];updateSettings();return;}
        if(contains(l.parallel,x,y)){const std::array<int,5> choices{0,1,2,4,8};auto it=std::find(choices.begin(),choices.end(),settings_.maxParallelJobs);settings_.maxParallelJobs=choices[(static_cast<std::size_t>(std::distance(choices.begin(),it))+1)%choices.size()];updateSettings();return;}
        if(contains(l.recursive,x,y)){settings_.recursive=!settings_.recursive;updateSettings();return;}if(contains(l.skip,x,y)){settings_.skipAlreadyProcessed=!settings_.skipAlreadyProcessed;updateSettings();return;}if(contains(l.preserve,x,y)){settings_.preserveDirectoryStructure=!settings_.preserveDirectoryStructure;updateSettings();return;}if(contains(l.overwrite,x,y)){settings_.overwrite=!settings_.overwrite;updateSettings();return;}
        if(y>=l.crfTrack.y-s(10)&&y<=l.crfTrack.y+s(18)&&x>=l.crfTrack.x&&x<=l.crfTrack.x+l.crfTrack.width){draggingCrf_=true;commitCrf(x,l);return;}
        if(y>=l.queue.y+s(54)&&x>=l.queue.x&&x<=l.queue.x+l.queue.width){const int row=(y-(l.queue.y+s(54)))/s(68)+scrollOffset_;const auto list=rows(controller_.snapshot());if(row>=0&&row<static_cast<int>(list.size()))selectedTask_=list[static_cast<std::size_t>(row)].first.id;}
    }
    std::vector<std::string> readDropSelection(){Atom actual{};int format=0;unsigned long count=0,after=0;unsigned char* data=nullptr;std::vector<std::string> paths;if(XGetWindowProperty(display_,window_,XInternAtom(display_,"XdndSelection",False),0,65536,True,uriList_,&actual,&format,&count,&after,&data)==Success&&data){std::string content(reinterpret_cast<char*>(data),count);XFree(data);std::istringstream lines(content);std::string line;while(std::getline(lines,line)){line=trim(line);if(line.empty()||line[0]=='#')continue;if(line.rfind("file://",0)==0)line=percentDecode(line.substr(7));if(!line.empty())paths.push_back(line);}}return paths;}
    bool handle(XEvent& e){if(e.type==Expose){draw();return false;}if(e.type==ConfigureNotify){width_=e.xconfigure.width;height_=e.xconfigure.height;return false;}if(e.type==ClientMessage){if(static_cast<Atom>(e.xclient.data.l[0])==wmDelete_)return true;const Atom type=static_cast<Atom>(e.xclient.message_type);if(type==xdndEnter_){dragSource_=static_cast<Window>(e.xclient.data.l[0]);return false;}if(type==xdndPosition_){XEvent reply{};reply.xclient.type=ClientMessage;reply.xclient.display=display_;reply.xclient.window=dragSource_;reply.xclient.message_type=xdndStatus_;reply.xclient.format=32;reply.xclient.data.l[0]=window_;reply.xclient.data.l[1]=1;reply.xclient.data.l[4]=static_cast<long>(XInternAtom(display_,"XdndActionCopy",False));XSendEvent(display_,dragSource_,False,NoEventMask,&reply);XFlush(display_);return false;}if(type==xdndDrop_){XConvertSelection(display_,xdndSelection_,uriList_,XInternAtom(display_,"XdndSelection",False),window_,static_cast<Time>(e.xclient.data.l[2]));return false;}}
        if(e.type==SelectionNotify){auto paths=readDropSelection();if(!paths.empty())controller_.addInputs(paths);XEvent reply{};reply.xclient.type=ClientMessage;reply.xclient.display=display_;reply.xclient.window=dragSource_;reply.xclient.message_type=xdndFinished_;reply.xclient.format=32;reply.xclient.data.l[0]=window_;reply.xclient.data.l[1]=paths.empty()?0:1;reply.xclient.data.l[2]=static_cast<long>(XInternAtom(display_,"XdndActionCopy",False));XSendEvent(display_,dragSource_,False,NoEventMask,&reply);XFlush(display_);return false;}
        if(e.type==ButtonPress){if(e.xbutton.button==Button4||e.xbutton.button==Button5){scrollOffset_=std::max(0,scrollOffset_+(e.xbutton.button==Button4?-3:3));return false;}click(e.xbutton.x,e.xbutton.y);return false;}
        if(e.type==MotionNotify&&draggingCrf_){commitCrf(e.xmotion.x,makeLayout(width_,height_,scale_));return false;}
        if(e.type==ButtonRelease){draggingCrf_=false;return false;}
        if(e.type==KeyPress){KeySym key=XLookupKeysym(&e.xkey,0);const bool ctrl=(e.xkey.state&ControlMask)!=0;if(editingCrf_){if(key==XK_Return||key==XK_KP_Enter)commitCrfText();else if(key==XK_BackSpace&&!crfEditText_.empty())crfEditText_.pop_back();else if(key==XK_Escape){editingCrf_=false;crfEditText_.clear();}else{char text[8]{};KeySym symbol{};const int length=XLookupString(&e.xkey,text,sizeof(text),&symbol,nullptr);for(int i=0;i<length&&crfEditText_.size()<3;++i)if(text[i]>='0'&&text[i]<='9')crfEditText_.push_back(text[i]);}return false;}if(ctrl&&key==XK_o){addFiles();return false;}if(ctrl&&key==XK_d){controller_.clearCompleted();return false;}if(ctrl&&key==XK_Return){startOrCancel();return false;}if(key==XK_Delete&&selectedTask_){controller_.removeTask(selectedTask_);return false;}if(key==XK_Escape)return true;}
        return false;}
    AppController& controller_;Display* display_{};int screen_{};Window window_{};double scale_{1.0};int width_{1500},height_{1020};std::unique_ptr<XftCanvas> canvas_;BatchSettings settings_;bool dark_{false},draggingCrf_{false},editingCrf_{false};std::string crfEditText_;int scrollOffset_{0};std::uint64_t selectedTask_{0};Atom wmDelete_{},xdndEnter_{},xdndPosition_{},xdndStatus_{},xdndDrop_{},xdndFinished_{},xdndSelection_{},uriList_{};Window dragSource_{};
};

} // namespace

int runNativeUi(AppController& controller){LinuxWindow window(controller);return window.run();}

} // namespace ffmpegui
