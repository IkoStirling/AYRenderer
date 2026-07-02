#pragma once

#include <string>

namespace ayt::render::detail
{

std::string screenshotBasePath(const std::string& filePath);
std::string screenshotTgaPath(const std::string& basePath);
std::string screenshotPngPath(const std::string& basePath);

// After bgfx::frame(), convert the default .tga sidecar to .png on the main thread.
void finalizeScreenshotSidecar(const std::string& basePath);

} // namespace ayt::render::detail
