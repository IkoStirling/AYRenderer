#include <AYRenderer/Photometry.h>
#include <AYRenderer.h>
#include <algorithm>
#include <cmath>
#include <locale>
#include <numbers>
#include <sstream>
namespace ayt::render {
namespace {
constexpr double pi=std::numbers::pi;
float smooth(float a,float b,float x){float t=std::clamp((x-a)/(b-a),0.f,1.f);return t*t*(3-2*t);}
}
float spotSolidAngle(float inner,float outer) noexcept {
    if(!std::isfinite(inner)||!std::isfinite(outer)||outer<0||inner>1||inner<=outer)return 0;
    return float(pi*(2.0-double(inner)-double(outer)));
}
float illuminanceLux(float cd,float distance,float cosine) noexcept {
    if(!std::isfinite(cd)||!std::isfinite(distance)||!std::isfinite(cosine)||cd<0||distance<=0)return 0;
    return float(std::min(double(cd)*std::clamp(cosine,0.f,1.f)/(double(distance)*distance),double(3.4028234e38f)));
}
bool IesProfile::parse(std::string_view text,IesProfile& output,std::string* error){
    const auto fail=[&](const char* message){if(error)*error=message;return false;};
    if(text.size()>4*1024*1024||text.find(char(0))!=text.npos)return fail("Invalid IES size or embedded NUL");
    std::istringstream lines{std::string(text)};lines.imbue(std::locale::classic());std::string line;
    if(!std::getline(lines,line))return fail("Missing IES header");
    if(!line.empty()&&line.back()=='\r')line.pop_back();
    if(line!="IESNA:LM-63-1995"&&line!="IESNA:LM-63-2002"&&line!="IES:LM-63-2019")return fail("Unsupported IES revision");
    bool tilt=false;
    while(std::getline(lines,line)){
        if(!line.empty()&&line.back()=='\r')line.pop_back();
        if(line.starts_with("TILT=")){if(line!="TILT=NONE")return fail("Only TILT=NONE is supported");tilt=true;break;}
    }
    if(!tilt)return fail("Missing IES TILT section");
    std::string numbers((std::istreambuf_iterator<char>(lines)),{});
    std::replace(numbers.begin(),numbers.end(),',',' ');
    std::istringstream values(numbers);values.imbue(std::locale::classic());
    double header[13];for(auto& x:header)if(!(values>>x)||!std::isfinite(x))return fail("Invalid IES numeric header");
    const auto integer=[](double x,double low,double high){return x>=low&&x<=high&&std::floor(x)==x;};
    if(!integer(header[0],1,100000)||header[1]<-1||header[2]<=0||header[2]>1e6
        ||!integer(header[3],2,4096)||!integer(header[4],1,4096)||header[5]!=1
        ||(header[6]!=1&&header[6]!=2)||header[10]!=1||header[11]!=1||header[12]<0)
        return fail("Unsupported IES type or header bounds");
    const size_t nv=size_t(header[3]),nh=size_t(header[4]);
    if(nv*nh>262144)return fail("IES sample budget exceeded");
    IesProfile next;next._vertical.resize(nv);next._horizontal.resize(nh);next._candela.resize(nv*nh);
    const auto angles=[&](std::vector<float>& out,float maximum){float previous=-1;for(auto& a:out){double x;if(!(values>>x)||!std::isfinite(x)||x<0||x>maximum||x<=previous)return false;a=float(x);if(a<=previous)return false;previous=a;}return true;};
    if(!angles(next._vertical,180)||!angles(next._horizontal,360))return fail("Invalid IES angle grid");
    if(next._horizontal.front()!=0 || (nh>1&&next._horizontal.back()!=90&&next._horizontal.back()!=180&&next._horizontal.back()!=360))
        return fail("Unsupported IES horizontal symmetry");
    if(next._vertical.front()!=0&&next._vertical.front()!=90)return fail("Unsupported IES vertical domain");
    if(next._vertical.back()!=90&&next._vertical.back()!=180)return fail("Unsupported IES vertical domain");
    for(auto& value:next._candela){double x;if(!(values>>x)||!std::isfinite(x)||x<0||x*header[2]>1e9)return fail("Invalid IES candela sample");value=float(x*header[2]);next._peak=std::max(next._peak,value);}
    values>>std::ws;if(!values.eof())return fail("Trailing IES photometric data");
    if(next._peak<=0)return fail("IES profile has no emitted light");
    if(nh>1&&next._horizontal.back()==360)for(size_t i=0;i<nv;++i)
        if(std::abs(next._candela[i]-next._candela[(nh-1)*nv+i])>std::max(1e-4f,next._peak*1e-4f))return fail("IES360 seam is discontinuous");
    next._mean.resize(nv);
    for(size_t v=0;v<nv;++v){double mean=next._candela[v];if(nh>1){mean=0;for(size_t h=1;h<nh;++h)mean+=(next._candela[h*nv+v]+next._candela[(h-1)*nv+v])*.5*(next._horizontal[h]-next._horizontal[h-1]);mean/=next._horizontal.back();}next._mean[v]=float(mean);}
    output=std::move(next);if(error)error->clear();return true;
}
float IesProfile::sample(float v,float h) const noexcept {
    if(_vertical.empty()||!std::isfinite(v)||!std::isfinite(h)||v<_vertical.front()||v>_vertical.back())return 0;
    h=std::fmod(h,360.f);if(h<0)h+=360;
    if(_horizontal.size()>1){if(_horizontal.back()<=180)h=std::min(h,360-h);if(_horizontal.back()==90&&h>90)h=180-h;}else h=0;
    const auto interval=[](const std::vector<float>& a,float x){auto it=std::upper_bound(a.begin(),a.end(),x);return std::min(size_t(std::max(ptrdiff_t(0),it-a.begin()-1)),a.size()-1);};
    const auto vi=interval(_vertical,v),hi=interval(_horizontal,h);
    const auto vj=std::min(vi+1,_vertical.size()-1),hj=std::min(hi+1,_horizontal.size()-1);
    const float vt=vi==vj?0:(v-_vertical[vi])/(_vertical[vj]-_vertical[vi]);
    const float ht=hi==hj?0:(h-_horizontal[hi])/(_horizontal[hj]-_horizontal[hi]);
    const auto at=[&](size_t x,size_t y){return _candela[x*_vertical.size()+y];};
    return std::lerp(std::lerp(at(hi,vi),at(hi,vj),vt),std::lerp(at(hj,vi),at(hj,vj),vt),ht);
}
float IesProfile::coneSolidAngle(float inner,float outer) const noexcept {
    if(!spotSolidAngle(inner,outer)||_peak<=0)return 0;
    double sum=0;constexpr int n=128;
    for(int i=0;i<n;++i){const float c=float(double(outer)+(1.0-double(outer))*(i+.5)/n);const float theta=float(std::acos(c)*180/pi);
        if(theta<_vertical.front()||theta>_vertical.back())continue;
        auto it=std::upper_bound(_vertical.begin(),_vertical.end(),theta);size_t a=std::min(size_t(it-_vertical.begin()-1),_vertical.size()-1),b=std::min(a+1,_vertical.size()-1);
        const float mean=a==b?_mean[a]:std::lerp(_mean[a],_mean[b],(theta-_vertical[a])/(_vertical[b]-_vertical[a]));
        sum+=mean/_peak*smooth(outer,inner,c);}
    return float(sum*(2*pi)*(1-double(outer))/n);
}
bool IesProfile::projection(float angle,uint16_t size,std::vector<uint8_t>& rgba) const {
    if(_peak<=0||!std::isfinite(angle)||angle<=0||angle>=1.56f||size<8||size>1024)return false;
    std::vector<uint8_t> out(size_t(size)*size*4);const double slope=std::tan(angle);
    for(int y=0;y<size;++y)for(int x=0;x<size;++x){const double px=(2.0*(x+.5)/size-1)*slope,py=(2.0*(y+.5)/size-1)*slope;
        const float theta=float(std::atan(std::hypot(px,py))*180/pi),phi=float(std::atan2(py,px)*180/pi);
        const auto q=uint8_t(std::clamp(std::lround(sample(theta,phi)/_peak*255),0l,255l));const auto k=(size_t(y)*size+x)*4;
        out[k]=out[k+1]=out[k+2]=q;out[k+3]=255;}
    rgba=std::move(out);return true;
}
bool IesProfile::angularTexture(std::vector<uint8_t>& rgba) const {
    if(_peak<=0)return false;std::vector<uint8_t> out(257*129*4);
    for(int y=0;y<129;++y)for(int x=0;x<257;++x){const auto q=uint8_t(std::clamp(std::lround(sample(y*180.f/128,x*360.f/256)/_peak*255),0l,255l));const auto k=(y*257+x)*4;out[k]=out[k+1]=out[k+2]=q;out[k+3]=255;}
    rgba=std::move(out);return true;
}
TextureHandle createIesTexture(Renderer& renderer,const IesProfile& profile){std::vector<uint8_t> data;if(!profile.angularTexture(data))return {};return renderer.createTextureFromRgba8(257,129,data.data());}
} // namespace ayt::render
