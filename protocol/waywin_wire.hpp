// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <stdexcept>
#include <algorithm>
namespace WayWinWire {
constexpr uint32_t Magic=0x32575759, Maximum=2*1024*1024;
enum Type : uint32_t { Hello=1, Apps=2, Launch=3, Shell=4, ClipboardGet=5, ClipboardSet=6,
    Pointer=7, Key=8, SurfaceClose=9, Text=10,
    Ready=101, AppList=102, Result=103, Frame=104, Clipboard=105, SurfaceGone=106,
    Channel=107, Title=108, Icon=109, Retry=110, Fault=111, TrayTitle=112 };
struct Message { uint32_t type=0,request=0,epoch=0;std::vector<uint8_t> data; };
inline void put(std::vector<uint8_t>&b,uint32_t n){for(int i=0;i<4;++i)b.push_back(uint8_t(n>>(8*i)));}
inline uint32_t get(const uint8_t*p){return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
inline void string(std::vector<uint8_t>&b,const std::string&s){put(b,uint32_t(s.size()));b.insert(b.end(),s.begin(),s.end());}
class Reader {
    const std::vector<uint8_t>&b;size_t p=0;
public:
    explicit Reader(const std::vector<uint8_t>&bytes):b(bytes){}
    uint32_t number(){if(b.size()-p<4)throw std::runtime_error("truncated field");auto n=get(b.data()+p);p+=4;return n;}
    std::string text(size_t max=65536){auto n=number();if(n>max||n>b.size()-p)throw std::runtime_error("invalid text");std::string s(b.begin()+p,b.begin()+p+n);p+=n;return s;}
    bool done()const{return p==b.size();}
    size_t position()const{return p;}
};
inline bool requestType(uint32_t t){return t>=Hello&&t<=Text;}
inline bool responseType(uint32_t t){return t>=Ready&&t<=TrayTitle;}
inline std::vector<uint8_t> encode(const Message&m){
    if(m.data.size()>Maximum)throw std::runtime_error("oversized frame");
    std::vector<uint8_t>b;b.reserve(24+m.data.size());put(b,Magic);put(b,2);put(b,m.type);put(b,m.request);put(b,m.epoch);put(b,uint32_t(m.data.size()));b.insert(b.end(),m.data.begin(),m.data.end());return b;
}
class Decoder {
    std::vector<uint8_t> buffer;
public:
    void reset(){buffer.clear();}
    std::vector<Message> feed(const uint8_t*p,size_t n){
        if(n>Maximum+24||buffer.size()+n>2*(Maximum+24)){reset();throw std::runtime_error("buffer limit");}
        buffer.insert(buffer.end(),p,p+n);std::vector<Message>out;size_t off=0;
        while(buffer.size()-off>=24){const auto*h=buffer.data()+off;auto size=get(h+20);
            if(get(h)!=Magic||get(h+4)!=2||size>Maximum){reset();throw std::runtime_error("protocol mismatch");}
            if(buffer.size()-off<24+size)break;
            out.push_back({get(h+8),get(h+12),get(h+16),{h+24,h+24+size}});off+=24+size;
        }
        buffer.erase(buffer.begin(),buffer.begin()+off);return out;
    }
};
inline std::string hex(const std::vector<uint8_t>&b){const char*d="0123456789abcdef";std::string s;s.reserve(b.size()*2);for(auto c:b){s+=d[c>>4];s+=d[c&15];}return s;}
inline std::vector<uint8_t> unhex(const std::string&s){
    if(s.size()%2||s.size()>2*(Maximum+24))throw std::runtime_error("invalid hex length");
    std::vector<uint8_t>b;b.reserve(s.size()/2);int nibble=-1;
    for(char c:s){int n=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:-1;if(n<0)throw std::runtime_error("invalid hex");if(nibble<0)nibble=n;else{b.push_back(uint8_t(nibble*16+n));nibble=-1;}}return b;
}
}
