#include "AYTest.h"

#include "detail/RenderPipeline.h"

#include <memory>
#include <string_view>

using ayt::render::detail::PassExecContext;
using ayt::render::detail::RenderPass;
using ayt::render::detail::RenderPipeline;

namespace {

class BuiltInShapePass final : public RenderPass {
public:
    std::string_view name() const override { return "BuiltInShape"; }
    uint32_t execute(PassExecContext&) override { return 0; }
};

class DifferentShapePass final : public RenderPass {
public:
    // Deliberately impersonates the other pass's diagnostic name. Type lookup
    // must still return the correct object and never static-cast this instance.
    std::string_view name() const override { return "BuiltInShape"; }
    uint32_t execute(PassExecContext&) override { return 0; }
};

} // namespace

TEST_SUITE(AYRenderer_RenderPipelineTypedLookup_R6)

TEST_CASE(type_lookup_is_not_confused_by_duplicate_diagnostic_names)
{
    RenderPipeline pipeline;
    pipeline.addPass(std::make_unique<DifferentShapePass>());
    pipeline.addPass(std::make_unique<BuiltInShapePass>());

    CHECK(pipeline.findPass("BuiltInShape") != nullptr);
    CHECK(pipeline.findPass<DifferentShapePass>() != nullptr);
    CHECK(pipeline.findPass<BuiltInShapePass>() != nullptr);
    CHECK(static_cast<RenderPass*>(pipeline.findPass<DifferentShapePass>())
          != static_cast<RenderPass*>(pipeline.findPass<BuiltInShapePass>()));
}

TEST_CASE(type_lookup_returns_null_for_an_absent_type)
{
    RenderPipeline pipeline;
    pipeline.addPass(std::make_unique<BuiltInShapePass>());

    CHECK(pipeline.findPass<DifferentShapePass>() == nullptr);
}

TEST_CASE(const_pipeline_returns_const_typed_pointer)
{
    RenderPipeline pipeline;
    pipeline.addPass(std::make_unique<BuiltInShapePass>());
    const RenderPipeline& constPipeline = pipeline;

    const BuiltInShapePass* pass = constPipeline.findPass<BuiltInShapePass>();
    CHECK(pass != nullptr);
    CHECK(pass->name() == "BuiltInShape");
}

TEST_CASE(duplicate_concrete_types_preserve_first_registration)
{
    RenderPipeline pipeline;
    auto first = std::make_unique<BuiltInShapePass>();
    BuiltInShapePass* firstRaw = first.get();
    pipeline.addPass(std::move(first));
    pipeline.addPass(std::make_unique<BuiltInShapePass>());

    CHECK(pipeline.findPass<BuiltInShapePass>() == firstRaw);
}

TEST_CASE(clear_invalidates_the_type_index)
{
    RenderPipeline pipeline;
    pipeline.addPass(std::make_unique<BuiltInShapePass>());
    CHECK(pipeline.findPass<BuiltInShapePass>() != nullptr);

    pipeline.clear();

    CHECK(pipeline.findPass<BuiltInShapePass>() == nullptr);
    CHECK(pipeline.passes().empty());
}

TEST_SUITE_END
