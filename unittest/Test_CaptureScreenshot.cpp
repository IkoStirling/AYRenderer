// Test_CaptureScreenshot.cpp — R4-c captureScreenshot API guards

#include "AYRenderer.h"
#include "AYTest.h"
#include "detail/ScreenshotSidecar.h"
#include "detail/TextureImageLoader.h"
#include "AYRenderer/ColorStylizationLook.h"
#include <AYIO/File.h>

#include <sys/stat.h>

#include <iostream>
#include <string>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <array>

#ifndef AY_SHADER_SHADERC_HINT
#  define AY_SHADER_SHADERC_HINT ""
#endif

namespace {

const char* kUnlitMaterial = R"(
material Unlit {
    property baseColor = vec4(1.0, 1.0, 1.0, 1.0);

    vertex {
        in  position : position;
        out position : position;
        return vec4(position, 1.0);
    }
    fragment {
        in  position : position;
        return baseColor;
    }
}
)";

bool fileExists(const std::string& path)
{
    if (path.empty()) {
        return false;
    }
    struct stat st;
    return ::stat(path.c_str(), &st) == 0;
}

bool shadercAvailable()
{
    return fileExists(AY_SHADER_SHADERC_HINT);
}

} // namespace

TEST_SUITE(RendererCaptureScreenshotTests)

TEST_CASE(delayed_screenshot_callback_never_reuses_stale_or_partial_output)
{
    using namespace ayt::render::detail;
    const auto directory=std::filesystem::temp_directory_path()/
        ("ay-capture-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(directory);
    struct Cleanup {std::filesystem::path path;~Cleanup(){std::error_code e;std::filesystem::remove_all(path,e);}} cleanup{directory};
    auto writeTga=[](const std::string& base,uint8_t red,bool complete) {
        const uint8_t bytes[]={0,0,2,0,0,0,0,0,0,0,0,0,2,0,1,0,32,32,0,0,red,255,0,0,red,255};
        std::ofstream file(screenshotTgaPath(base),std::ios::binary);file.write(reinterpret_cast<const char*>(bytes),complete?26:22);
    };
    const auto output=(directory/"same-output").string();
    writeTga(output,17,true);CHECK(finalizeScreenshotSidecar(output,output));
    ScreenshotCaptureQueue queue;
    const auto first=queue.enqueue(output),second=queue.enqueue(output);
    CHECK(!first.empty() && first!=second);CHECK(queue.size()==2);
    writeTga(second,211,true);queue.advance();CHECK(queue.size()==2);
    auto image=decodeImageFile(screenshotPngPath(output));CHECK(image.rgba8.size()==8);if(image.rgba8.size()==8)CHECK(image.rgba8[0]==17);
    writeTga(first,53,false);queue.advance();CHECK(queue.size()==2);
    writeTga(first,53,true);queue.advance();CHECK(queue.size()==0);
    image=decodeImageFile(screenshotPngPath(output));CHECK(image.rgba8.size()==8);if(image.rgba8.size()==8)CHECK(image.rgba8[0]==211);
    CHECK(!std::filesystem::exists(screenshotTgaPath(first)));CHECK(!std::filesystem::exists(screenshotTgaPath(second)));
    CHECK(std::filesystem::exists(screenshotTgaPath(output)));
}

TEST_CASE(screenshot_callback_queue_is_bounded_and_shutdown_clears_unfinished_requests)
{
    using namespace ayt::render::detail;
    ScreenshotCaptureQueue queue;
    CHECK(queue.enqueue("").empty());
    for(size_t i=0;i<ScreenshotCaptureQueue::kCapacity;++i)CHECK(!queue.enqueue("missing-capture-directory/frame").empty());
    CHECK(queue.full());CHECK(queue.enqueue("overflow").empty());
    queue.advance(true);CHECK(queue.size()==0);CHECK(!queue.full());
}

TEST_CASE(capture_screenshot_rejects_invalid_requests)
{
    ayt::render::Renderer renderer;

    CHECK(!renderer.captureScreenshot(""));
    CHECK(!renderer.captureScreenshot("shot.png"));

    ayt::render::InitDesc desc;
    desc.backend = ayt::render::Backend::Noop;
    desc.width   = 640;
    desc.height  = 480;
    CHECK(renderer.initialize(desc));
    CHECK(!renderer.captureScreenshot("shot.png"));

    renderer.shutdown();
}

TEST_CASE(utf8_raster_decode_and_look_publication_use_native_paths)
{
    namespace fs=std::filesystem;
    const auto root=fs::temp_directory_path()/("ay-look-utf8-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup{fs::path path;~Cleanup(){std::error_code e;fs::remove_all(path,e);}} cleanup{root};
    const auto asUtf8=[](const fs::path& p){const auto s=p.u8string();return std::string(s.begin(),s.end());};
    const auto imagePath=root/fs::u8path("参考图-🌙.tga");
    const uint8_t bytes[]={0,0,2,0,0,0,0,0,0,0,0,0,1,0,1,0,32,32,0,0,255,255};
    CHECK(ayt::io::File::atomicWriteUtf8(asUtf8(imagePath),bytes,sizeof(bytes)));
    const auto image=ayt::render::detail::decodeImageFile(asUtf8(imagePath));
    CHECK(image.width==1 && image.height==1);CHECK(image.rgba8.size()==4);
    if(image.rgba8.size()==4)CHECK(image.rgba8[0]==255 && image.rgba8[1]==0 && image.rgba8[2]==0 && image.rgba8[3]==255);
    const auto look=ayt::render::colorStylizationPreset(ayt::render::ColorStylizationPreset::TimeEcho);
    const auto path=asUtf8(root/fs::u8path("作品/嵌套/色板-🌙.rsl"));std::string error;
    CHECK(ayt::render::saveColorStylizationLook(path,look,&error));CHECK(error.empty());
    ayt::render::ColorStylizationSettings loaded;
    CHECK(ayt::render::loadColorStylizationLook(path,loaded,&error));
    CHECK(ayt::render::serializeColorStylizationLook(loaded)==ayt::render::serializeColorStylizationLook(look));
}

TEST_CASE(capture_screenshot_queues_without_crash_on_noop_frame)
{
    if (!shadercAvailable()) {
        std::cerr << "[Renderer test] SKIP: shaderc not available.\n";
        return;
    }

    ayt::render::Renderer renderer;
    ayt::render::InitDesc desc;
    desc.backend = ayt::render::Backend::Noop;
    desc.width   = 800;
    desc.height  = 600;
    CHECK(renderer.initialize(desc));

    ayt::render::MaterialHandle material =
        renderer.createMaterialFromPhoskia(kUnlitMaterial, "capture_unlit");
    if (!material.isValid()) {
        std::cerr << "[Renderer test] SKIP: material acquire failed.\n";
        renderer.shutdown();
        return;
    }

    ayt::render::MeshHandle mesh = renderer.createUnitCube();
    CHECK(mesh.isValid());

    ayt::render::RenderScene scene;
    scene.add(mesh, material);

    renderer.beginFrame({});
    renderer.render(scene);
    CHECK(!renderer.captureScreenshot("noop_shot.png"));
    renderer.endFrame();

    CHECK(!fileExists("noop_shot.png"));

    renderer.destroyMesh(mesh);
    renderer.destroyMaterial(material);
    renderer.shutdown();
}

TEST_SUITE_END
