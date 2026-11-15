#pragma once
#include "kokaku_controls.hpp"
#include "ge_gpu_backend.hpp"
#include <algorithm>
#include <span>

namespace kokaku {
// English names are independent of the Windows display language. IDs in the
// user's INI remain unchanged.
inline std::wstring control_key_name(int key) {
    if ((key>='A' && key<='Z') || (key>='0' && key<='9')) return std::wstring(1,wchar_t(key));
    if (key>=0x70 && key<=0x87) return L"F"+std::to_wstring(key-0x6f);
    if (key>=0x60 && key<=0x69) return L"Num "+std::to_wstring(key-0x60);
    switch(key) {
    case 0:return L"Unbound";case 1:return L"Left mouse";case 2:return L"Right mouse";
    case 4:return L"Middle mouse";case 5:return L"Mouse 4";case 6:return L"Mouse 5";
    case wheel_up:return L"Wheel up";case wheel_down:return L"Wheel down";
    case 8:return L"Backspace";case 9:return L"Tab";case 0x0d:return L"Enter";
    case 0x10:return L"Shift";case 0x11:return L"Ctrl";case 0x12:return L"Alt";
    case 0x13:return L"Pause";case 0x14:return L"Caps Lock";case 0x1b:return L"Esc";
    case 0x20:return L"Space";case 0x21:return L"Page Up";case 0x22:return L"Page Down";
    case 0x23:return L"End";case 0x24:return L"Home";
    case 0x25:return L"Left";case 0x26:return L"Up";case 0x27:return L"Right";case 0x28:return L"Down";
    case 0x2c:return L"Print Screen";case 0x2d:return L"Insert";case 0x2e:return L"Delete";
    case 0x5b:return L"Left Win";case 0x5c:return L"Right Win";case 0x5d:return L"Menu";
    case 0x6a:return L"Num *";case 0x6b:return L"Num +";case 0x6d:return L"Num -";
    case 0x6e:return L"Num .";case 0x6f:return L"Num /";
    case 0x90:return L"Num Lock";case 0x91:return L"Scroll Lock";
    case 0xa0:return L"Left Shift";case 0xa1:return L"Right Shift";
    case 0xa2:return L"Left Ctrl";case 0xa3:return L"Right Ctrl";
    case 0xa4:return L"Left Alt";case 0xa5:return L"Right Alt";
    case 0xba:return L";";case 0xbb:return L"=";case 0xbc:return L",";case 0xbd:return L"-";
    case 0xbe:return L".";case 0xbf:return L"/";case 0xc0:return L"`";
    case 0xdb:return L"[";case 0xdc:return L"\\";case 0xdd:return L"]";case 0xde:return L"'";
    default:return L"Key "+std::to_wstring(key);
    }
}
inline int control_prompt_key(const Controls &controls,std::uint32_t button,ControlContext context=ControlContext::Menu) {
    if(button>=0x10000000 && button<0x10000004) {
        const auto &keys=controls.keys[button-0x10000000];
        return keys[0]?keys[0]:keys[1];
    }
    for(std::size_t i=0;i<actions.size();++i)
        if(actions[i].button==button && (actions[i].context==context || actions[i].context==ControlContext::Always))
            return controls.keys[i][0]?controls.keys[i][0]:controls.keys[i][1];
    return 0;
}

// 20x20 pixel sprites, shared by the original game's prompts and KeyAssign.
// A compact 3x5 alphabet keeps Escape/Space readable in the original icon box.
using ControlIcon = std::array<std::uint32_t,400>;
inline constexpr std::array<std::array<unsigned,7>,36> key_letters{{
    {14,17,17,31,17,17,17},{30,17,17,30,17,17,30},{14,17,16,16,16,17,14},
    {30,17,17,17,17,17,30},{31,16,16,30,16,16,31},{31,16,16,30,16,16,16},
    {14,17,16,23,17,17,14},{17,17,17,31,17,17,17},{14,4,4,4,4,4,14},
    {7,2,2,2,18,18,12},{17,18,20,24,20,18,17},{16,16,16,16,16,16,31},
    {17,27,21,21,17,17,17},{17,25,21,19,17,17,17},{14,17,17,17,17,17,14},
    {30,17,17,30,16,16,16},{14,17,17,17,21,18,13},{30,17,17,30,20,18,17},
    {15,16,16,14,1,1,30},{31,4,4,4,4,4,4},{17,17,17,17,17,17,14},
    {17,17,17,17,17,10,4},{17,17,17,21,21,21,10},{17,17,10,4,10,17,17},
    {17,17,10,4,4,4,4},{31,1,2,4,8,16,31},
    {14,17,19,21,25,17,14},{4,12,4,4,4,4,14},{14,17,1,2,4,8,31},
    {30,1,1,14,1,1,30},{2,6,10,18,31,2,2},{31,16,16,30,1,1,30},
    {14,16,16,30,17,17,14},{31,1,2,4,8,8,8},{14,17,17,14,17,17,14},{14,17,17,15,1,1,14}
}};
inline std::array<unsigned,5> icon_letter(wchar_t c) {
    constexpr std::array<std::array<unsigned,5>,36> letters{{
        {2,5,7,5,5},{6,5,6,5,6},{3,4,4,4,3},{6,5,5,5,6},{7,4,6,4,7},{7,4,6,4,4},
        {3,4,5,5,3},{5,5,7,5,5},{7,2,2,2,7},{1,1,1,5,2},{5,5,6,5,5},{4,4,4,4,7},
        {5,7,7,5,5},{5,7,7,7,5},{2,5,5,5,2},{6,5,6,4,4},{2,5,5,3,1},{6,5,6,5,5},
        {3,4,2,1,6},{7,2,2,2,2},{5,5,5,5,7},{5,5,5,5,2},{5,5,7,7,5},{5,5,2,5,5},
        {5,5,2,2,2},{7,1,2,4,7},
        {7,5,5,5,7},{2,6,2,2,7},{6,1,2,4,7},{6,1,2,1,6},{5,5,7,1,1},
        {7,4,6,1,6},{3,4,6,5,2},{7,1,2,2,2},{2,5,2,5,2},{2,5,3,1,6}
    }};
    if(c>='a'&&c<='z')c-=32;
    if(c>='A'&&c<='Z')return letters[c-'A'];
    if(c>='0'&&c<='9')return letters[26+c-'0'];
    switch(c) {
    case '-':return {0,0,7,0,0};case '=':return {0,7,0,7,0};case '+':return {0,2,7,2,0};
    case '.':return {0,0,0,0,2};case ',':return {0,0,0,2,4};case ';':return {0,2,0,2,4};
    case '/':return {1,1,2,4,4};case '\\':return {4,4,2,1,1};case '[':return {6,4,4,4,6};
    case ']':return {3,1,1,1,3};case '\'':return {2,2,0,0,0};case '`':return {4,2,0,0,0};
    default:return {7,1,2,0,2};
    }
}
inline ControlIcon control_icon(int key) {
    ControlIcon pixels{};
    constexpr std::uint32_t edge=0xffffdba2, face=0xff49331b, light=0xfffff2dc, shadow=0xff211508;
    const auto rect=[&](int x,int y,int w,int h,std::uint32_t color) {
        for(int j=y;j<y+h;++j)for(int i=x;i<x+w;++i)if(i>=0&&i<20&&j>=0&&j<20)pixels[j*20+i]=color;
    };
    const bool mouse=key==1||key==2||key==4||key==5||key==6||key==wheel_up||key==wheel_down;
    if(mouse) {
        rect(6,1,8,1,edge);rect(4,2,12,15,edge);rect(6,17,8,2,shadow);
        rect(5,3,10,13,face);rect(6,16,8,1,edge);rect(9,2,2,8,edge);rect(5,9,10,1,edge);
        if(key==1)rect(5,3,4,6,light);
        if(key==2)rect(11,3,4,6,light);
        if(key==4)rect(9,4,2,4,light);
        if(key==5||key==6){rect(2,key==5?9:13,2,3,light);rect(6,key==5?11:14,2,1,light);}
        if(key==wheel_up||key==wheel_down) {
            const bool up=key==wheel_up;
            rect(9,3,2,5,light);rect(8,up?4:6,4,1,light);rect(7,up?5:5,6,1,light);
        }
        return pixels;
    }
    rect(2,2,16,1,edge);rect(1,3,18,13,edge);rect(2,16,16,2,shadow);
    rect(2,4,16,11,face);rect(3,15,14,1,shadow);rect(3,4,14,1,0xff725638);
    if(key>=0x25&&key<=0x28) {
        // Rotate one seven-pixel arrow; no font or locale dependency.
        for(int y=0;y<7;++y)for(int x=0;x<7;++x) {
            const bool on=(y<4&&x>=3-y&&x<=3+y)||(y>=3&&x>=2&&x<=4);
            if(!on)continue;
            int px=x,py=y;
            if(key==0x28){px=6-x;py=6-y;}
            if(key==0x25){px=y;py=6-x;}
            if(key==0x27){px=6-y;py=x;}
            rect(6+px,6+py,1,1,light);
        }
        return pixels;
    }
    auto label=control_key_name(key);
    switch(key) {
    case 0:label=L"-";break;case 8:label=L"BSP";break;case 0x0d:label=L"ENT";break;
    case 0x10:label=L"SHF";break;case 0x11:label=L"CTL";break;case 0x20:label=L"SPC";break;
    case 0x14:label=L"CAP";break;case 0x21:label=L"PGU";break;case 0x22:label=L"PGD";break;
    case 0x24:label=L"HOM";break;case 0x2d:label=L"INS";break;case 0x2e:label=L"DEL";break;
    case 0xa0:label=L"LSH";break;case 0xa1:label=L"RSH";break;
    case 0xa2:label=L"LCT";break;case 0xa3:label=L"RCT";break;
    case 0xa4:label=L"LAL";break;case 0xa5:label=L"RAL";break;
    case 0x5b:label=L"LWN";break;case 0x5c:label=L"RWN";break;
    case 0x13:label=L"PAU";break;case 0x2c:label=L"PRT";break;
    case 0x90:label=L"NML";break;case 0x91:label=L"SCL";break;
    }
    if(key>=0x60&&key<=0x69)label=L"N"+std::to_wstring(key-0x60);
    if(key==0x6a)label=L"N*";if(key==0x6b)label=L"N+";if(key==0x6d)label=L"N-";
    if(key==0x6e)label=L"N.";if(key==0x6f)label=L"N/";
    if(label.starts_with(L"Key ")){constexpr wchar_t hex[]=L"0123456789ABCDEF";label={L'K',hex[(key>>4)&15],hex[key&15]};}
    if(label.size()>3)label=label.substr(0,3);
    if(label.size()==1 && ((key>='A'&&key<='Z')||(key>='0'&&key<='9'))) {
        const auto glyph=key_letters[key>='A'?key-'A':26+key-'0'];
        for(int y=0;y<7;++y)for(int x=0;x<5;++x)if(glyph[y]&(1u<<(4-x)))rect(7+x,6+y,1,1,light);
        return pixels;
    }
    const int start=(20-(int(label.size())*4-1))/2;
    for(std::size_t n=0;n<label.size();++n){const auto glyph=icon_letter(label[n]);
        for(int y=0;y<5;++y)for(int x=0;x<3;++x)if(glyph[y]&(1u<<(2-x)))rect(start+int(n)*4+x,7+y,1,1,light);
    }
    return pixels;
}
inline void draw_control_icon(std::vector<vcs::GeGpuVertex>& vertices,int key,float x,float y,float scale=1.f) {
    const auto pixels=control_icon(key);
    for(int row=0;row<20;++row)for(int col=0;col<20;) {
        const auto color=pixels[row*20+col];int end=col+1;
        while(end<20&&pixels[row*20+end]==color)++end;
        if(color){vcs::GeGpuVertex a{},b{},c{},d{};
            a.x=x+col*scale;a.y=y+row*scale;a.rgba=color;b=a;b.x=x+end*scale;
            c=b;c.y+=scale;d=a;d.y+=scale;vertices.insert(vertices.end(),{a,b,c,a,c,d});}
        col=end;
    }
}
// RGB fingerprints of the visible 20x20 cells, not addresses: the title,
// pause, briefing and multiplayer modules relocate their UI textures.
inline std::uint32_t control_prompt_button(std::uint32_t hash) {
    switch(hash) {
    case 0x2f2bfdaf:return 0x1000;case 0x5daf5c6d:return 0x2000;
    case 0x5677c7e5:return 0x4000;case 0xdeb8de99:return 0x8000;
    case 0x5eec5459:return 0x100;case 0x1eb26e29:return 0x200;
    case 0x186d7b9b:return 0x10;case 0xd5d13b3b:return 0x40;
    case 0xc779bf35:return 0x80;case 0xd6bec475:return 0x20;
    case 0xe18f4023:return 1;
    case 0xf2048f85:return 0x10000000;case 0xde457825:return 0x10000001;
    case 0x572230bc:return 0x10000002;case 0xd5768eb9:return 0x10000003;
    default:return 0;
    }
}
inline void replace_control_texture(std::span<std::byte> bytes,int key) {
    if(bytes.size()<32*32*4)return;
    const auto icon=control_icon(key);
    std::fill(bytes.begin(),bytes.end(),std::byte{});
    for(int y=0;y<20;++y)for(int x=0;x<20;++x)for(int c=0;c<4;++c)
        bytes[(y*32+x)*4+c]=std::byte((icon[y*20+x]>>(c*8))&255);
}
}
