#pragma once
#ifndef STREAMLINE_LEGACY
#include <Mod.hpp>
#include <DescriptorHeap.h>
#include <atomic>
#include <d3d12.h>
#include <mods/vr/d3d12/ComPtr.hpp>
#include <mods/vr/d3d12/CommandContext.hpp>
#include <sl.h>
#include <sl_dlss.h>
#ifdef STREAMLINE_DLSS_RR
#include <sl_dlss_d.h>
#endif
//#include <sl_deepdvc.h>
#include <utility/PointerHook.hpp>
#include <memory/FunctionHook.h>
#include <nvidia/MotionVectorReprojection.h>

class UpscalerAfrNvidiaModule : public Mod
{
public:
    inline static std::shared_ptr<UpscalerAfrNvidiaModule>& Get()
    {
        static auto instance = std::make_shared<UpscalerAfrNvidiaModule>();
        return instance;
    }

    // Mod interface implementation
    std::string_view get_name() const override { return "DLSS_AER"; }
    
    std::optional<std::string> on_initialize() override;
    void on_draw_ui() override;
    void on_config_load(const utility::Config& cfg, bool set_defaults) override;
    void on_config_save(utility::Config& cfg) override;
    void on_device_reset() override;
    void on_d3d12_initialize(ID3D12Device4* pDevice4, D3D12_RESOURCE_DESC& desc) override;

    UpscalerAfrNvidiaModule() = default;
    ~UpscalerAfrNvidiaModule() override = default;

private:
    void InstallHooks();

    // Motion vector reprojection component
#ifdef MOTION_VECTOR_REPROJECTION
    MotionVectorReprojection m_motion_vector_reprojection{};
#endif

    std::unique_ptr<FunctionHook> m_get_new_frame_token_hook{nullptr};
    std::unique_ptr<FunctionHook> m_set_tag_hook{nullptr};
    std::unique_ptr<FunctionHook> m_evaluate_feature_hook{nullptr};
    std::unique_ptr<FunctionHook> m_set_constants_hook{nullptr};
    std::unique_ptr<FunctionHook> m_allocate_resources_hook{nullptr};
    std::unique_ptr<FunctionHook> m_free_resources_hook{nullptr};

//    std::unique_ptr<PointerHook> m_dlss_set_options_hook{nullptr};
    std::unique_ptr<FunctionHook> m_dlss_set_options_hook{nullptr};
    std::unique_ptr<FunctionHook> m_dlssrr_set_options_hook{nullptr};
    std::unique_ptr<FunctionHook> m_sl_dvc_set_options_hook{nullptr};

    void ReprojectMotionVectors(const sl::FrameToken& frame, sl::BaseStructure** inputs, uint32_t numInputs, void* cmdBuffer);

//    static sl::Result on_slGetNewFrameToken(sl::FrameToken*& token, const uint32_t* frameIndex = nullptr);
    static sl::Result on_slSetTag(sl::ViewportHandle& viewport, const sl::ResourceTag* tags, uint32_t numTags, sl::CommandBuffer* cmdBuffer);
    static sl::Result on_slEvaluateFeature(sl::Feature feature, const sl::FrameToken& frame, sl::BaseStructure** inputs, uint32_t numInputs, sl::CommandBuffer* cmdBuffer);
    static sl::Result on_slSetConstants(sl::Constants& values, const sl::FrameToken& frame, sl::ViewportHandle& viewport);
    static sl::Result on_dlssSetOptions(const sl::ViewportHandle& viewport, const sl::DLSSOptions& options);
#ifdef STREAMLINE_DLSS_RR
    static sl::Result on_dlssrrSetOptions(const sl::ViewportHandle& viewport, const sl::DLSSDOptions& options);
#endif
//    static sl::Result on_slDVCSetOptions(const sl::ViewportHandle& viewport, const sl::DeepDVCOptions& options);
    static sl::Result on_slAllocateResources(sl::CommandBuffer* cmdBuffer, sl::Feature feature, const sl::ViewportHandle& viewport);
    static sl::Result on_slFreeResources(sl::Feature feature, const sl::ViewportHandle& viewport);

    // ModValue settings
    const ModToggle::Ptr m_enabled{ ModToggle::create(generate_name("Enabled"), true) };
    const ModToggle::Ptr m_motion_vector_fix{ ModToggle::create(generate_name("MotionVectorFix"), true) };
    const ModToggle::Ptr m_free_stale_on_reschange{ ModToggle::create(generate_name("FreeStaleOnResChange"), true) };

    ValueList m_options{
        *m_enabled,
        *m_free_stale_on_reschange,
#ifdef MOTION_VECTOR_REPROJECTION
        *m_motion_vector_fix
#endif
    };
};
#endif