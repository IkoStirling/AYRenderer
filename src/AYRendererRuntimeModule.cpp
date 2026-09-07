#include <AYRenderer/RendererRuntimeModule.h>

#include <AYRenderer/RendererSubSystem.h>

#include <memory>
#include <string>

namespace ayt::render
{

RendererRuntimeModule::RendererRuntimeModule()
    : SubSystemModule(
          ayt::module::ModuleDescriptor{
              .id = std::string(kRendererRuntimeModuleId),
              .displayName = "AYRenderer Runtime",
              .version = "0.1.0",
              .dependencies = {
                  ayt::module::ModuleDependency::optional(
                      "AYDevice.Runtime")}},
          "Renderer",
          []() {
              return std::make_unique<RendererSubSystem>();
          })
{
}

} // namespace ayt::render
