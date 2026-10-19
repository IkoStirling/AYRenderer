#pragma once
#include <AYRenderer/EmissivePointCloud.h>
#include <atomic>
#include <memory>
namespace ayt::render::detail {
class BGFXAdapter;
inline constexpr uint16_t kPointMarkView=50,kPointCountView=51,kPointPrefixView=52,kPointCompactView=53,kPointReadbackView=54;
class GpuPointState;
class GpuPointContext final : public std::enable_shared_from_this<GpuPointContext> {
public:
    static std::shared_ptr<GpuPointContext> create(BGFXAdapter& adapter,
        std::shared_ptr<std::atomic<bool>> alive,std::string* error);
    ~GpuPointContext();
    std::shared_ptr<GpuPointState> createState(std::string* error);
    void poll() noexcept;
    void releaseBeforeDeviceShutdown() noexcept;
    void afterDeviceShutdown() noexcept;
    bool alive() const noexcept;
private:
    GpuPointContext();struct Impl;std::unique_ptr<Impl> _impl;
    friend class GpuPointState;
};
class GpuPointState final {
public:
    ~GpuPointState();
    bool alive() const noexcept;
    bool prepare(std::span<const EmissivePoint> points,const math::Float4x4& view,
        const math::Float4x4& projection,uint16_t width,uint16_t height,
        EmissivePointSettings settings,EmissivePointStats& stats,std::string* error);
    void submit(uint16_t view);
    void releaseGpu() noexcept;
private:
    explicit GpuPointState(std::shared_ptr<GpuPointContext> context);
    struct Impl;std::unique_ptr<Impl> _impl;
    friend class GpuPointContext;
};
} // namespace ayt::render::detail
