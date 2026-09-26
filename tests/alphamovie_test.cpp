#include "AmvMovie.h"
#include <zlib.h>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <cstring>

using namespace krkr::amv;
namespace {
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void Reject(F f) {
    bool failed=false; try { f(); } catch(const std::exception&) { failed=true; }
    Check(failed,"malformed input was accepted");
}
void U32(std::vector<uint8_t>& bytes, size_t pos, uint32_t value) {
    for(int i=0;i<4;++i) bytes.at(pos+i)=uint8_t(value>>(i*8));
}
class MemoryInput final : public Input {
    std::vector<uint8_t> bytes_;
public:
    explicit MemoryInput(std::vector<uint8_t> bytes) : bytes_(std::move(bytes)) {}
    uint64_t Size() const override { return bytes_.size(); }
    void Read(uint64_t pos,void* dest,size_t bytes) override {
        Check(pos<=bytes_.size() && bytes<=bytes_.size()-pos,"read past input");
        std::memcpy(dest,bytes_.data()+pos,bytes);
    }
};
class FileInput final : public Input {
    std::ifstream file_;uint64_t size_;
public:
    explicit FileInput(const char* name) : file_(name,std::ios::binary) {
        Check(bool(file_),"cannot open AMV fixture");file_.seekg(0,std::ios::end);size_=file_.tellg();
    }
    uint64_t Size() const override { return size_; }
    void Read(uint64_t pos,void* dest,size_t bytes) override {
        Check(pos<=size_ && bytes<=size_-pos,"file read past input");
        file_.seekg(pos);file_.read(static_cast<char*>(dest),bytes);Check(bool(file_),"short fixture read");
    }
};
std::unique_ptr<Input> Memory(const std::vector<uint8_t>& b) { return std::make_unique<MemoryInput>(b); }

// A single neutral 16x16 macroblock: U/V DC0+EOB, then four Y DC0+EOB.
// Canonical JPEG codes give exactly 00 28 a2 8a, independently of the decoder.
std::vector<uint8_t> Fixture(uint32_t frames=2,bool dct=false,bool empty=false) {
    std::vector<uint8_t> b(dct?232:168,1);
    std::fill(b.begin(),b.begin()+40,0);std::memcpy(b.data(),"AJPM",4);
    U32(b,12,uint32_t(b.size()));U32(b,20,frames);U32(b,28,30);b[32]=16;b[34]=16;U32(b,36,dct?1:2);
    for(uint32_t i=0;i<frames;++i) {
        const size_t p=b.size();const bool blank=empty && i==0;
        b.resize(p+(dct||blank?20:24),0);std::memcpy(b.data()+p,"FRAM",4);U32(b,p+8,i);
        if(!blank) {
            b[p+16]=16;b[p+18]=16;
            if(!dct) {
                std::vector<uint8_t> alpha(256,i?64:128),z(compressBound(256));uLongf n=z.size();
                Check(compress(z.data(),&n,alpha.data(),alpha.size())==Z_OK,"fixture compression failed");
                z.resize(n);U32(b,p+20,uint32_t(n));b.insert(b.end(),z.begin(),z.end());
            }
            b.insert(b.end(),{0x00,0x28,0xa2,0x8a});
            if(dct) b.insert(b.end(),{0x28,0xa2,0x8a});
        }
        U32(b,p+4,uint32_t(b.size()-p-8));
    }
    U32(b,4,uint32_t(b.size()));return b;
}
void Tests() {
    const auto fixture=Fixture();
    Movie movie(Memory(fixture));Check(movie.GetInfo().rate==30 && movie.GetInfo().count==2,"header metadata");
    const auto first=movie.Decode(0);Check(first->rgba.size()==1024,"frame byte count");
    for(size_t i=0;i<1024;i+=4) {
        Check(first->rgba[i]==128 && first->rgba[i+1]==128 && first->rgba[i+2]==128 && first->rgba[i+3]==128,
              "neutral frame pixels/alpha");
    }
    Check(movie.Decode(1)->rgba[3]==64,"independent zlib alpha");
    movie.ClearDecoded();Check(movie.GetInfo().count==2 && !movie.CachedBytes(),"clear keeps movie metadata");
    auto dct=Fixture(2,true,true);Movie other(Memory(dct));
    Check(other.Decode(0)->rgba.empty(),"empty frame retained");
    Check(other.Decode(1)->rgba[3]==128,"DCT alpha plane");

    Player player;Check(player.Loop() && player.NextLoop() && player.Preload()==5,"original defaults");
    player.Open(Memory(fixture));player.SetLoop(false);player.Play();
    Check(player.NextImage()->index==0,"first frame zero based");
    Check(player.NextImage()->index==1 && player.IsPlaying(),"last frame stays active until stop");
    Check(player.NextImage()->index==1,"nonloop holds last frame");
    player.Play();player.Seek(1);Check(player.NextImage()->index==1,"seek displays requested frame");
    player.Play();Check(player.NextImage()->index==0,"play restarts at zero");
    player.SetLoop(true);player.Play();
    for(int i=0;i<5;++i) Check(player.NextImage()->index==unsigned(i%2),"loop order");
    player.SetLoop(false);player.Seek(0);player.SetNext(Memory(Fixture(3)));player.SetNextLoop(true);
    player.NextImage();Check(player.GetInfo().count==2,"next movie not switched early");
    Check(player.NextImage()->index==1 && player.GetInfo().count==3 && player.Loop(),"next metadata at old last frame");
    Check(player.NextImage()->index==0,"next movie starts at zero");
    const auto stoppedFrame=player.FrameNumber();
    player.Stop();Check(!player.IsPlaying() && !player.CachedBytes() && player.FrameNumber()==stoppedFrame &&
        player.GetInfo().count==3,"stop keeps metadata/cursor and releases decoded frames");
    player.Close();Check(player.GetInfo().count==0,"close releases movie");
    Reject([&]{player.Play();});Reject([&]{player.Seek(0);});
    player.Open(Memory(fixture));Reject([&]{player.Seek(-1);});Reject([&]{player.Seek(2);});

    for(size_t i=0;i<fixture.size();++i) {
        auto cut=fixture;cut.resize(i);Reject([&]{Movie bad(Memory(cut));});
    }
    for(size_t offset:{size_t(0),size_t(8),size_t(12),size_t(20),size_t(36),size_t(168),size_t(172),size_t(188)}) {
        auto bad=fixture;U32(bad,offset,0xffffffffu);Reject([&]{Movie invalid(Memory(bad));invalid.Decode(0);});
    }
    auto corrupt=fixture;corrupt[fixture.size()-1]=0xff;
    Reject([&]{Movie bad(Memory(corrupt));bad.Decode(1);});

    Frame pixels;pixels.width=2;pixels.height=2;
    pixels.rgba={250,100,20,128, 30,60,90,255, 10,20,30,0, 100,110,120,255};
    std::vector<uint8_t> dest(24,0xcc);
    CopyBGRA(pixels,dest.data()+12,-12,2,2,false);
    Check(dest[12]==20 && dest[14]==250 && dest[0]==30 && dest[3]==0,"BGRA and signed pitch");
    Check(dest[8]==0xcc && dest[20]==0xcc,"bitmap padding untouched");
    CopyBGRA(pixels,dest.data(),12,2,2,true);
    Check(dest[0]==10 && dest[1]==50 && dest[2]==125 && dest[3]==128,"premultiplied alpha conversion");
    Reject([&]{CopyBGRA(pixels,dest.data(),4,2,2,false);});
    std::cout<<"AlphaMovie container, decode, empty frames, seek, loop, queue, cleanup, malformed inputs, BGRA/pitch passed\n";
}
}
int main(int argc,char** argv) {
    try {
        if(argc==5 && std::string(argv[1])=="--probe") {
            Movie movie(std::make_unique<FileInput>(argv[2]));auto frame=movie.Decode(std::stoul(argv[3]));
            std::ofstream output(argv[4],std::ios::binary);
            output.write(reinterpret_cast<const char*>(frame->rgba.data()),frame->rgba.size());
            Check(bool(output),"cannot write probe");
            std::cout<<frame->width<<'x'<<frame->height<<" at "<<frame->left<<','<<frame->top<<'\n';return 0;
        }
        if(argc==3 && std::string(argv[1])=="--scan") {
            Movie movie(std::make_unique<FileInput>(argv[2]));size_t peak=0;
            for(uint32_t i=0;i<movie.GetInfo().count;++i) {
                auto f=movie.Decode(i);Check(f->index==i,"real fixture frame order");
                peak=std::max(peak,movie.CachedBytes());Check(peak<=Movie::CacheLimit,"cache budget exceeded");
            }
            std::cout<<"decoded "<<movie.GetInfo().count<<" frames; cache peak "<<peak<<" bytes\n";return 0;
        }
        Tests();return 0;
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
