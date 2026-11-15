#define NOMINMAX
#include "core.hpp"
#include <windows.h>
#include <bcrypt.h>
#include <compressapi.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <vector>

namespace setup {
using Bytes = std::vector<unsigned char>;
[[noreturn]] static void fail(const std::string& s) { throw std::runtime_error(s); }
static void check_cancel(const Options& o) { if (o.cancel && o.cancel->load()) throw Cancelled(); }
static void report(const Options& o, const char* phase, const std::string& detail, uint64_t done=0, uint64_t total=0) {
    check_cancel(o);
    if (o.report) o.report({phase, detail, done, total});
}
std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    int n=MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), int(s.size()), nullptr, 0);
    if (!n) fail("Invalid UTF-8 text.");
    std::wstring r(n, 0); MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), r.data(), n); return r;
}
std::string utf8(const std::wstring& s) {
    if (s.empty()) return {};
    int n=WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0, nullptr, nullptr);
    std::string r(n, 0); WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), r.data(), n, nullptr, nullptr); return r;
}
std::span<const unsigned char> resource(unsigned id) {
    HRSRC r=FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!r) fail("The setup resources are missing. Use a complete copy of the setup EXE.");
    auto p=static_cast<const unsigned char*>(LockResource(LoadResource(nullptr, r)));
    if (!p) fail("Cannot load the setup resources.");
    return {p, SizeofResource(nullptr, r)};
}
static uint16_t u16(const unsigned char* p) { return uint16_t(p[0]) | uint16_t(p[1])<<8; }
static uint32_t u32(const unsigned char* p) { return uint32_t(p[0]) | uint32_t(p[1])<<8 | uint32_t(p[2])<<16 | uint32_t(p[3])<<24; }
static uint64_t u64(const unsigned char* p) { return u32(p) | uint64_t(u32(p+4))<<32; }
static uint32_t dual32(const unsigned char* p) {
    uint32_t le=u32(p), be=uint32_t(p[4])<<24 | uint32_t(p[5])<<16 | uint32_t(p[6])<<8 | p[7];
    if (le!=be) fail("Invalid ISO directory fields.");
    return le;
}
static std::string lower(std::string s) { for (auto& c:s) c=char(std::tolower(static_cast<unsigned char>(c))); return s; }
static void safe_path(const std::string& s) {
    if (s.empty() || s.size()>220 || s.front()=='/' || s.back()=='/') fail("Invalid file path in the input.");
    size_t start=0;
    while (start<s.size()) {
        auto end=s.find('/',start); if (end==std::string::npos) end=s.size();
        auto part=s.substr(start,end-start);
        if (part.empty() || part=="." || part==".." || part.back()=='.' || part.back()==' ') fail("Unsafe file path in the input.");
        for (unsigned char c:part) if (!(std::isalnum(c) || c=='.' || c=='_' || c=='-' || c==' ')) fail("Unsupported file name in the input.");
        auto stem=lower(part.substr(0,part.find('.')));
        if (stem=="con" || stem=="prn" || stem=="aux" || stem=="nul" ||
            (stem.size()==4 && (stem.substr(0,3)=="com" || stem.substr(0,3)=="lpt") && stem[3]>='0' && stem[3]<='9')) fail("Reserved file name in the input.");
        start=end+1;
    }
}
class Hash {
    BCRYPT_ALG_HANDLE alg{}; BCRYPT_HASH_HANDLE hash{};
public:
    Hash() {
        if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0)<0) fail("SHA-256 is unavailable.");
        if (BCryptCreateHash(alg,&hash,nullptr,0,nullptr,0,0)<0) { BCryptCloseAlgorithmProvider(alg,0); fail("Cannot create SHA-256 verifier."); }
    }
    ~Hash() { BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(alg,0); }
    Hash(const Hash&)=delete;
    void add(std::span<const unsigned char> b) {
        if (b.size()>MAXDWORD || BCryptHashData(hash,const_cast<PUCHAR>(b.data()),ULONG(b.size()),0)<0) fail("SHA-256 verification failed.");
    }
    std::string finish() {
        std::array<unsigned char,32> out{};
        if (BCryptFinishHash(hash,out.data(),32,0)<0) fail("SHA-256 verification failed.");
        std::ostringstream ss; ss<<std::hex<<std::setfill('0'); for (auto b:out) ss<<std::setw(2)<<unsigned(b); return ss.str();
    }
};
class Input {
    HANDLE h=INVALID_HANDLE_VALUE;
public:
    uint64_t size{};
    explicit Input(const fs::path& p) {
        // Refuse concurrent write/delete while a validation or extraction is active.
        h=CreateFileW(p.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
        if (h==INVALID_HANDLE_VALUE) fail("Cannot open the ISO. Check its path, permissions and whether another app is modifying it.");
        LARGE_INTEGER n{};
        if (!GetFileSizeEx(h,&n) || n.QuadPart<0) { CloseHandle(h); fail("Cannot read ISO size."); }
        size=uint64_t(n.QuadPart);
    }
    ~Input() { if(h!=INVALID_HANDLE_VALUE) CloseHandle(h); }
    void read(uint64_t pos, std::span<unsigned char> b) {
        if (pos>size || b.size()>size-pos || b.size()>MAXDWORD) fail("The ISO is truncated or contains an invalid file extent.");
        LARGE_INTEGER n{}; n.QuadPart=static_cast<LONGLONG>(pos); DWORD got{};
        if (!SetFilePointerEx(h,n,nullptr,FILE_BEGIN) || !ReadFile(h,b.data(),DWORD(b.size()),&got,nullptr) || got!=b.size()) fail("Cannot read the ISO. The file may be damaged or the drive disconnected.");
    }
    Bytes read(uint64_t pos, size_t n) { Bytes b(n); read(pos,b); return b; }
};
struct Entry { uint64_t offset, size; };
struct Expected { const char* path; uint64_t size; const char* hash; };
#include "build/usa_manifest.hpp"

class Iso {
    Input in;
    std::map<std::string,Entry> files;
    std::set<uint64_t> directories;
    uint64_t volume{};
    void walk(uint64_t offset, uint64_t size, const std::string& parent, unsigned depth) {
        if (depth>12 || size>16*1024*1024 || size<34 || directories.size()>1024 || !directories.insert(offset).second) fail("Invalid or cyclic ISO directory tree.");
        if (offset>volume || size>volume-offset) fail("ISO directory exceeds the disc size.");
        auto data=in.read(offset,size);
        for(size_t pos=0;pos<data.size();) {
            unsigned n=data[pos];
            if (!n) { pos=(pos/2048+1)*2048; continue; }
            if (n<34 || n>data.size()-pos || (pos%2048)+n>2048) fail("Invalid ISO directory record.");
            auto p=data.data()+pos; pos+=n;
            if (33u+p[32]>n || !p[32]) fail("Invalid ISO file name.");
            if (p[32]==1 && p[33]<=1) continue;
            std::string name(reinterpret_cast<const char*>(p+33),p[32]);
            auto version=name.find(';'); if(version!=std::string::npos) name.resize(version);
            auto path=parent+name; safe_path(path);
            if(p[1] || p[26] || p[27] || p[25]&128) fail("Unsupported ISO layout (extended, interleaved or multi-extent file).");
            Entry e{uint64_t(dual32(p+2))*2048,dual32(p+10)};
            if(e.offset>volume || e.size>volume-e.offset) fail("A file extends beyond the ISO. Select a complete disc image.");
            if(p[25]&2) walk(e.offset,e.size,path+"/",depth+1);
            else if(!files.emplace(lower(path),e).second) fail("Duplicate file names in the ISO.");
        }
    }
    std::string disc_id() {
        auto e=find("PSP_GAME/PARAM.SFO");
        if(e.size>65536 || e.size<20) fail("Invalid PSP metadata.");
        auto b=in.read(e.offset,e.size);
        if (std::memcmp(b.data(),"\0PSF",4)) fail("Invalid PARAM.SFO header.");
        auto keys=u32(b.data()+8), vals=u32(b.data()+12), count=u32(b.data()+16);
        if(count>128 || 20ull+uint64_t(count)*16>b.size() || keys>=b.size() || vals>=b.size()) fail("Invalid PARAM.SFO table.");
        for(uint32_t i=0;i<count;i++) {
            auto p=b.data()+20+i*16;
            uint64_t key=uint64_t(keys)+u16(p), value=uint64_t(vals)+u32(p+12), n=u32(p+4);
            if(key>=b.size() || value>=b.size() || n>b.size()-value) fail("Invalid PARAM.SFO offsets.");
            auto end=std::find(b.begin()+key,b.end(),0);
            if(end==b.end()) fail("Invalid PARAM.SFO key.");
            if(std::string(b.begin()+key,end)=="DISC_ID") {
                if(u16(p+2)!=0x0204 || n<2 || n>32 || b[value+n-1]!=0) fail("Invalid disc ID.");
                return std::string(reinterpret_cast<char*>(b.data()+value),n-1);
            }
        }
        fail("The ISO does not contain a PSP disc ID.");
    }
public:
    explicit Iso(const fs::path& path):in(path) {
        if(lower(utf8(path.extension().wstring()))!=".iso") fail("Select an uncompressed .iso file.");
        if(in.size<18*2048) fail("The file is too small to be a PSP ISO.");
        bool found=false;
        for(unsigned sector=16;sector<80;sector++) {
            auto p=in.read(uint64_t(sector)*2048,2048);
            if(std::memcmp(p.data()+1,"CD001",5) || p[6]!=1) fail("This is not a supported ISO 9660 disc image.");
            if(p[0]==255) break;
            if(p[0]!=1) continue;
            if(u16(p.data()+128)!=2048 || (unsigned(p[130])<<8|p[131])!=2048) fail("Unsupported ISO sector size.");
            volume=uint64_t(dual32(p.data()+80))*2048;
            if(volume>in.size || volume<18*2048) fail("The ISO is incomplete or truncated.");
            if(p[156]<34 || !(p[181]&2)) fail("Invalid ISO root directory.");
            walk(uint64_t(dual32(p.data()+158))*2048,dual32(p.data()+166),"",0);
            found=true; break;
        }
        if(!found) fail("The ISO has no primary volume descriptor.");
        auto id=disc_id();
        if(id!="ULUS10020" && id!="ULUS-10020") fail("Unsupported game or region: "+id+". This release requires USA / ULUS10020.");
        for(auto& expected:kExpected) {
            if(find(expected.path).size!=expected.size) fail(std::string("Unsupported or damaged USA revision: ")+expected.path);
        }
    }
    Entry find(const std::string& p) {
        auto i=files.find(lower(p));
        if(i==files.end()) fail("Required game file is missing: "+p);
        return i->second;
    }
    uint64_t total() const { uint64_t n=0; for(auto& e:kExpected) n+=e.size; return n; }
    void stream(const Expected& expected, const fs::path* dest, const Options& o, uint64_t& done, uint64_t total, const char* phase) {
        auto e=find(expected.path); Hash hash; Bytes b(1024*1024); std::ofstream out;
        if(dest) {
            fs::create_directories(dest->parent_path());
            out.open(*dest,std::ios::binary|std::ios::trunc);
            if(!out) fail("Cannot create an output file. Check the destination permissions.");
        }
        for(uint64_t pos=0;pos<e.size;) {
            check_cancel(o);
            size_t n=size_t(std::min<uint64_t>(b.size(),e.size-pos));
            std::span<unsigned char> part(b.data(),n); in.read(e.offset+pos,part); hash.add(part);
            if(dest && !out.write(reinterpret_cast<char*>(b.data()),n)) fail("Cannot write game data. Check the free space and destination drive.");
            pos+=n; done+=n;
            report(o,phase,expected.path,done,total);
            if(o.cancel_after_bytes && done>=o.cancel_after_bytes) throw Cancelled();
        }
        if(dest) { out.flush(); if(!out) fail("Cannot finish writing game data."); }
        if(hash.finish()!=expected.hash) fail(std::string("Data verification failed: ")+expected.path+". Use an unmodified USA / ULUS10020 ISO.");
    }
    void check(const Options& o, bool full) {
        uint64_t done=0,total=0;
        for(auto& e:kExpected) if(full || e.size<8*1024*1024) total+=e.size;
        for(auto& e:kExpected) if(full || e.size<8*1024*1024) stream(e,nullptr,o,done,total,"VALIDATING");
    }
    void extract(const fs::path& root,const Options& o) {
        uint64_t done=0;
        for(auto& e:kExpected) { fs::path dest=root/wide(e.path); stream(e,&dest,o,done,total(),"EXTRACTING"); }
    }
};
uint64_t verify(const fs::path& iso, const Options& options, bool full) {
    report(options,"VALIDATING","Reading disc metadata...");
    Iso input(iso); input.check(options,full); return input.total();
}

struct Packed { std::string path,hash; uint64_t size; std::span<const unsigned char> data; };
static std::vector<Packed> payload() {
    auto b=resource(102); size_t pos=12;
    if(b.size()<12 || std::memcmp(b.data(),"KOKAKU01",8)) fail("Invalid setup payload.");
    unsigned count=u32(b.data()+8); if(count>100 || !count) fail("Invalid setup payload table.");
    std::vector<Packed> files; std::set<std::string> names;
    for(unsigned i=0;i<count;i++) {
        if(pos>b.size() || b.size()-pos<52) fail("Truncated setup payload.");
        auto p=b.data()+pos; unsigned len=u32(p); uint64_t size=u64(p+4),packed=u64(p+12);
        pos+=52;
        if(len>220 || size>128*1024*1024 || len>b.size()-pos || packed>b.size()-pos-len) fail("Invalid setup payload extent.");
        std::string name(reinterpret_cast<const char*>(b.data()+pos),len); safe_path(name);
        if(!names.insert(lower(name)).second) fail("Duplicate setup payload entry.");
        std::ostringstream hash; hash<<std::hex<<std::setfill('0'); for(unsigned j=0;j<32;j++) hash<<std::setw(2)<<unsigned(p[20+j]);
        pos+=len; files.push_back({name,hash.str(),size,b.subspan(pos,packed)}); pos+=packed;
    }
    if(pos!=b.size() || !names.contains("kokaku recomp.exe")) fail("The setup payload is incomplete.");
    return files;
}
static void unpack(const std::vector<Packed>& files,const fs::path& root,const Options& o) {
    uint64_t done=0,total=0; for(auto& f:files) total+=f.size;
    for(auto& f:files) {
        report(o,"PREPARING",f.path,done,total);
        Bytes raw(f.size); DECOMPRESSOR_HANDLE handle{}; SIZE_T actual{};
        if(!CreateDecompressor(COMPRESS_ALGORITHM_XPRESS_HUFF,nullptr,&handle)) fail("Cannot start the Windows decompressor.");
        BOOL ok=Decompress(handle,const_cast<unsigned char*>(f.data.data()),f.data.size(),raw.data(),raw.size(),&actual);
        CloseDecompressor(handle);
        if(!ok || actual!=raw.size()) fail("The setup payload is damaged. Obtain a fresh copy.");
        Hash hash; hash.add(raw); if(hash.finish()!=f.hash) fail("Setup payload checksum mismatch.");
        check_cancel(o);
        auto dest=root/wide(f.path); fs::create_directories(dest.parent_path());
        std::ofstream out(dest,std::ios::binary); out.write(reinterpret_cast<char*>(raw.data()),raw.size()); out.flush();
        if(!out) fail("Cannot write the native runtime. Check the destination drive.");
        done+=f.size; report(o,"PREPARING",f.path,done,total);
    }
}
fs::path prepare(const fs::path& iso,const fs::path& parent,const Options& o) {
    report(o,"VALIDATING","Checking USA / ULUS10020...");
    Iso source(iso); source.check(o,false);
    std::error_code ec;
    if(!fs::is_directory(parent,ec)) fail("Choose an existing destination folder.");
    auto base=fs::canonical(parent), final=base/L"KOKAKU Recomp";
    if(fs::exists(final)) fail("KOKAKU Recomp already exists here. Select another parent folder; existing files will not be overwritten.");
    if(utf8(final.wstring()).size()>180) fail("The destination path is too long for the current game runtime. Choose a shorter path.");
    auto files=payload(); uint64_t needed=source.total()+64*1024*1024;
    for(auto& f:files) needed+=f.size;
    ULARGE_INTEGER free{};
    if(!GetDiskFreeSpaceExW(base.c_str(),&free,nullptr,nullptr)) fail("Cannot check destination free space.");
    if(free.QuadPart<needed) fail("Not enough free space. The destination needs at least "+std::to_string((needed+1048575)/1048576)+" MiB.");
    GUID guid{}; if(FAILED(CoCreateGuid(&guid))) fail("Cannot create a temporary folder identifier.");
    wchar_t tag[40]{}; StringFromGUID2(guid,tag,40);
    fs::path staging=base/(std::wstring(L".KOKAKU-preparing-")+tag);
    if(!CreateDirectoryW(staging.c_str(),nullptr)) fail("Cannot create files in the destination. Choose a writable folder.");
    // Only this newly created, uniquely named direct child is owned by this operation.
    try {
        source.extract(staging/L"PSP_DATA",o);
        unpack(files,staging,o);
        report(o,"FINALIZING","Writing installation record...",1,1);
        std::ofstream receipt(staging/L"PREPARATION.txt",std::ios::binary);
        receipt<<"KOKAKU Recomp / native port by DDD\r\nUSA ULUS10020 / setup 0.1.0\r\n"
               <<"Source files verified with SHA-256.\r\nSupport: https://ko-fi.com/X8X4FMCO5\r\n\r\n";
        for(auto& e:kExpected) receipt<<e.hash<<"  PSP_DATA/"<<e.path<<"\r\n";
        receipt.close(); if(!receipt) fail("Cannot write the preparation record.");
        check_cancel(o);
        // MoveFile refuses an existing destination, including one created during extraction.
        if(!MoveFileExW(staging.c_str(),final.c_str(),MOVEFILE_WRITE_THROUGH)) fail("Cannot finalize the output folder. The destination may already exist.");
    } catch(...) {
        // Do not follow or remove anything outside our canonical parent and exact staging path.
        auto attrs=GetFileAttributesW(staging.c_str());
        if(staging.parent_path()==base && attrs!=INVALID_FILE_ATTRIBUTES && !(attrs&FILE_ATTRIBUTE_REPARSE_POINT)) {
            fs::remove_all(staging,ec);
        }
        if(ec) fail("Preparation stopped. Temporary files could not be removed: "+utf8(staging.wstring()));
        throw;
    }
    // A cancellation after the atomic rename must not report a failed installation.
    if(o.report) o.report({"COMPLETE","KOKAKU Recomp is ready.",1,1});
    return final;
}
}
