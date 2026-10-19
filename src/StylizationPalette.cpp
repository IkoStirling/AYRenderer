#include "AYRenderer/StylizationPalette.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace ayt::render {
namespace {
struct Bin {uint64_t weight=0;std::array<uint64_t,3> sum{};};
struct Box {size_t first=0,last=0;double weight=0,error=0;int axis=0;};
constexpr double metric[3]={.2126,.7152,.0722};
Box measure(size_t first,size_t last,const std::vector<uint16_t>& order,const std::vector<Bin>& bins) {
    Box box{first,last};double sum[3]{},square[3]{};
    for(size_t i=first;i<last;++i) {
        const auto& bin=bins[order[i]];box.weight+=double(bin.weight);
        for(int c=0;c<3;++c){const double v=double(bin.sum[c]);sum[c]+=v;square[c]+=v*v/double(bin.weight);}
    }
    double largest=-1;
    for(int c=0;c<3;++c) {
        const double variance=std::max(0.0,square[c]-sum[c]*sum[c]/box.weight)*metric[c];
        box.error+=variance;if(variance>largest){largest=variance;box.axis=c;}
    }
    return box;
}
}
bool extractStylizationPalette(std::span<const uint8_t> rgba,uint32_t width,uint32_t height,
    uint8_t requested,StylizationPalette& output,std::string* error) {
    if(error)error->clear();
    const auto fail=[&](const char* text){if(error)*error=text;return false;};
    if(requested<2 || requested>16)return fail("Palette size must be between 2 and 16");
    const uint64_t pixels=uint64_t(width)*height;
    if(!width || !height || pixels>16u*1024u*1024u)return fail("Palette image must contain 1..16777216 pixels");
    if(rgba.size()<pixels*4)return fail("Palette image RGBA8 buffer is incomplete");
    std::vector<Bin> bins(32*32*32);uint32_t visible=0;
    for(size_t i=0;i<size_t(pixels);++i) {
        const auto* p=rgba.data()+i*4;if(!p[3])continue;++visible;
        auto& bin=bins[(uint16_t(p[0]>>3)<<10)|(uint16_t(p[1]>>3)<<5)|(p[2]>>3)];
        bin.weight+=p[3];for(int c=0;c<3;++c)bin.sum[c]+=uint64_t(p[c])*p[3];
    }
    if(!visible)return fail("Palette image has no visible pixels");
    std::vector<uint16_t> order;order.reserve(bins.size());
    for(size_t i=0;i<bins.size();++i)if(bins[i].weight)order.push_back(uint16_t(i));
    std::vector<Box> boxes{measure(0,order.size(),order,bins)};
    while(boxes.size()<requested) {
        size_t selected=boxes.size();double largest=-1;
        for(size_t i=0;i<boxes.size();++i)if(boxes[i].last-boxes[i].first>1 && boxes[i].error>largest) {
            selected=i;largest=boxes[i].error;
        }
        if(selected==boxes.size())break;
        const auto box=boxes[selected];
        std::sort(order.begin()+box.first,order.begin()+box.last,[&](uint16_t a,uint16_t b) {
            const double av=double(bins[a].sum[box.axis])/double(bins[a].weight);
            const double bv=double(bins[b].sum[box.axis])/double(bins[b].weight);
            return av==bv?a<b:av<bv;
        });
        uint64_t accumulated=0;size_t cut=box.first+1;
        for(size_t i=box.first;i+1<box.last;++i) {
            accumulated+=bins[order[i]].weight;cut=i+1;
            if(double(accumulated)>=box.weight*.5)break;
        }
        boxes[selected]=measure(box.first,cut,order,bins);
        boxes.push_back(measure(cut,box.last,order,bins));
    }
    StylizationPalette result;result.size=uint8_t(boxes.size());result.contributingPixels=visible;
    for(size_t i=0;i<boxes.size();++i) {
        std::array<uint64_t,3> sum{};uint64_t weight=0;
        for(size_t j=boxes[i].first;j<boxes[i].last;++j) {
            const auto& bin=bins[order[j]];weight+=bin.weight;
            for(int c=0;c<3;++c)sum[c]+=bin.sum[c];
        }
        const double denominator=double(weight)*255.0;
        result.colors[i]={float(double(sum[0])/denominator),float(double(sum[1])/denominator),float(double(sum[2])/denominator)};
    }
    const auto luminance=[](StylizationColor c){return metric[0]*c.r+metric[1]*c.g+metric[2]*c.b;};
    std::sort(result.colors.begin(),result.colors.begin()+result.size,[&](auto a,auto b) {
        const auto av=luminance(a),bv=luminance(b);if(av!=bv)return av<bv;
        if(a.r!=b.r)return a.r<b.r;if(a.g!=b.g)return a.g<b.g;return a.b<b.b;
    });
    if(result.size==1){result.colors[1]=result.colors[0];result.size=2;}
    std::fill(result.colors.begin()+result.size,result.colors.end(),result.colors[result.size-1]);
    output=result;return true;
}
} // namespace ayt::render
