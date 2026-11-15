#include "kokaku_native_menu.hpp"
#include "kokaku_pc_balance.hpp"
#include "kokaku_control_icons.hpp"
#include "kokaku_controls_ui.hpp"
#include "kokaku_online.hpp"
#include "kokaku_net.hpp"
#include "ge_gpu_backend.hpp"
#include "vcs_config.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <chrono>
#include <ctime>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <vector>
#if defined(_WIN32)
#include <windows.h>
#endif

namespace kokaku {
namespace {
std::atomic<bool> receiving{};
std::mutex key_mutex;
std::vector<int> keys;
std::uint64_t frame_number{}, last_option_frame{};
std::uint64_t option_screen_frame{};
bool option_screen_seen{};
std::uint64_t navigation_frame{};
bool navigation_seen{};
bool navigation_title{};
bool option_seen{}, editing{}, capturing{};
int page{}, row{}, binding_slot{};
constexpr int page_count=5;
// Network page: saved to [Network], read by the game at its next start.
bool net_loaded{}, net_dirty{}, net_online{true}, typing{};
std::string net_name, typed;
bool default_uppercase() {
#if defined(_WIN32)
    return (GetKeyState(VK_SHIFT)<0)!=((GetKeyState(VK_CAPITAL)&1)!=0);
#else
    return false;
#endif
}
bool (*uppercase_state)()=default_uppercase;
Controls draft;
vcs::VcsConfiguration settings;
std::wstring message;
bool save_open{}, save_writing{}, overwrite{};
int save_row{};
std::uint32_t previous_buttons{};
std::filesystem::path save_root;
std::string save_game, save_base;
std::array<bool, 5> save_exists{};
std::array<std::wstring, 5> save_dates{};
constexpr std::uint32_t background=0xff30200d, foreground=0xffefdfc6, accent=0xffffdba2;

std::wstring wide(const std::string &s) { return std::wstring(s.begin(),s.end()); }
std::vector<int> take_keys() { std::lock_guard lock(key_mutex); auto result=std::move(keys); keys.clear(); return result; }
void result(psprecomp::AllegrexContext &ctx, int value) { ctx.set_gpr(2, static_cast<std::uint32_t>(value)); ctx.pc=ctx.gpr[31]; }
std::uint32_t raw_buttons(const std::vector<int> &input) {
    std::uint32_t buttons{};
    const auto controls=controls_snapshot();
    for (int key:input) {
        for(std::size_t i=0;i<actions.size();++i)
            if(actions[i].context==ControlContext::Menu && key &&
                (controls.keys[i][0]==key || controls.keys[i][1]==key)) buttons|=actions[i].button;
        // Fixed recovery keys keep the editor usable after clearing menu bindings.
        switch(key) {
        case 0x26:buttons|=0x10;break;case 0x28:buttons|=0x40;break;
        case 0x25:buttons|=0x80;break;case 0x27:buttons|=0x20;break;
        case 0x0d:buttons|=0x4000;break;case 0x1b:buttons|=0x2000;break;
        case 0x2e:buttons|=0x1000;break;
        case 9:buttons|=0x8000;break;
        }
    }
    return buttons;
}
void commit_controls() {
    std::string error;
    message=controls_commit(draft,error) ? L"Controls saved" : L"Unable to save controls";
}
std::string lower(std::string s) {
    for(char &c:s) if(c>='A' && c<='Z') c=static_cast<char>(c+32);
    const auto first=s.find_first_not_of(" \t\r"), last=s.find_last_not_of(" \t\r");
    return first==std::string::npos ? "" : s.substr(first,last-first+1);
}
bool commit_settings() {
    // Preserve comments, unrelated options and the diagnostic configuration path.
    std::map<std::string,std::map<std::string,std::string>> values{
        {"display",{{"fullscreen",settings.display.fullscreen?"true":"false"},{"showfps",settings.display.show_fps?"true":"false"}}},
        {"rendering",{{"internalresolutionmode","Custom"},{"internalwidth",std::to_string(settings.rendering.internal_width)},
                      {"internalheight",std::to_string(settings.rendering.internal_height)},{"msaa",std::to_string(settings.rendering.msaa)}}},
        {"audio",{{"volume",std::to_string(settings.audio.volume)}}},
        {"timing",{{"framerate",std::to_string(settings.timing.frame_rate)}}},
        {"gameplay",{{"pcbalance",settings.gameplay.pc_balance?"true":"false"}}}};
    if(net_dirty) values["network"]={{"online",net_online?"true":"false"},{"nickname",net_name}};
    const auto path=settings.source_path;
    if(path.empty()) return false;
    std::ifstream in(path); if(!in) return false;
    std::ostringstream out; std::string line, section;
    auto missing=[&] { auto it=values.find(section); if(it!=values.end()) { for(auto &[key,value]:it->second) out<<key<<'='<<value<<'\n'; values.erase(it); } };
    while(std::getline(in,line)) {
        auto text=lower(line);
        if(!text.empty() && text.front()=='[' && text.back()==']') { missing(); section=text.substr(1,text.size()-2); }
        const auto eq=text.find('=');
        auto group=values.find(section);
        if(eq!=std::string::npos && group!=values.end()) {
            auto value=group->second.find(lower(text.substr(0,eq)));
            if(value!=group->second.end()) { out<<line.substr(0,line.find('='))<<'='<<value->second<<'\n'; group->second.erase(value); continue; }
        }
        out<<line<<'\n';
    }
    missing();
    for(auto &[group,entries]:values) { out<<'['<<group<<"]\n"; for(auto &[key,value]:entries) out<<key<<'='<<value<<'\n'; }
    in.close(); auto temporary=path; temporary+=".pending";
    std::ofstream file(temporary,std::ios::binary|std::ios::trunc); file<<out.str(); file.close(); if(!file) return false;
#if defined(_WIN32)
    return MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=FALSE;
#else
    std::error_code ec; std::filesystem::rename(temporary,path,ec); return !ec;
#endif
}
struct VideoPreset { unsigned width,height,msaa; };
constexpr std::array video_presets{VideoPreset{480,272,1},VideoPreset{1280,720,2},
    VideoPreset{1920,1080,2},VideoPreset{1920,1080,4}};
int preset() {
    return settings.rendering.internal_width==1920?(settings.rendering.msaa>=4?3:2):
        settings.rendering.internal_width==1280?1:0;
}
// Player names: letters, digits, space, - and _, at most 16 characters.
void type_name(const std::vector<int> &input) {
    for(int key:input) {
        if(key==0x1b) { typing=false; message=L"Name unchanged"; return; }
        if(key==0x0d) {
            while(!typed.empty() && typed.back()==' ') typed.pop_back();
            if(typed.empty()) { message=L"The name cannot be empty"; continue; }
            const auto previous=net_name;
            net_name=typed; net_dirty=true; typing=false;
            if(commit_settings()) message=L"Saved. Applies after restart";
            else { net_name=previous; message=L"Unable to save settings"; }
            return;
        }
        if(key==8) { if(!typed.empty()) typed.pop_back(); continue; }
        char c=0;
        if(key>='A' && key<='Z') c=static_cast<char>(uppercase_state()?key:key+32);
        else if(key>='0' && key<='9') c=static_cast<char>(key);
        else if(key>=0x60 && key<=0x69) c=static_cast<char>('0'+key-0x60);
        else if(key==0x20 && !typed.empty()) c=' ';
        else if(key==0xbd || key==0x6d) c=uppercase_state() && key==0xbd?'_':'-';
        if(c && typed.size()<16) typed+=c;
    }
}
void edit_option(std::uint32_t buttons, const std::vector<int> &input) {
    if(typing) { type_name(input); return; }
    if(capturing) {
        for(int key:input) {
            if(key==0x1b) { capturing=false; message=L"Binding cancelled"; break; }
            if(valid_binding(key)) { draft.bind(row,binding_slot,key); capturing=false; commit_controls(); break; }
            message=L"F1 and F2 are reserved";
        }
        return;
    }
    if(buttons&0x8000) { page=(page+1)%page_count; row=0; message.clear(); }
    const int count=page==0?static_cast<int>(actions.size()):page==1?3:page==2?5:page==4?2:1;
    if(buttons&0x10) row=(row+count-1)%count;
    if(buttons&0x40) row=(row+1)%count;
    const int direction=(buttons&0x80)?-1:(buttons&0x20)?1:0;
    if(page==0) {
        if(direction) binding_slot=1-binding_slot;
        if(buttons&0x1000) { draft.keys[row][binding_slot]=0; commit_controls(); }
        if(buttons&0x4000) { capturing=true; message=L"Press a key or mouse button. Esc cancels"; }
    } else if(page==1 && (direction || (buttons&0x4000))) {
        if(row==0) draft.sensitivity=std::clamp(draft.sensitivity+(direction<0?-0.01f:0.01f),0.01f,2.f);
        if(row==1) draft.invert_y=!draft.invert_y;
        if(row==2 && (buttons&0x4000)) draft=Controls{};
        commit_controls();
    } else if(page==2 && (direction || (buttons&0x4000))) {
        if(row==0) {
            const auto &value=video_presets[(preset()+(direction<0?3:1))%video_presets.size()];
            settings.rendering.internal_width=value.width;
            settings.rendering.internal_height=value.height;
            settings.rendering.msaa=value.msaa;
        }
        if(row==1) settings.display.fullscreen=!settings.display.fullscreen;
        if(row==2) settings.display.show_fps=!settings.display.show_fps;
        if(row==3) settings.audio.volume=static_cast<unsigned>(std::clamp(static_cast<int>(settings.audio.volume)+(direction<0?-10:10),0,100));
        if(row==4) settings.timing.frame_rate=settings.timing.frame_rate==30?60:30;
        message=commit_settings()?L"Saved. Applies after restart":L"Unable to save settings";
    } else if(page==3 && (direction || (buttons&0x4000))) {
        settings.gameplay.pc_balance=!settings.gameplay.pc_balance;
        if(commit_settings()) {
            set_pc_balance_enabled(settings.gameplay.pc_balance);
            message=L"Saved. Applies next mission/area";
        } else {
            settings.gameplay.pc_balance=!settings.gameplay.pc_balance;
            message=L"Unable to save settings";
        }
    } else if(page==4 && row==0 && (direction || (buttons&0x4000))) {
        net_online=!net_online; net_dirty=true;
        if(commit_settings()) message=L"Saved. Applies after restart";
        else { net_online=!net_online; message=L"Unable to save settings"; }
    } else if(page==4 && row==1 && (buttons&0x4000)) {
        typing=true; typed=net_name; message=L"Type a name. Enter saves";
    }
}
void option_idle(psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
    const auto object=ctx.gpr[4]; auto &memory=rt.memory();
    if(!memory.contains(object,0x24)) { result(ctx,-1); return; }
    option_screen_seen=true;option_screen_frame=frame_number;
    const auto command=memory.load32(object+0x10);
    if(command==0x33) {
        draft=controls_snapshot(); settings=vcs::load_vcs_configuration(vcs::vcs_configuration().source_path);
        if(!net_loaded) { net_loaded=true; net_online=network_config().online; net_name=network_config().nickname; }
        page=0; row=0; binding_slot=0; capturing=false; typing=false; message.clear(); take_keys();
        receiving=true; editing=true; option_seen=true; last_option_frame=frame_number;
    } else {
        editing=false; option_seen=false;
    }
    result(ctx,command==0x33?1:command==0x34?2:-1);
}
void option_keys(psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
    auto &memory=rt.memory(); const auto object=ctx.gpr[4];
    if(!memory.contains(object,0x24)) { receiving=false; editing=false; option_seen=false; result(ctx,0); return; }
    const auto root=memory.load32(object+0x20);
    if(!memory.contains(root,0x98) || memory.load32(root)!=0x08a7a5f8) {
        receiving=false; editing=false; option_seen=false; result(ctx,0); return;
    }
    option_seen=true; editing=true; last_option_frame=frame_number; receiving=true;
    const auto input=take_keys(); const auto buttons=typing?0u:memory.load32(root+0x40)|raw_buttons(input);
    if(!capturing && !typing && (buttons&0x2000)) {
        const auto side=memory.load32(root+0x70);
        if(memory.contains(side,0x14)) memory.store32(side+0x10,5);
        receiving=false; editing=false; option_seen=false; result(ctx,0); return;
    }
    edit_option(buttons,input); result(ctx,-1);
}

void quad(std::vector<vcs::GeGpuVertex> &v,float x,float y,float w,float h,std::uint32_t color) {
    vcs::GeGpuVertex a{},b{},c{},d{};
    a.x=x;a.y=y;a.rgba=color;b=a;b.x+=w;c=b;c.y+=h;d=a;d.y+=h;
    v.insert(v.end(),{a,b,c,a,c,d});
}
struct TextPixel { int x,y,width; unsigned alpha; };
const std::vector<TextPixel> &raster(const std::wstring &text) {
    static std::map<std::wstring,std::vector<TextPixel>> cache;
    auto found=cache.find(text); if(found!=cache.end()) return found->second;
    if(cache.size()>512) cache.clear();
    auto &pixels=cache[text];
#if defined(_WIN32)
    HDC dc=CreateCompatibleDC(nullptr);
    BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=276;info.bmiHeader.biHeight=-16;
    info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
    void *bits{}; HBITMAP bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&bits,nullptr,0);
    HFONT font=CreateFontW(-12,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH,L"Tahoma");
    if(bitmap && font && bits) {
        auto old_bitmap=SelectObject(dc,bitmap),old_font=SelectObject(dc,font);
        std::fill_n(static_cast<std::uint32_t*>(bits),276*16,0u); SetBkColor(dc,RGB(0,0,0));SetTextColor(dc,RGB(255,255,255));
        TextOutW(dc,0,0,text.c_str(),static_cast<int>(text.size())); GdiFlush();
        const auto data=static_cast<std::uint32_t*>(bits);
        for(int y=0;y<16;++y) for(int x=0;x<276;) {
            unsigned alpha=data[y*276+x]&255; int end=x+1;
            while(end<276 && (data[y*276+end]&255)==alpha) ++end;
            if(alpha) pixels.push_back({x,y,end-x,alpha}); x=end;
        }
        SelectObject(dc,old_font);SelectObject(dc,old_bitmap);
    }
    if(font) DeleteObject(font);if(bitmap) DeleteObject(bitmap);DeleteDC(dc);
#endif
    return pixels;
}
void text(std::vector<vcs::GeGpuVertex> &v,const std::wstring &s,int x,int y,std::uint32_t color=foreground,std::uint32_t bg=background,int width=276) {
    for(const auto &p:raster(s)) {
        if(p.x>=width) continue;
        std::uint32_t mixed=0xff000000;
        for(int shift:{0,8,16}) mixed|=((((color>>shift)&255)*p.alpha+((bg>>shift)&255)*(255-p.alpha))/255)<<shift;
        quad(v,static_cast<float>(x+p.x),static_cast<float>(y+p.y),static_cast<float>(std::min(p.width,width-p.x)),1,mixed);
    }
}
void hint(std::vector<vcs::GeGpuVertex> &v,int key,const std::wstring &label,int x,int y) {
    draw_control_icon(v,key,float(x),float(y));
    text(v,label,x+23,y+4);
}
// Room list shown in place of the original "Searching for a game room" dialog,
// and a one-line status while inside a room.
void draw_online(std::vector<vcs::GeGpuVertex> &v,const OnlineView &view) {
    constexpr std::uint32_t highlight=0xff725638, dim=0xff9c9282;
    if(!view.browser) {
        if(view.banner.empty()) return;
        quad(v,300,2,178,15,background);
        text(v,wide(view.banner),305,3,accent,background,170);
        return;
    }
    quad(v,0,0,480,272,background);
    text(v,L"ONLINE ROOMS",12,7,accent);
    text(v,wide(view.headline),250,7,foreground,background,222);
    quad(v,8,25,464,1,accent);
    text(v,L"ROOM",24,31,dim);text(v,L"MAP",178,31,dim);text(v,L"MODE",298,31,dim);text(v,L"PLAYERS",372,31,dim);
    constexpr int visible_rows=7,row_height=22,top=50;
    const int count=static_cast<int>(view.rows.size());
    const int first=count?(std::clamp(view.cursor,0,count-1)/visible_rows)*visible_rows:0;
    for(int i=first;i<std::min(first+visible_rows,count);++i) {
        const auto &row=view.rows[static_cast<std::size_t>(i)];
        const int y=top+(i-first)*row_height;
        const bool selected=i==view.cursor;
        const std::uint32_t back=selected?highlight:background;
        const std::uint32_t color=!row.joinable?dim:selected?accent:foreground;
        if(selected) { quad(v,8,float(y),464,20,highlight);quad(v,8,float(y),3,20,accent); }
        text(v,wide(row.name),24,y+3,color,back,150);
        text(v,wide(row.map),178,y+3,color,back,116);
        text(v,wide(row.mode),298,y+3,color,back,72);
        text(v,wide(row.players),380,y+3,color,back,30);
        text(v,wide(row.tag),418,y+3,row.joinable?accent:dim,back,52);
    }
    if(!count) {
        text(v,L"No rooms at the moment.",24,top+30);
        text(v,L"Go back and choose Create to host one:",24,top+52,dim);
        text(v,L"it will appear in this list for everyone.",24,top+68,dim);
    } else if(count>visible_rows) {
        text(v,std::to_wstring(first+1)+L"-"+std::to_wstring(std::min(first+visible_rows,count))+L" / "+std::to_wstring(count),400,top+visible_rows*row_height+2,dim);
    }
    // Two text runs: one rasterised line is at most 276 pixels wide.
    const auto line=[&](const std::string &s,int y,std::uint32_t color) {
        const auto split=s.size()>44?s.rfind(' ',44):std::string::npos;
        if(split==std::string::npos) { text(v,wide(s),12,y,color,background,456);return; }
        const auto head=wide(s.substr(0,split));
        int width=0;
        for(const auto &p:raster(head)) width=std::max(width,p.x+p.width);
        text(v,head,12,y,color);
        text(v,wide(s.substr(split)),14+width,y,color,background,454-width);
    };
    if(!view.details.empty()) line(view.details,206,foreground);
    if(!view.message.empty()) line(view.message,222,accent);
    else if(!view.footer.empty()) line(view.footer,222,dim);
    quad(v,0,245,480,27,background);
    quad(v,8,244,464,1,dim);
    const auto current=controls_snapshot();
    if(view.busy) {
        hint(v,control_prompt_key(current,0x2000),L"Cancel",386,248);
    } else {
        draw_control_icon(v,control_prompt_key(current,0x10),8,248);
        hint(v,control_prompt_key(current,0x40),L"Select",28,248);
        hint(v,control_prompt_key(current,0x4000),L"Join",130,248);
        hint(v,control_prompt_key(current,0x1000),L"Refresh",224,248);
        hint(v,control_prompt_key(current,0x2000),L"Back",386,248);
    }
}
}
void install_native_menu(psprecomp::Runtime &runtime) {
    // Exact virtual state callbacks of CUI_Parts_Option_Setting in ULUS10020.
    runtime.register_function(0x0886c6ec,option_idle,"kokaku_option_idle");
    runtime.register_function(0x0886c72c,option_keys,"kokaku_option_keys");
}
bool native_menu_key(int key) {
    if(!receiving.load()) return false;
    std::lock_guard lock(key_mutex); if(keys.size()<32) keys.push_back(key); return true;
}
bool native_menu_active() { return receiving.load(); }
bool native_option_screen() { return option_screen_seen && frame_number-option_screen_frame<3; }
void native_menu_navigation_drawn(bool title) {
    if(!navigation_seen || navigation_frame!=frame_number)navigation_title=title;
    else if(!title)navigation_title=false;
    navigation_seen=true;navigation_frame=frame_number;
}
bool native_menu_navigation_active() {
    return navigation_seen && frame_number-navigation_frame<3;
}
std::string native_savedata_name(int slot) { return slot==0?save_base:save_base.substr(0,16)+"PC"+std::to_string(slot+1); }
void native_savedata_open(const std::filesystem::path &root,const std::string &game,const std::string &save,bool writing) {
    save_root=root;save_game=game;save_base=save=="*"?"":save;save_writing=writing;save_row=0;overwrite=false;
    for(int slot=0;slot<5;++slot) {
        const auto path=root/(game+native_savedata_name(slot))/"DATA.BIN";
        std::error_code ec; save_exists[slot]=std::filesystem::is_regular_file(path,ec); save_dates[slot]=L"Empty";
        if(save_exists[slot]) {
            const auto stamp=std::filesystem::last_write_time(path,ec);
            if(!ec) {
                const auto time=std::chrono::system_clock::to_time_t(std::chrono::file_clock::to_sys(stamp));
                const auto local=std::localtime(&time); wchar_t label[64]{};
                if(local && std::wcsftime(label,64,L"%d/%m/%Y %H:%M",local)) save_dates[slot]=label;
            }
        }
    }
    previous_buttons=~0u;take_keys();save_open=true;receiving=true;
}
int native_savedata_update(std::uint32_t buttons) {
    if(!save_open) return -2;
    const auto pressed=(buttons&~previous_buttons)|raw_buttons(take_keys());previous_buttons=buttons;
    if(pressed&0x2000) { if(overwrite) { overwrite=false;return -2; } save_open=false;receiving=false;return -1; }
    if(!overwrite) {
        if(pressed&0x10) save_row=(save_row+4)%5;
        if(pressed&0x40) save_row=(save_row+1)%5;
    }
    if(pressed&0x4000) {
        if(!save_writing && !save_exists[save_row]) return -2;
        if(save_writing && save_exists[save_row] && !overwrite) { overwrite=true;return -2; }
        save_open=false;receiving=false;return save_row;
    }
    return -2;
}
void native_menu_frame(std::uint64_t frame,std::uint32_t buffer,std::uint32_t stride,std::uint32_t format) {
    frame_number=frame;
    const bool visible=editing && option_seen && frame-last_option_frame<3;
    const bool navigation=navigation_seen && frame-navigation_frame<3;
    OnlineView online{};
    const bool online_visible=online_view(online);
    if(!visible && !save_open) { receiving=false;if(!navigation && !online_visible)return; }
    // The game intentionally skips alternate vblanks. An extra host-only
    // batch there would clear the framebuffer and hide the original menu.
    if(!vcs::ge_gpu_backend_graphics_ready() || !vcs::ge_gpu_backend_has_pending_frame()) return;
    std::vector<vcs::GeGpuVertex> vertices; vertices.reserve(30000);
    const int left=save_open?70:164, top=save_open?65:37, right=save_open?408:438;
    if(!visible && !save_open && (!navigation || online.browser)) {
        // The room list draws its own prompts.
    } else if(!visible && !save_open) {
        const auto current=controls_snapshot();
        const int y=navigation_title?216:245;
        quad(vertices,0,float(y),480,24,background);
        for(int i=0;i<4;++i)draw_control_icon(vertices,control_prompt_key(current,std::array<unsigned,4>{0x10,0x40,0x80,0x20}[i]),float(14+i*20),float(y+2));
        text(vertices,L"Navigate",98,y+6);
        hint(vertices,control_prompt_key(current,0x4000),L"Confirm",210,y+2);
        hint(vertices,control_prompt_key(current,0x2000),L"Back",345,y+2);
    } else if(save_open) {
        quad(vertices,float(left),float(top),float(right-left),197,background);
        text(vertices,save_writing?L"SAVE GAME":L"LOAD GAME",left+8,top+5,accent);
        for(int i=0;i<5;++i) {
            const int y=top+30+i*25;
            if(i==save_row) quad(vertices,static_cast<float>(left+6),static_cast<float>(y),2,17,accent);
            text(vertices,L"Slot "+std::to_wstring(i+1),left+14,y,i==save_row?accent:foreground);
            text(vertices,save_dates[i],left+100,y,i==save_row?accent:foreground);
        }
        const auto current=controls_snapshot();
        if(overwrite)text(vertices,L"Overwrite?",left+8,top+175,accent);
        else {
            draw_control_icon(vertices,0x26,float(left+6),float(top+171));
            hint(vertices,0x28,L"Slot",left+26,top+171);
        }
        hint(vertices,control_prompt_key(current,0x4000),overwrite?L"Yes":L"Confirm",left+112,top+171);
        hint(vertices,control_prompt_key(current,0x2000),overwrite?L"Cancel":L"Back",left+236,top+171);
    } else {
        quad(vertices,float(left),float(top),float(right-left),197,background);
        text(vertices,page==0?L"KEYBOARD / MOUSE":page==1?L"MOUSE":page==2?L"VIDEO / AUDIO PC":page==3?L"GAMEPLAY":L"NETWORK",left+5,top+3,accent);
        if(page==0) {
            const auto current=editing?draft:controls_snapshot();
            const int first=(row/7)*7;
            text(vertices,L"ACTION",left+7,top+22);
            text(vertices,L"PRIMARY",left+158,top+22,foreground,background,55);
            text(vertices,L"ALT",left+230,top+22);
            for(int i=first;i<std::min(first+7,static_cast<int>(actions.size()));++i) {
                const int y=top+38+(i-first)*19;
                if(editing && row==i) {
                    quad(vertices,float(left+2),float(y+3),2,15,accent);
                    quad(vertices,float(left+166+binding_slot*60),float(y),24,19,0xff725638);
                }
                text(vertices,actions[i].label,left+7,y+3,row==i?accent:foreground,background,150);
                for(int slot=0;slot<2;++slot)
                    draw_control_icon(vertices,current.keys[i][slot],float(left+168+slot*60),float(y));
            }
            text(vertices,control_key_name(current.keys[row][binding_slot]),left+7,top+174,accent,background,190);
            text(vertices,std::to_wstring(first+1)+L"-"+std::to_wstring(std::min(first+7,int(actions.size())))+L" / "+std::to_wstring(actions.size()),left+204,top+174);
        } else {
            std::vector<std::wstring> rows;
            if(page==1) { wchar_t value[40]{};std::swprintf(value,40,L"Mouse sensitivity: %.2f",draft.sensitivity);
                rows={value,std::wstring(L"Invert Y: ")+(draft.invert_y?L"Yes":L"No"),L"Restore default controls"};
            } else if(page==2) rows={L"Resolution: "+std::to_wstring(settings.rendering.internal_width)+L"x"+std::to_wstring(settings.rendering.internal_height)+L"  MSAA "+std::to_wstring(settings.rendering.msaa),
                std::wstring(L"Fullscreen: ")+(settings.display.fullscreen?L"Yes":L"No"),std::wstring(L"Show FPS: ")+(settings.display.show_fps?L"Yes":L"No"),L"Master volume: "+std::to_wstring(settings.audio.volume)+L"%",
                L"Frame rate: "+std::to_wstring(settings.timing.frame_rate)+L" FPS"};
            else if(page==4) {
                rows={std::wstring(L"Online rooms: ")+(net_online?L"On":L"Off"),
                      L"Player name: "+wide(typing?typed+"_":net_name)};
                text(vertices,net_online?L"Join shows the list of rooms":L"Join enters the first room found",left+8,top+96,accent);
                text(vertices,net_online?L"on the internet and on your network.":L"on your local network (original).",left+8,top+112);
                text(vertices,L"The name is what other players see.",left+8,top+134);
            } else {
                rows={std::wstring(L"PC balance: ")+(settings.gameplay.pc_balance?L"On":L"Off")};
                text(vertices,settings.gameplay.pc_balance?L"Player HP: 40% of original":L"Player HP: 100% of original",left+8,top+65,accent);
                text(vertices,settings.gameplay.pc_balance?L"Enemy damage: +60%":L"Enemy damage: original",left+8,top+83);
                text(vertices,settings.gameplay.pc_balance?L"Soldier attack pauses: -30%":L"Soldier attack pauses: original",left+8,top+101);
                text(vertices,settings.gameplay.pc_balance?L"AI Tachikoma damage: -95%":L"AI Tachikoma damage: original",left+8,top+119);
                text(vertices,settings.gameplay.pc_balance?L"Soldiers: more lateral movement":L"Soldiers: original movement",left+8,top+137);
                text(vertices,L"Next area. Always off in multiplayer",left+8,top+155);
            }
            for(int i=0;i<static_cast<int>(rows.size());++i) { if(i==row) quad(vertices,static_cast<float>(left+2),static_cast<float>(top+30+i*28),2,15,accent);text(vertices,rows[i],left+8,top+30+i*28,i==row?accent:foreground); }
        }
        if(!message.empty()) {
            quad(vertices,float(left+3),float(top+174),267,20,background);
            text(vertices,message,left+7,top+174,accent,background,263);
        }
        quad(vertices,0,245,480,27,background);
        if(typing) {
            text(vertices,L"Letters, digits, space and -",10,251,foreground,background,200);
            hint(vertices,0x0d,L"Save",300,247);
            hint(vertices,0x1b,L"Cancel",386,247);
        } else if(capturing) {
            text(vertices,L"Press a key, mouse button or scroll wheel",10,251,foreground,background,350);
            hint(vertices,0x1b,L"Cancel",386,247);
        } else {
            // Recovery shortcuts match raw_buttons(), even when all menu
            // bindings were removed. Page follows its configured action.
            hint(vertices,0x26,L"",5,247);hint(vertices,0x28,L"Row",24,247);
            hint(vertices,0x25,L"",74,247);hint(vertices,0x27,page==0?L"Slot":L"Value",93,247);
            hint(vertices,0x0d,L"Edit",160,247);hint(vertices,0x1b,L"Back",225,247);
            hint(vertices,9,L"Page",310,247);
            if(page==0)hint(vertices,0x2e,L"Clear",397,247);
        }
        quad(vertices,350,0,130,25,background);
        text(vertices,L"PC",435,6,accent);
    }
    if(online_visible && !visible && !save_open) draw_online(vertices,online);
    if(vertices.empty()) return;
    // The game's UI is drawn to the back buffer before sceDisplay swaps it.
    // Append to those existing passes, never create a pass on the front buffer.
    for(const auto &target:vcs::ge_gpu_backend_pending_framebuffers()) {
        vcs::GeGpuDrawDescriptor draw{};draw.framebuffer_address=target.framebuffer_address;
        draw.framebuffer_stride=target.framebuffer_stride;draw.framebuffer_format=target.framebuffer_format;
        draw.primitive=3;draw.vertex_count=static_cast<std::uint32_t>(vertices.size());draw.through=true;
        vcs::ge_gpu_backend_accumulate_color_triangles(draw,vertices);
    }
}
bool run_native_menu_tests(std::string &error) {
    const auto directory=std::filesystem::temp_directory_path()/("kokaku-menu-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto require=[](bool value,const char *reason) { if(!value) throw std::runtime_error(reason); };
#if defined(_WIN32)
    const char *old=std::getenv("PSPRECOMP_CONTROLS"); const std::string old_path=old?old:"";
    struct Restore { std::string value; ~Restore() { _putenv_s("PSPRECOMP_CONTROLS",value.c_str()); receiving=false; } } restore{old_path};
#endif
    try {
        std::filesystem::create_directory(directory);
        const auto controls=directory/"controls.ini";
#if defined(_WIN32)
        _wputenv_s(L"PSPRECOMP_CONTROLS",controls.c_str());
#endif
        require(!native_menu_active(),"Native menu should not be active initially");
        require(!editing && !option_seen,"Menu editing should be inactive initially");
        draft=Controls{};draft.bind(17,0,'T');draft.bind(19,0,5);
        require(controls_commit(draft,error),"Could not prepare remapped menu controls");
        require((raw_buttons({'T'})&0x4000)!=0 && (raw_buttons({5})&0x8000)!=0,
                "Native menu ignored remapped keyboard/mouse navigation");
        require((raw_buttons({'Z'})&0x4000)==0,"Old confirm key stayed active after remapping");
        require((raw_buttons({0x0d})&0x4000)!=0 && (raw_buttons({0x1b})&0x2000)!=0,
                "Native menu recovery keys stopped working");
        draft=Controls{};page=0;row=0;binding_slot=0;capturing=false;
        edit_option(0x4000,{});require(capturing,"Confirm did not start capture");
        edit_option(0,{'T'});require(!capturing,"Capture did not finish");
        Controls loaded;std::string detail;require(loaded.load(controls,detail) && loaded.keys[0][0]=='T',"Captured key did not persist");
        edit_option(0x4000,{});edit_option(0,{0x1b});require(!capturing && draft.keys[0][0]=='T',"Escape changed the binding");
        binding_slot=1;edit_option(0x4000,{});edit_option(0,{wheel_down});
        require(loaded.load(controls,detail) && loaded.keys[0][1]==wheel_down,"Mouse wheel binding did not persist");
        edit_option(0x1000,{});require(draft.keys[0][1]==0,"Clear did not remove alternate binding");
        settings={};settings.source_path=directory/"settings.ini";
        require(settings.gameplay.pc_balance,"PC balance should default to enabled");
        { std::ofstream out(settings.source_path);out<<"; keep comment\n[Display]\nFullscreen=false\nUnknown=retained\n[Rendering]\nMSAA=4\n[Audio]\nVolume=100\n"; }
        settings.rendering.internal_width=1280;settings.rendering.internal_height=720;settings.rendering.msaa=2;settings.audio.volume=70;
        require(commit_settings(),"Settings write failed");
        const auto config=vcs::load_vcs_configuration(settings.source_path);
        require(config.rendering.internal_width==1280 && config.rendering.msaa==2 && config.audio.volume==70,"Settings did not reload");
        page=2;row=4;settings.timing.frame_rate=60;
        edit_option(0x80,{});
        require(vcs::load_vcs_configuration(settings.source_path).timing.frame_rate==30,"30 FPS selection did not persist");
        edit_option(0x20,{});
        require(vcs::load_vcs_configuration(settings.source_path).timing.frame_rate==60,"60 FPS selection did not persist");
        edit_option(0x8000,{});
        require(page==3 && row==0,"Gameplay page is not reachable from video settings");
        edit_option(0x4000,{});
        require(!vcs::load_vcs_configuration(settings.source_path).gameplay.pc_balance && !pc_balance_enabled(),
                "PC balance Off did not persist or reach the game");
        edit_option(0x20,{});
        require(vcs::load_vcs_configuration(settings.source_path).gameplay.pc_balance && pc_balance_enabled(),
                "PC balance On did not persist or reach the game");
        const auto settings_path=settings.source_path;
        settings.source_path=directory/"missing"/"settings.ini";
        edit_option(0x4000,{});
        require(settings.gameplay.pc_balance && pc_balance_enabled(),"Failed save changed the balance setting");
        settings.source_path=settings_path;
        // Network page: the room list switch and the player name.
        edit_option(0x8000,{});
        require(page==4 && row==0,"Network page is not reachable from gameplay");
        net_loaded=true;net_dirty=false;net_online=true;net_name="Agent-0001";typing=false;
        struct Shift { ~Shift() { uppercase_state=default_uppercase; } } shift;
        uppercase_state=[] { return false; };
        const auto ini_text=[&] { std::ifstream in(settings_path);return std::string((std::istreambuf_iterator<char>(in)),{}); };
        require(ini_text().find("nickname")==std::string::npos && ini_text().find("online")==std::string::npos,
                "Network values were written before the page was used");
        edit_option(0x20,{});
        require(!net_online && ini_text().find("online=false")!=std::string::npos && ini_text().find("nickname=Agent-0001")!=std::string::npos,
                "Online rooms Off did not persist");
        edit_option(0x4000,{});
        require(net_online && ini_text().find("online=true")!=std::string::npos,"Online rooms On did not persist");
        edit_option(0x40,{});edit_option(0x4000,{});
        require(row==1 && typing && typed=="Agent-0001","Confirm did not start name entry");
        for(int i=0;i<12;++i) edit_option(0,{8});
        require(typed.empty(),"Backspace did not erase the name");
        edit_option(0,{0x0d});
        require(typing && net_name=="Agent-0001","An empty name was accepted");
        edit_option(0x8000|0x40|0x2000,{' ','M','O','T','O',0xbd,'K','O',0x70,0x61,' ','9',' '});
        require(typing && typed=="moto-ko1 9 " && page==4 && row==1,"Typed characters or navigation while typing");
        uppercase_state=[] { return true; };
        edit_option(0,{'X',0xbd,'Y','Z','A','B','C','D','E','F'});
        require(typed=="moto-ko1 9 X_YZA","Name was not limited to 16 characters");
        edit_option(0,{0x1b});
        require(!typing && net_name=="Agent-0001" && ini_text().find("nickname=Agent-0001")!=std::string::npos,"Escape changed the name");
        edit_option(0x4000,{});
        edit_option(0,{8,8,8,8,8,8,8,8,8,8,'M','O','T','O','K','O',' ',0x0d});
        require(!typing && net_name=="MOTOKO" && ini_text().find("nickname=MOTOKO")!=std::string::npos,"Name did not persist");
        { NetworkConfiguration saved_net=network_config();
          char program[]="test";char *arguments[]={program};
          init_network_config(settings_path,1,arguments);
          const bool reloaded=network_config().nickname=="MOTOKO" && network_config().online;
          network_config()=saved_net;
          require(reloaded,"Saved network settings did not reload"); }
        net_loaded=false;net_dirty=false;
        edit_option(0x8000,{});
        require(page==0 && row==0,"Network page did not wrap back to controls");
        { std::ifstream in(settings.source_path);std::string content((std::istreambuf_iterator<char>(in)),{});
          require(content.find("Unknown=retained")!=std::string::npos && content.find("; keep comment")!=std::string::npos,"Unrelated settings were lost"); }
        std::filesystem::create_directory(directory/"TEST");{std::ofstream out(directory/"TEST"/"DATA.BIN");out<<"existing";}
        native_savedata_open(directory,"TEST","",true);native_savedata_update(0);
        require(native_savedata_update(0x4000)==-2 && overwrite,"Occupied slot did not require confirmation");
        native_savedata_update(0);require(native_savedata_update(0x2000)==-2 && !overwrite,"Back did not cancel overwrite");
        native_savedata_update(0);native_savedata_update(0x40);native_savedata_update(0);
        require(native_savedata_update(0x4000)==1 && native_savedata_name(1)=="PC2","Empty second slot was not selected");
        native_savedata_open(directory,"TEST","",false);native_savedata_update(0);native_savedata_update(0x40);native_savedata_update(0);
        require(native_savedata_update(0x4000)==-2,"Empty slot was loadable");native_savedata_update(0);
        require(native_savedata_update(0x2000)==-1 && !save_open,"Load cancellation failed");
        require(std::filesystem::file_size(directory/"TEST"/"DATA.BIN")==8,"Selecting/cancelling modified existing data");
        require(!receiving.load() && !editing && !option_seen,"Menu states should be inactive after closing");
        std::filesystem::remove_all(directory);error.clear();return true;
    } catch(const std::exception &exception) { error=std::string(exception.what())+" ("+directory.string()+")";return false; }
}
}
