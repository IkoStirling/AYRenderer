#pragma once

#include <string>

namespace ayt::render::detail
{

std::string screenshotBasePath(const std::string& filePath);
std::string screenshotTgaPath(const std::string& basePath);
std::string screenshotPngPath(const std::string& basePath);

// Retry on later frames until the asynchronous bgfx callback has written TGA.
// Returns true only after decoding and PNG publication; owner thread only.
bool finalizeScreenshotSidecar(const std::string& basePath);

} // namespace ayt::render::detail
