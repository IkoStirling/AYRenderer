#include "AYRenderer/ColorStylizationLook.h"
#include "AYRenderer/StylizationPalette.h"
#include "detail/TextureImageLoader.h"
#include "DemoUtf8Arguments.h"
#include <charconv>
#include <cstdio>
#include <string_view>

int runRenderStyleTool(int argc,char** argv) {
    using namespace ayt::render;
    if(argc==3 && std::string_view(argv[1])=="inspect") {
        ColorStylizationSettings look;std::string error;
        if(!loadColorStylizationLook(argv[2],look,&error)){std::fprintf(stderr,"%s\n",error.c_str());return 1;}
        std::puts(serializeColorStylizationLook(look).c_str());return 0;
    }
    if((argc==4 || argc==5) && std::string_view(argv[1])=="palette") {
        int count=8;
        if(argc==5) {
            const std::string_view text(argv[4]);const auto parsed=std::from_chars(text.data(),text.data()+text.size(),count);
            if(parsed.ec!=std::errc{} || parsed.ptr!=text.data()+text.size() || count<2 || count>16) {
                std::fprintf(stderr,"Palette color count must be an integer from 2 to 16\n");return 2;
            }
        }
        const auto image=detail::decodeImageFile(argv[2]);StylizationPalette palette;std::string error;
        if(!extractStylizationPalette(image.rgba8,image.width,image.height,uint8_t(count),palette,&error)) {
            std::fprintf(stderr,"%s: %s\n",argv[2],error.c_str());return 1;
        }
        ColorStylizationSettings look;look.enabled=true;look.pixelationEnabled=false;look.posterizationEnabled=false;
        look.colorMapping=StylizationColorMapping::FixedPalette;look.paletteSize=palette.size;look.palette=palette.colors;
        if(!saveColorStylizationLook(argv[3],look,&error)){std::fprintf(stderr,"%s\n",error.c_str());return 1;}
        std::printf("Saved %s: %u colors from %u visible pixels (%ux%u)\n",argv[3],unsigned(palette.size),palette.contributingPixels,image.width,image.height);
        for(int i=0;i<palette.size;++i)std::printf("#%02X%02X%02X%s",unsigned(palette.colors[i].r*255+.5f),unsigned(palette.colors[i].g*255+.5f),unsigned(palette.colors[i].b*255+.5f),i+1==palette.size?"\n":" ");
        return 0;
    }
    std::fprintf(stderr,"Usage: AYRenderer_RenderStyleTool palette <image> <output.rsl> [colors 2..16]\n"
                        "       AYRenderer_RenderStyleTool inspect <look.rsl>\n");return 2;
}

#ifdef _WIN32
int wmain(int argc,wchar_t** argv) {
    ayt::render::demo::Utf8Arguments arguments(argc,argv);
    return runRenderStyleTool(argc,arguments.pointers.data());
}
#else
int main(int argc,char** argv){return runRenderStyleTool(argc,argv);}
#endif
