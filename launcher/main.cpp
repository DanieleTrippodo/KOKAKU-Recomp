#define NOMINMAX
#include "core.hpp"
#include <windows.h>
#include <windowsx.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

namespace {
// Palette of the game's own menus: deep blue ground, brighter blue plates with
// a cut corner, orange for whatever is active.
constexpr COLORREF ground=RGB(9,16,62), grid=RGB(16,28,92), ring=RGB(30,52,150), bar=RGB(6,10,44),
    plateColor=RGB(22,40,132), plateBright=RGB(40,70,196), field=RGB(8,14,54), lineColor=RGB(104,144,240),
    white=RGB(236,242,255), muted=RGB(150,172,228), orange=RGB(236,150,44), ink=RGB(20,14,6), errorColor=RGB(255,176,150);
constexpr int width=840,height=644;
enum { IsoEdit=1001, FolderEdit, BrowseIso, BrowseFolder, Prepare, Cancel, OpenFolder, Support, Minimize, Close };
HWND window{},isoEdit{},folderEdit{},prepareBtn{},cancelBtn{},openBtn{},isoBtn{},folderBtn{};
HFONT smallFont{},bodyFont{},heroFont{};
HBRUSH editBrush{};
double scale=1;
bool italian=false;
std::atomic<bool> cancelRequested{false},running{false},finished{false};
std::thread worker;
std::mutex progressMutex;
setup::Progress progress{"IDLE","",0,0};
std::string failure;
setup::fs::path output;
bool validated=false,complete=false,verifyOnly=false,closing=false,wasCancelled=false,updating=false;
// Written by the worker, consumed only after its release/acquire completion signal.
bool resultSuccess=false,resultCancelled=false;
std::string resultError;
setup::fs::path resultOutput;
std::chrono::steady_clock::time_point started;
int px(int v) { return int(v*scale+0.5); }
RECT rect(int x,int y,int w,int h) { return {px(x),px(y),px(x+w),px(y+h)}; }
// The interface follows the language of Windows: Italian, or English for everyone else.
const wchar_t* tr(const wchar_t* english,const wchar_t* italianText) { return italian?italianText:english; }
// What the preparation engine reports, for an Italian reader. Unlisted texts stay as they are.
std::wstring localized(const std::string& s) {
    static const std::pair<const char*,const wchar_t*> table[]={
        {"IDLE",L"IN ATTESA"},{"VALIDATING",L"VERIFICA"},{"EXTRACTING",L"ESTRAZIONE"},{"PREPARING",L"PREPARAZIONE"},
        {"FINALIZING",L"COMPLETAMENTO"},{"READY",L"PRONTO"},{"COMPLETE",L"COMPLETATO"},
        {"Reading disc metadata...",L"Lettura dei dati del disco..."},
        {"KOKAKU Recomp is ready.",L"KOKAKU Recomp è pronto."},
        {"USA / ULUS10020 matched. Select PREPARE GAME to continue.",L"ISO USA / ULUS10020 riconosciuta. Scegli PREPARA IL GIOCO per continuare."},
        {"Select an uncompressed .iso file.",L"Scegli un file .iso non compresso."},
        {"Cannot open the ISO. Check its path, permissions and whether another app is modifying it.",
         L"Impossibile aprire la ISO. Controlla percorso, permessi e che un altro programma non la stia modificando."},
        {"This is not a supported ISO 9660 disc image.",L"Non è un'immagine disco ISO 9660 supportata."},
        {"The ISO is incomplete or truncated.",L"La ISO è incompleta o troncata."},
        {"Choose an existing destination folder.",L"Scegli una cartella di destinazione esistente."},
        {"KOKAKU Recomp already exists here. Select another parent folder; existing files will not be overwritten.",
         L"Qui esiste già una cartella KOKAKU Recomp. Scegli un'altra cartella: i file esistenti non vengono sovrascritti."},
        {"The destination path is too long for the current game runtime. Choose a shorter path.",
         L"Il percorso di destinazione è troppo lungo per il gioco. Scegline uno più corto."},
        {"Cannot create files in the destination. Choose a writable folder.",L"Impossibile creare file nella destinazione. Scegli una cartella scrivibile."},
        {"A file operation failed. Check folder permissions, free space and the selected paths.",
         L"Un'operazione sui file non è riuscita. Controlla permessi, spazio libero e percorsi scelti."}};
    if(italian) {
        for(const auto& [english,text]:table) if(s==english) return text;
        const std::string region="Unsupported game or region: ",space="Not enough free space.";
        if(s.rfind(region,0)==0) return L"Gioco o regione non supportati: "+setup::wide(s.substr(region.size(),s.find('.')-region.size()))+L". Serve la versione USA / ULUS10020.";
        if(s.rfind(space,0)==0) return L"Spazio libero insufficiente nella destinazione.";
    }
    return setup::wide(s);
}
std::wstring value(HWND h) { int n=GetWindowTextLengthW(h); std::wstring s(n+1,0); GetWindowTextW(h,s.data(),n+1); s.resize(n); return s; }
void fill(HDC dc,RECT r,COLORREF color) { HBRUSH b=CreateSolidBrush(color); FillRect(dc,&r,b); DeleteObject(b); }
void stroke(HDC dc,int x,int y,int x2,int y2,COLORREF color=lineColor) {
    HPEN p=CreatePen(PS_SOLID,1,color); auto old=SelectObject(dc,p); MoveToEx(dc,px(x),px(y),nullptr); LineTo(dc,px(x2),px(y2)); SelectObject(dc,old); DeleteObject(p);
}
void text(HDC dc,const std::wstring& s,int x,int y,int w,int h,HFONT font,COLORREF color=white,UINT flags=DT_LEFT|DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS) {
    RECT r=rect(x,y,w,h); auto old=SelectObject(dc,font); SetTextColor(dc,color); SetBkMode(dc,TRANSPARENT); DrawTextW(dc,s.c_str(),int(s.size()),&r,flags|DT_NOPREFIX); SelectObject(dc,old);
}
// A plate with its top-right corner cut, in device pixels, as the game draws its menu entries.
void plate_px(HDC dc,RECT r,int cut,COLORREF inside,COLORREF border) {
    POINT points[]={{r.left,r.top},{r.right-1-cut,r.top},{r.right-1,r.top+cut},{r.right-1,r.bottom-1},{r.left,r.bottom-1}};
    HBRUSH b=CreateSolidBrush(inside); HPEN p=CreatePen(PS_SOLID,1,border);
    auto oldBrush=SelectObject(dc,b); auto oldPen=SelectObject(dc,p);
    Polygon(dc,points,5);
    SelectObject(dc,oldBrush); SelectObject(dc,oldPen); DeleteObject(b); DeleteObject(p);
}
void plate(HDC dc,int x,int y,int w,int h,COLORREF inside=plateColor,COLORREF border=lineColor,int cut=10) {
    plate_px(dc,rect(x,y,w,h),px(cut),inside,border);
}
void circle(HDC dc,int cx,int cy,int radius,COLORREF color) {
    HPEN p=CreatePen(PS_SOLID,1,color); auto oldPen=SelectObject(dc,p); auto oldBrush=SelectObject(dc,GetStockObject(NULL_BRUSH));
    Ellipse(dc,px(cx-radius),px(cy-radius),px(cx+radius),px(cy+radius));
    SelectObject(dc,oldBrush); SelectObject(dc,oldPen); DeleteObject(p);
}
setup::Progress snapshot() { std::lock_guard lock(progressMutex); return progress; }
void paint(HDC dc) {
    fill(dc,rect(0,0,width,height),ground);
    // Faint grid and target rings behind the title, like the Multi Play screens.
    for(int x=40;x<width;x+=40) stroke(dc,x,37,x,176,grid);
    for(int y=56;y<176;y+=40) stroke(dc,0,y,width,y,grid);
    for(int radius:{34,58,82}) circle(dc,474,106,radius,ring);
    stroke(dc,474,24,474,188,ring); stroke(dc,380,106,568,106,ring);
    fill(dc,rect(0,0,width,36),bar);
    text(dc,tr(L"K / /   KOKAKU RECOMP  /  PREPARATION UTILITY",L"K / /   KOKAKU RECOMP  /  PREPARAZIONE DEL GIOCO"),20,7,680,22,smallFont,muted);
    stroke(dc,0,36,width,36);
    text(dc,L"KOKAKU",28,60,420,61,heroFont);
    text(dc,L"R E C O M P",30,123,400,30,bodyFont,orange);
    plate(dc,560,66,252,90,plateColor,lineColor,14);
    text(dc,L"GHOST IN THE SHELL",572,72,228,22,smallFont,white);
    text(dc,L"STAND ALONE COMPLEX",572,94,228,22,smallFont,muted);
    stroke(dc,572,120,800,120);
    text(dc,L"USA / ULUS10020",572,126,150,22,smallFont,muted);
    text(dc,L"v0.2",722,126,78,22,smallFont,orange,DT_RIGHT|DT_SINGLELINE|DT_VCENTER);
    auto p=snapshot();
    bool extracting=running && !verifyOnly;
    // The three steps as menu entries: the current one is orange.
    const int step=complete?2:extracting?1:0;
    const wchar_t* steps[]={tr(L"01  SELECT ISO",L"01  SCEGLI LA ISO"),tr(L"02  PREPARE FILES",L"02  PREPARA I FILE"),tr(L"03  READY TO PLAY",L"03  PRONTO PER GIOCARE")};
    for(int i=0;i<3;i++) {
        const bool current=i==step, done=i<step;
        plate(dc,28+i*264,184,256,30,current?orange:plateColor,current?orange:lineColor,9);
        text(dc,steps[i],42+i*264,185,230,28,smallFont,current?ink:done?white:muted);
    }
    text(dc,tr(L"SOURCE / GAME DISC IMAGE",L"ORIGINE / IMMAGINE DEL DISCO"),28,228,570,19,smallFont,muted);
    text(dc,validated?tr(L"[ USA VERIFIED ]",L"[ USA VERIFICATA ]"):L"[ .ISO ]",600,228,212,19,smallFont,validated?orange:muted,DT_RIGHT|DT_SINGLELINE|DT_VCENTER);
    plate(dc,28,254,614,38,field,validated?orange:lineColor,9);
    text(dc,tr(L"DESTINATION / PARENT FOLDER",L"DESTINAZIONE / CARTELLA"),28,310,650,19,smallFont,muted);
    plate(dc,28,336,614,38,field,lineColor,9);
    text(dc,tr(L"Creates a new KOKAKU Recomp folder here.",L"Qui viene creata una nuova cartella KOKAKU Recomp."),28,381,782,22,smallFont,muted);
    plate(dc,28,417,784,116,plateColor,!failure.empty()?errorColor:lineColor,14);
    std::wstring status=complete?tr(L"COMPLETE",L"COMPLETATO"):!failure.empty()?tr(L"CHECK REQUIRED",L"DA CONTROLLARE"):wasCancelled?tr(L"CANCELLED",L"ANNULLATO"):localized(p.phase);
    text(dc,L">  "+status,44,426,570,22,smallFont,!failure.empty()?errorColor:white);
    int percent=p.total?int(std::min<uint64_t>(100,p.completed*100/p.total)):0;
    std::wstring counter=running?std::to_wstring(percent)+L"%":complete?L"100%":L"--";
    text(dc,counter,693,426,102,22,smallFont,orange,DT_RIGHT|DT_SINGLELINE|DT_VCENTER);
    // One bar for the bytes processed in the current phase.
    fill(dc,rect(44,456,752,10),field);
    const int filled=complete?752:752*percent/100;
    if(filled>0) fill(dc,rect(44,456,filled,10),orange);
    for(int i=1;i<16;i++) stroke(dc,44+i*47,456,44+i*47,466,plateColor);
    std::wstring detail=!failure.empty()?localized(failure):p.detail.empty()?std::wstring(tr(L"Select your USA game ISO to begin.",L"Scegli la tua ISO USA del gioco per cominciare.")):localized(p.detail);
    if(wasCancelled) detail=tr(L"Cancelled. Temporary files removed. You can try again.",L"Annullato. I file temporanei sono stati rimossi. Puoi riprovare.");
    text(dc,detail,44,475,752,43,smallFont,!failure.empty()?errorColor:muted,DT_LEFT|DT_WORDBREAK);
    text(dc,complete?tr(L"Your portable game is ready.",L"Il gioco portatile è pronto."):tr(L"Uses your own game disc image.",L"Usa la tua immagine del disco."),28,545,380,20,smallFont,white);
    text(dc,complete?tr(L"Open the folder and run KOKAKU Recomp.exe.",L"Apri la cartella e avvia KOKAKU Recomp.exe."):tr(L"The original ISO is kept unchanged.",L"La ISO originale non viene modificata."),28,565,380,20,smallFont,muted);
    if(complete) text(dc,tr(L"Online rooms: every player needs v0.2.",L"Stanze online: a tutti serve la v0.2."),28,585,380,20,smallFont,orange);
    fill(dc,rect(0,609,width,35),bar);
    stroke(dc,0,609,width,609);
    text(dc,tr(L"NATIVE PORT BY DDD",L"PORT NATIVO DI DDD"),28,616,220,24,smallFont,white);
    text(dc,L"WINDOWS x64",272,616,125,24,smallFont,muted);
    HBRUSH edge=CreateSolidBrush(lineColor); RECT all=rect(0,0,width,height); FrameRect(dc,&all,edge); DeleteObject(edge);
}
void update_buttons() {
    bool busy=running;
    EnableWindow(isoEdit,!busy); EnableWindow(folderEdit,!busy);
    EnableWindow(isoBtn,!busy); EnableWindow(folderBtn,!busy);
    ShowWindow(prepareBtn,complete?SW_HIDE:SW_SHOW);
    ShowWindow(openBtn,complete?SW_SHOW:SW_HIDE);
    EnableWindow(prepareBtn,!busy && !value(isoEdit).empty() && !value(folderEdit).empty());
    ShowWindow(cancelBtn,busy?SW_SHOW:SW_HIDE);
    SetWindowTextW(prepareBtn,busy?tr(L"WORKING...",L"IN CORSO..."):tr(L"PREPARE GAME  >",L"PREPARA IL GIOCO  >"));
    InvalidateRect(window,nullptr,FALSE);
}
HWND button(const wchar_t* title,int id) {
    return CreateWindowExW(0,L"BUTTON",title,WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,0,0,0,0,window,reinterpret_cast<HMENU>(INT_PTR(id)),nullptr,nullptr);
}
void fonts() {
    for(auto f:{smallFont,bodyFont,heroFont}) if(f) DeleteObject(f);
    // Readable text in the system interface font; only the big title keeps the pixel font.
    auto make=[](int size,int weight,const wchar_t* face,DWORD quality) { return CreateFontW(-px(size),0,0,0,weight,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,quality,DEFAULT_PITCH,face); };
    smallFont=make(14,FW_SEMIBOLD,L"Segoe UI",CLEARTYPE_QUALITY); bodyFont=make(22,FW_SEMIBOLD,L"Segoe UI",CLEARTYPE_QUALITY);
    heroFont=make(55,FW_NORMAL,L"Departure Mono",NONANTIALIASED_QUALITY);
    SendMessageW(isoEdit,WM_SETFONT,reinterpret_cast<WPARAM>(smallFont),TRUE);
    SendMessageW(folderEdit,WM_SETFONT,reinterpret_cast<WPARAM>(smallFont),TRUE);
}
void layout() {
    auto place=[](int id,int x,int y,int w,int h) { MoveWindow(GetDlgItem(window,id),px(x),px(y),px(w),px(h),TRUE); };
    place(IsoEdit,40,263,589,21); place(FolderEdit,40,345,589,21);
    place(BrowseIso,654,254,158,39); place(BrowseFolder,654,336,158,39);
    place(Prepare,518,549,294,45); place(OpenFolder,518,549,294,45);
    place(Cancel,416,549,90,45); place(Support,442,614,370,27);
    place(Minimize,756,2,40,32); place(Close,797,2,40,32);
    fonts(); InvalidateRect(window,nullptr,FALSE);
}
setup::fs::path pick(bool folder) {
    IFileOpenDialog* dialog{};
    if(FAILED(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dialog)))) return {};
    DWORD options{}; dialog->GetOptions(&options);
    dialog->SetOptions(options|FOS_FORCEFILESYSTEM|FOS_PATHMUSTEXIST|(folder?FOS_PICKFOLDERS:FOS_FILEMUSTEXIST));
    dialog->SetTitle(folder?tr(L"Select a parent folder for KOKAKU Recomp",L"Scegli la cartella in cui creare KOKAKU Recomp"):tr(L"Select your USA / ULUS10020 game ISO",L"Scegli la tua ISO USA / ULUS10020 del gioco"));
    dialog->SetOkButtonLabel(folder?tr(L"Use this folder",L"Usa questa cartella"):tr(L"Select ISO",L"Scegli ISO"));
    COMDLG_FILTERSPEC spec{tr(L"PSP disc image (*.iso)",L"Immagine disco PSP (*.iso)"),L"*.iso"};
    if(!folder) { dialog->SetFileTypes(1,&spec); dialog->SetDefaultExtension(L"iso"); }
    setup::fs::path path;
    if(SUCCEEDED(dialog->Show(window))) {
        IShellItem* item{};
        if(SUCCEEDED(dialog->GetResult(&item))) { PWSTR name{}; if(SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH,&name))) { path=name; CoTaskMemFree(name); } item->Release(); }
    }
    dialog->Release(); return path;
}
void start(bool onlyVerify) {
    if(running) return;
    if(worker.joinable()) worker.join();
    auto iso=setup::fs::path(value(isoEdit)),parent=setup::fs::path(value(folderEdit));
    cancelRequested=false; finished=false; running=true; verifyOnly=onlyVerify;
    complete=false; validated=false; failure.clear(); wasCancelled=false; output.clear();
    resultSuccess=false; resultCancelled=false; resultError.clear(); resultOutput.clear();
    started=std::chrono::steady_clock::now();
    { std::lock_guard lock(progressMutex); progress={"VALIDATING","Reading disc metadata...",0,0}; }
    update_buttons();
    worker=std::thread([iso,parent,onlyVerify] {
        try {
            setup::Options options{&cancelRequested,[](const setup::Progress& p) { std::lock_guard lock(progressMutex); progress=p; },0};
            if(onlyVerify) {
                setup::verify(iso,options,false);
                resultSuccess=true;
                std::lock_guard lock(progressMutex);
                progress={"READY","USA / ULUS10020 matched. Select PREPARE GAME to continue.",0,0};
            } else { resultOutput=setup::prepare(iso,parent,options); resultSuccess=true; }
        } catch(const setup::Cancelled&) { resultCancelled=true; }
          catch(const std::filesystem::filesystem_error&) { resultError="A file operation failed. Check folder permissions, free space and the selected paths."; }
          catch(const std::exception& e) { resultError=e.what(); }
        finished.store(true,std::memory_order_release);
    });
}
void owner_draw(DRAWITEMSTRUCT* d) {
    int id=int(d->CtlID); bool disabled=d->itemState&ODS_DISABLED,pressed=d->itemState&ODS_SELECTED;
    bool primary=id==Prepare||id==OpenFolder,chrome=id==Close||id==Minimize,link=id==Support;
    COLORREF fg=disabled?muted:primary?ink:link?orange:white;
    if(chrome||link) {
        fill(d->hDC,d->rcItem,pressed?plateColor:bar);
    } else {
        // The corner that the plate leaves open shows the window behind it.
        fill(d->hDC,d->rcItem,ground);
        COLORREF inside=disabled?field:primary?(pressed?white:orange):pressed?plateBright:plateColor;
        plate_px(d->hDC,d->rcItem,px(9),inside,disabled?plateColor:primary?orange:lineColor);
    }
    auto title=value(d->hwndItem); auto old=SelectObject(d->hDC,smallFont);
    SetBkMode(d->hDC,TRANSPARENT); SetTextColor(d->hDC,fg);
    RECT r=d->rcItem; DrawTextW(d->hDC,title.c_str(),int(title.size()),&r,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
    SelectObject(d->hDC,old);
    if(d->itemState&ODS_FOCUS) { InflateRect(&r,-5,-5); DrawFocusRect(d->hDC,&r); }
}
LRESULT CALLBACK proc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    switch(msg) {
    case WM_CREATE: {
        window=h;
        isoEdit=CreateWindowExW(0,L"EDIT",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|ES_AUTOHSCROLL,0,0,0,0,h,reinterpret_cast<HMENU>(IsoEdit),nullptr,nullptr);
        folderEdit=CreateWindowExW(0,L"EDIT",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|ES_AUTOHSCROLL,0,0,0,0,h,reinterpret_cast<HMENU>(FolderEdit),nullptr,nullptr);
        SendMessageW(isoEdit,EM_SETCUEBANNER,TRUE,reinterpret_cast<LPARAM>(tr(L"Select a Ghost in the Shell USA .iso",L"Scegli una .iso USA di Ghost in the Shell")));
        SendMessageW(folderEdit,EM_SETCUEBANNER,TRUE,reinterpret_cast<LPARAM>(tr(L"Choose a writable destination folder",L"Scegli una cartella di destinazione scrivibile")));
        SendMessageW(isoEdit,EM_SETLIMITTEXT,32760,0); SendMessageW(folderEdit,EM_SETLIMITTEXT,32760,0);
        isoBtn=button(tr(L"BROWSE ISO  +",L"SFOGLIA ISO  +"),BrowseIso); folderBtn=button(tr(L"CHOOSE FOLDER  +",L"SCEGLI CARTELLA  +"),BrowseFolder);
        prepareBtn=button(tr(L"PREPARE GAME  >",L"PREPARA IL GIOCO  >"),Prepare); cancelBtn=button(tr(L"CANCEL",L"ANNULLA"),Cancel); openBtn=button(tr(L"OPEN GAME FOLDER  >",L"APRI LA CARTELLA  >"),OpenFolder);
        button(tr(L"SUPPORT DDD / ko-fi.com/X8X4FMCO5",L"SOSTIENI DDD / ko-fi.com/X8X4FMCO5"),Support); button(L"_",Minimize); button(L"X",Close);
        layout(); update_buttons(); SetTimer(h,1,100,nullptr); return 0;
    }
    case WM_COMMAND: {
        int id=LOWORD(wp);
        if((id==IsoEdit||id==FolderEdit) && HIWORD(wp)==EN_CHANGE && !updating && !running) {
            if(id==IsoEdit) validated=false;
            complete=false; failure.clear(); wasCancelled=false;
            {std::lock_guard lock(progressMutex);progress={"IDLE","",0,0};}
            update_buttons(); return 0;
        }
        if(HIWORD(wp)!=BN_CLICKED) break;
        if(id==BrowseIso && !running) { auto p=pick(false); if(!p.empty()) { SetWindowTextW(isoEdit,p.c_str()); start(true); } }
        if(id==BrowseFolder && !running) { auto p=pick(true); if(!p.empty()) SetWindowTextW(folderEdit,p.c_str()); }
        if(id==Prepare && !running) start(false);
        if(id==Cancel) { cancelRequested=true; EnableWindow(cancelBtn,FALSE); }
        if(id==OpenFolder && !output.empty()) ShellExecuteW(h,L"open",output.c_str(),nullptr,nullptr,SW_SHOWNORMAL);
        if(id==Support) ShellExecuteW(h,L"open",L"https://ko-fi.com/X8X4FMCO5",nullptr,nullptr,SW_SHOWNORMAL);
        if(id==Minimize) ShowWindow(h,SW_MINIMIZE);
        if(id==Close) SendMessageW(h,WM_CLOSE,0,0);
        return 0;
    }
    case WM_TIMER:
        if(finished.exchange(false,std::memory_order_acquire)) {
            if(worker.joinable()) worker.join();
            running=false;
            failure=resultError; wasCancelled=resultCancelled; output=resultOutput;
            validated=resultSuccess; complete=resultSuccess&&!verifyOnly;
            EnableWindow(cancelBtn,TRUE); update_buttons();
            if(closing) DestroyWindow(h);
        } else if(running) { RECT r=rect(28,417,785,117); InvalidateRect(h,&r,FALSE); }
        return 0;
    case WM_DRAWITEM: owner_draw(reinterpret_cast<DRAWITEMSTRUCT*>(lp)); return TRUE;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC: SetBkColor(reinterpret_cast<HDC>(wp),field); SetTextColor(reinterpret_cast<HDC>(wp),white); return reinterpret_cast<LRESULT>(editBrush);
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{}; HDC dc=BeginPaint(h,&ps); RECT r{};GetClientRect(h,&r);
        HDC mem=CreateCompatibleDC(dc); HBITMAP bmp=CreateCompatibleBitmap(dc,r.right,r.bottom); auto old=SelectObject(mem,bmp);
        paint(mem); BitBlt(dc,0,0,r.right,r.bottom,mem,0,0,SRCCOPY); SelectObject(mem,old); DeleteObject(bmp); DeleteDC(mem); EndPaint(h,&ps); return 0;
    }
    case WM_DPICHANGED: {
        scale=HIWORD(wp)/96.0; auto r=reinterpret_cast<RECT*>(lp);
        SetWindowPos(h,nullptr,r->left,r->top,px(width),px(height),SWP_NOZORDER|SWP_NOACTIVATE); layout(); return 0;
    }
    case WM_NCHITTEST: {
        POINT p{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)}; ScreenToClient(h,&p);
        if(p.y<px(36) && p.x<px(752)) return HTCAPTION;
        return HTCLIENT;
    }
    case WM_CLOSE:
        if(running) { closing=true; cancelRequested=true; EnableWindow(cancelBtn,FALSE); }
        else DestroyWindow(h);
        return 0;
    case WM_DESTROY: KillTimer(h,1); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h,msg,wp,lp);
}
int cli(int argc,wchar_t** argv) {
    try {
        std::atomic<bool> stop=false;
        std::string last;
        setup::Options options{&stop,[&](const setup::Progress& p) { if(p.phase!=last) { std::cout<<p.phase<<": "<<p.detail<<std::endl; last=p.phase; } },0};
        if(std::wstring(argv[1])==L"--verify" && argc==3) { auto n=setup::verify(argv[2],options,true); std::cout<<"VERIFIED "<<n<<" bytes\n"; }
        else if(std::wstring(argv[1])==L"--prepare" && (argc==4||argc==6)) {
            if(argc==6) {
                if(std::wstring(argv[4])!=L"--cancel-after") throw std::runtime_error("Unknown diagnostic option.");
                options.cancel_after_bytes=std::stoull(argv[5]);
            }
            auto path=setup::prepare(argv[2],argv[3],options); std::cout<<"OUTPUT "<<setup::utf8(path.wstring())<<std::endl;
        } else throw std::runtime_error("Usage: --verify ISO | --prepare ISO PARENT [--cancel-after BYTES]");
        return 0;
    } catch(const setup::Cancelled& e) { std::cerr<<e.what()<<std::endl; return 2; }
      catch(const std::exception& e) { std::cerr<<e.what()<<std::endl; return 1; }
}
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,LPWSTR,int show) {
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    int argc{}; wchar_t** argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    if(argc>1 && std::wstring(argv[1])!=L"--ui") { int code=cli(argc,argv); LocalFree(argv); CoUninitialize(); return code; }
    int result=1; HANDLE fontHandle{};
    try {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        scale=GetDpiForSystem()/96.0;
        italian=PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_ITALIAN;
        auto font=setup::resource(101); DWORD count{};
        fontHandle=AddFontMemResourceEx(const_cast<unsigned char*>(font.data()),DWORD(font.size()),nullptr,&count);
        if(!fontHandle || !count) throw std::runtime_error("The embedded Departure Mono font could not be loaded.");
        editBrush=CreateSolidBrush(field);
        WNDCLASSEXW wc{}; wc.cbSize=sizeof(wc); wc.lpfnWndProc=proc;wc.hInstance=instance;wc.hCursor=LoadCursor(nullptr,IDC_ARROW);wc.lpszClassName=L"DDD.KokakuRecomp.Setup";
        wc.hIcon=static_cast<HICON>(LoadImageW(instance,MAKEINTRESOURCEW(1),IMAGE_ICON,GetSystemMetrics(SM_CXICON),GetSystemMetrics(SM_CYICON),LR_DEFAULTCOLOR));
        wc.hIconSm=static_cast<HICON>(LoadImageW(instance,MAKEINTRESOURCEW(1),IMAGE_ICON,GetSystemMetrics(SM_CXSMICON),GetSystemMetrics(SM_CYSMICON),LR_DEFAULTCOLOR));
        RegisterClassExW(&wc);
        RECT work{};SystemParametersInfoW(SPI_GETWORKAREA,0,&work,0);
        int x=work.left+std::max(0L,(work.right-work.left-px(width))/2),y=work.top+std::max(0L,(work.bottom-work.top-px(height))/2);
        HWND h=CreateWindowExW(WS_EX_APPWINDOW|WS_EX_CONTROLPARENT,wc.lpszClassName,L"KOKAKU Recomp Setup | DDD",WS_POPUP|WS_SYSMENU|WS_MINIMIZEBOX|WS_CLIPCHILDREN,x,y,px(width),px(height),nullptr,nullptr,instance,nullptr);
        if(!h) throw std::runtime_error("Cannot create the setup window.");
        DWORD corners=1;DwmSetWindowAttribute(h,33,&corners,sizeof(corners));
        wchar_t exe[32768]{};GetModuleFileNameW(nullptr,exe,32768);
        updating=true;
        SetWindowTextW(folderEdit,setup::fs::path(exe).parent_path().c_str());
        if(argc>=3) SetWindowTextW(isoEdit,argv[2]);
        if(argc>=4) SetWindowTextW(folderEdit,argv[3]);
        updating=false; update_buttons();
        ShowWindow(h,show);UpdateWindow(h);SetFocus(isoBtn);
        MSG msg{};
        while(GetMessageW(&msg,nullptr,0,0)>0) {
            if(msg.message==WM_KEYDOWN && msg.wParam==VK_ESCAPE && running) {cancelRequested=true;continue;}
            if(IsDialogMessageW(h,&msg)) continue;
            TranslateMessage(&msg);DispatchMessageW(&msg);
        }
        if(worker.joinable()) worker.join();
        result=0;
    } catch(const std::exception& e) { MessageBoxW(nullptr,setup::wide(e.what()).c_str(),L"KOKAKU Recomp Setup",MB_OK|MB_ICONERROR); }
    if(fontHandle) RemoveFontMemResourceEx(fontHandle);
    for(auto f:{smallFont,bodyFont,heroFont}) if(f) DeleteObject(f);
    if(editBrush) DeleteObject(editBrush);
    LocalFree(argv);CoUninitialize();return result;
}
