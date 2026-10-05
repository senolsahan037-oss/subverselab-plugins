#pragma once
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>

struct Wav { int rate=48000, channels=2; std::vector<float> data; };
static uint32_t rd32(const unsigned char* p){return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
static uint16_t rd16(const unsigned char* p){return uint16_t(p[0])|uint16_t(p[1]<<8);}
[[maybe_unused]] static bool readWav(const std::string& path, Wav& w) {
    std::ifstream f(path,std::ios::binary); if(!f)return false; std::vector<unsigned char>b((std::istreambuf_iterator<char>(f)),{});
    if(b.size()<44||std::memcmp(b.data(),"RIFF",4)||std::memcmp(b.data()+8,"WAVE",4))return false;
    size_t p=12; uint16_t fmt=0,bits=0; uint32_t rate=0; const unsigned char* dat=nullptr; uint32_t bytes=0;
    while(p+8<=b.size()){const char* id=(const char*)b.data()+p;uint32_t n=rd32(b.data()+p+4);size_t q=p+8;if(q+n>b.size())break;
        if(!std::memcmp(id,"fmt ",4)&&n>=16){fmt=rd16(b.data()+q);w.channels=rd16(b.data()+q+2);rate=rd32(b.data()+q+4);bits=rd16(b.data()+q+14);}
        if(!std::memcmp(id,"data",4)){dat=b.data()+q;bytes=n;} p=q+n+(n&1);}
    if(!dat||fmt!=1||(bits!=16&&bits!=24&&bits!=32)||w.channels<1||w.channels>2)return false; w.rate=int(rate); int bps=bits/8, frames=int(bytes/(bps*w.channels));w.data.resize(size_t(frames)*2);
    for(int i=0;i<frames;i++)for(int ch=0;ch<2;ch++){int src=std::min(ch,w.channels-1);const unsigned char*x=dat+(i*w.channels+src)*bps;float v=0;
        if(bits==16)v=int16_t(rd16(x))/32768.0f; else if(bits==24){int32_t z=(x[0]|(x[1]<<8)|(x[2]<<16));if(z&0x800000)z|=~0xffffff;v=z/8388608.0f;} else {uint32_t z=rd32(x);std::memcpy(&v,&z,4);}w.data[size_t(i)*2+ch]=v;}
    return true;
}
static bool writeWav24(const std::string& path,int rate,const std::vector<float>& d){std::ofstream f(path,std::ios::binary);if(!f)return false;uint32_t frames=uint32_t(d.size()/2),bytes=frames*6,size=36+bytes;auto w16=[&](uint16_t x){f.put(char(x));f.put(char(x>>8));};auto w32=[&](uint32_t x){for(int i=0;i<4;i++)f.put(char(x>>(8*i)));};f.write("RIFF",4);w32(size);f.write("WAVEfmt ",8);w32(16);w16(1);w16(2);w32(rate);w32(rate*6);w16(6);w16(24);f.write("data",4);w32(bytes);for(float x:d){x=std::max(-1.0f,std::min(0.999999f,x));int32_t z=int32_t(x*8388607.0f);f.put(char(z));f.put(char(z>>8));f.put(char(z>>16));}return bool(f);}
