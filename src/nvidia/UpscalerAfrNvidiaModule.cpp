#ifndef STREAMLINE_LEGACY
#include "UpscalerAfrNvidiaModule.h"
#include <algorithm>
#include <atomic>
#include <mutex>
#include <vector>

static std::atomic<bool>     s_dlss_options_reset{true};
static std::atomic<uint64_t> s_dlss_stable_wh{0}; // high32=width, low32=height

static std::atomic<uint64_t> s_shared_viewports{(uint64_t(1024u + 1u) << 32) | 0u}; // low32=stable, high32=afr

struct CachedViewportPair
{
    uint64_t     wh;
    sl::DLSSMode mode;
    uint64_t     pair;
};
static std::mutex                     s_viewport_cache_mutex{};
static std::vector<CachedViewportPair> s_viewport_pair_cache{}; // guarded by s_viewport_cache_mutex
static uint32_t                       s_viewport_generation{0}; // guarded by s_viewport_cache_mutex

namespace {
    constexpr uint32_t kRotatedViewportBase = 0x56520000;

    uint32_t stable_viewport_id() { return (uint32_t)s_shared_viewports.load(std::memory_order_relaxed); }
    uint32_t afr_viewport_id() { return (uint32_t)(s_shared_viewports.load(std::memory_order_relaxed) >> 32); }
}


#ifdef _DEBUG
#include <nvidia/ShaderDebugOverlay.h>
#endif
#include "sl_matrix_helpers.h"
#include <Framework.hpp>
#include <experimental/DebugUtils.h>
#include <imgui.h>
#include <mods/VR.hpp>
#include <spdlog/spdlog.h>

std::optional<std::string> UpscalerAfrNvidiaModule::on_initialize()
{
    InstallHooks();
    return Mod::on_initialize();
}

void UpscalerAfrNvidiaModule::on_draw_ui()
{
    if (!ImGui::CollapsingHeader(get_name().data())) {
        return;
    }

    m_enabled->draw("Enable NVIDIA AFR");
    m_free_stale_on_reschange->draw("Free stale DLSS resources on resolution change");
#ifdef MOTION_VECTOR_REPROJECTION
    m_motion_vector_fix->draw("Motion Vector Reprojection Fix");
#endif
}

void UpscalerAfrNvidiaModule::on_config_load(const utility::Config& cfg, bool set_defaults)
{
    for (IModValue& option : m_options) {
        option.config_load(cfg, set_defaults);
    }
}

void UpscalerAfrNvidiaModule::on_config_save(utility::Config& cfg)
{
    for (IModValue& option : m_options) {
        option.config_save(cfg);
    }
}

void UpscalerAfrNvidiaModule::on_device_reset()
{
    s_dlss_options_reset = true;
#ifdef MOTION_VECTOR_REPROJECTION
    m_motion_vector_reprojection.on_device_reset();
#endif
}

void UpscalerAfrNvidiaModule::on_d3d12_initialize(ID3D12Device4* pDevice4, D3D12_RESOURCE_DESC& desc)
{
#ifdef MOTION_VECTOR_REPROJECTION
    m_motion_vector_reprojection.on_d3d12_initialize(pDevice4, desc);
#endif
}

void UpscalerAfrNvidiaModule::InstallHooks()
{
    static HMODULE p_hm_sl_interposter = nullptr;
    if (p_hm_sl_interposter) {
        return;
    }
    spdlog::info("Installing sl.interposer hooks");
    while ((p_hm_sl_interposter = GetModuleHandle("sl.interposer.dll")) == nullptr) {
        std::this_thread::yield();
    }

//    auto getNewFrameTokenFn = GetProcAddress(g_interposer, "slGetNewFrameToken");
//    m_get_new_frame_token_hook = std::make_unique<FunctionHook>((void **)getNewFrameTokenFn, (void *) &DlssDualView::on_slGetNewFrameToken);
//    if(!m_get_new_frame_token_hook->create()) {
//        spdlog::error("Failed to hook slGetNewFrameToken");
//    }

    auto slSetTagFn = GetProcAddress(p_hm_sl_interposter, "slSetTag");
    m_set_tag_hook  = std::make_unique<FunctionHook>((void**)slSetTagFn, (void*)&UpscalerAfrNvidiaModule::on_slSetTag);
    if (!m_set_tag_hook->create()) {
        spdlog::error("Failed to hook slSetTag");
    }

    auto slEvaluateFeatureFn = GetProcAddress(p_hm_sl_interposter, "slEvaluateFeature");
    m_evaluate_feature_hook  = std::make_unique<FunctionHook>((void**)slEvaluateFeatureFn, (void*)&UpscalerAfrNvidiaModule::on_slEvaluateFeature);
    if (!m_evaluate_feature_hook->create()) {
        spdlog::error("Failed to hook slEvaluateFeature");
    }

    auto slSetConstantsFn = GetProcAddress(p_hm_sl_interposter, "slSetConstants");
    m_set_constants_hook  = std::make_unique<FunctionHook>((void**)slSetConstantsFn, (void*)&UpscalerAfrNvidiaModule::on_slSetConstants);
    if (!m_set_constants_hook->create()) {
        spdlog::error("Failed to hook slSetConstants");
    }

    using get_feature_funciton = sl::Result (*)(sl::Feature feature, const char* functionName, void*& function);
    auto get_feature_function = (get_feature_funciton) GetProcAddress(p_hm_sl_interposter, "slGetFeatureFunction");
    void* dlssSetOptionsFn;
    get_feature_function(sl::kFeatureDLSS, "slDLSSSetOptions", dlssSetOptionsFn);
    m_dlss_set_options_hook = std::make_unique<FunctionHook>((void**)dlssSetOptionsFn, (void*)&UpscalerAfrNvidiaModule::on_dlssSetOptions);
    if (!m_dlss_set_options_hook->create()) {
        spdlog::error("Failed to hook slSetOptions");
    }
//    void* slDLSSDSetOptionsFn;
//    get_feature_function(sl::kFeatureDLSS_RR, "slDLSSDSetOptions", slDLSSDSetOptionsFn);
//    m_dlssrr_set_options_hook = std::make_unique<FunctionHook>((void**)slDLSSDSetOptionsFn, (void*)&UpscalerAfrNvidiaModule::on_dlssrrSetOptions);
//    if (!m_dlssrr_set_options_hook->create()) {
//        spdlog::error("Failed to hook slDLSSDSetOptions");
//    }

#ifdef STREAMLINE_DLSS_RR
    void* slDLSSDSetOptionsFn;
    get_feature_function(sl::kFeatureDLSS_RR, "slDLSSDSetOptions", slDLSSDSetOptionsFn);
    m_dlssrr_set_options_hook = std::make_unique<FunctionHook>((void**)slDLSSDSetOptionsFn, (void*)&UpscalerAfrNvidiaModule::on_dlssrrSetOptions);
    if (!m_dlssrr_set_options_hook->create()) {
        spdlog::error("Failed to hook slDLSSDSetOptions");
    }
#endif

    auto slAllocateResourcesFn = GetProcAddress(p_hm_sl_interposter, "slAllocateResources");
    m_allocate_resources_hook  = std::make_unique<FunctionHook>((void**)slAllocateResourcesFn, (void*)&UpscalerAfrNvidiaModule::on_slAllocateResources);
    if (!m_allocate_resources_hook->create()) {
        spdlog::error("Failed to hook slAllocateResources");
    }

    auto slFreeResourcesFn = GetProcAddress(p_hm_sl_interposter, "slFreeResources");
    m_free_resources_hook  = std::make_unique<FunctionHook>((void**)slFreeResourcesFn, (void*)&UpscalerAfrNvidiaModule::on_slFreeResources);
    if (!m_free_resources_hook->create()) {
        spdlog::error("Failed to hook slFreeResources");
    }

//        REL::Relocation<uintptr_t>  getDllsFrameTokenFnAddr{REL::ID(1078687)};
//        m_onGetDllsFrameToken = std::make_unique<FunctionHook>(getDllsFrameTokenFnAddr.address(), (uintptr_t)&DlssDualView::on_ceGetNewFrameToken);
//        m_onGetDllsFrameToken->create();

    spdlog::info("sl.interposer Hooks installed");
}

/*sl::Result UpscalerAfrNvidiaModule::on_slGetNewFrameToken(sl::FrameToken*& token, const uint32_t* frameIndex)
{
    auto instance = UpscalerAfrNvidiaModule::Get();

    auto original_fn = instance->m_get_new_frame_token_hook->get_original<decltype(UpscalerAfrNvidiaModule::on_slGetNewFrameToken)>();
    auto vr          = VR::get();
    //    spdlog::info("slGetNewFrameToken called {} frame {}", vr->get_current_render_eye() == VRRuntime::Eye::RIGHT,*frameIndex);
    //
    //    if(vr->is_hmd_active()  && !vr->m_left_trigger_down && vr->get_current_render_eye() == VRRuntime::Eye::RIGHT && instance->m_afr_frame_token != nullptr) {
    //        token = instance->m_afr_frame_token;
    //        return sl::Result::eOk;
    //
    //    }
    //    if(instance->first_frame == 0) {
    //        instance->first_frame = *frameIndex;
    //    }
    auto half_frame = *frameIndex / 2;
    auto result     = original_fn(token, frameIndex);
    //    original_fn(instance->m_afr_frame_token, &half_frame);
    return result;
}*/

#ifdef MOTION_VECTOR_REPROJECTION
void UpscalerAfrNvidiaModule::ReprojectMotionVectors(const sl::FrameToken& frame, sl::BaseStructure** inputs, uint32_t numInputs, sl::CommandBuffer* cmdBuffer) {
    if(!m_motion_vector_reprojection.isInitialized()) {
        return;
    }
    sl::ResourceTag* mvTag = nullptr;
    sl::ResourceTag* depthTag = nullptr;
    for (uint32_t i = 0; i < numInputs; i++) {
        if(inputs[i]->structType == sl::ResourceTag::s_structType && ((sl::ResourceTag*) inputs[i])->type == sl::kBufferTypeMotionVectors) {
            mvTag = (sl::ResourceTag*)inputs[i];
        } else if(inputs[i]->structType == sl::ResourceTag::s_structType && ((sl::ResourceTag*) inputs[i])->type == sl::kBufferTypeDepth) {
            depthTag = (sl::ResourceTag*)inputs[i];
        }
    }
    static auto vr = VR::get();

    if(mvTag && depthTag && mvTag->resource && mvTag->resource->native && depthTag->resource && depthTag->resource->native) {
        auto mv_resource        = mvTag->resource;
        auto mv_state           = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        auto mv_native_resource = (ID3D12Resource*)mv_resource->native;

        auto depth_resource        = depthTag->resource;
        auto depth_state           = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        auto depth_native_resource = (ID3D12Resource*)depth_resource->native;

        auto command_list    = (ID3D12GraphicsCommandList*)cmdBuffer;
        if(m_enabled->value() && m_motion_vector_fix->value()) {
            m_motion_vector_reprojection.ProcessMotionVectors(mv_native_resource, mv_state, depth_native_resource, depth_state, vr->m_render_frame_count, command_list);
        }
#ifdef _DEBUG
        static auto shader_debug_overlay = ShaderDebugOverlay::Get();
        if(DebugUtils::config.debugShaders && ShaderDebugOverlay::ValidateResource(mv_native_resource, shader_debug_overlay->m_motion_vector_buffer)) {
            ShaderDebugOverlay::CopyResource(command_list, mv_native_resource, shader_debug_overlay->m_motion_vector_buffer[vr->m_render_frame_count % 2].Get(), mv_state, D3D12_RESOURCE_STATE_GENERIC_READ);
        }
#endif
    }
}
#endif


sl::Result UpscalerAfrNvidiaModule::on_slSetTag(sl::ViewportHandle& viewport, const sl::ResourceTag* tags, uint32_t numTags, sl::CommandBuffer* cmdBuffer)
{
    static auto            instance    = UpscalerAfrNvidiaModule::Get();
    static auto            original_fn = instance->m_set_tag_hook->get_original<decltype(UpscalerAfrNvidiaModule::on_slSetTag)>();
    static auto            vr          = VR::get();
    if(vr->m_render_frame_count % 2 == 0 && instance->m_enabled->value()) {
        sl::ViewportHandle afr_viewport_handle{afr_viewport_id()};
        return original_fn(afr_viewport_handle, tags, numTags, cmdBuffer);
    }
    sl::ViewportHandle stable_viewport{stable_viewport_id()};
    return original_fn(stable_viewport, tags, numTags, cmdBuffer);
}

namespace {
    bool supported_afr_feature(sl::Feature feature)
    {
#ifdef STREAMLINE_DLSS_RR
        if(feature == sl::kFeatureDLSS_RR) {
            return true;
        }
#endif
        return feature == sl::kFeatureDLSS;
    }

}

sl::Result UpscalerAfrNvidiaModule::on_slEvaluateFeature(sl::Feature feature, const sl::FrameToken& frame, sl::BaseStructure** inputs, uint32_t numInputs, sl::CommandBuffer* cmdBuffer)
{
    static auto instance    = UpscalerAfrNvidiaModule::Get();
    static auto original_fn = instance->m_evaluate_feature_hook->get_original<decltype(UpscalerAfrNvidiaModule::on_slEvaluateFeature)>();
    static auto vr          = VR::get();
//
//    constexpr sl::BufferType kBufferTypeReactiveMaskHint = 36;
//
//    // Filter out kBufferTypeReactiveMaskHint inputs
//    std::vector<sl::BaseStructure*> filtered_inputs;
//    for (uint32_t i = 0; i < numInputs; i++) {
//        if (inputs[i]->structType == sl::ResourceTag::s_structType) {
//            auto resourceTag = (sl::ResourceTag*)inputs[i];
//            if (resourceTag->type == kBufferTypeReactiveMaskHint) {
//                continue; // Skip this input
//            }
//        }
//        filtered_inputs.push_back(inputs[i]);
//    }
//
//    uint32_t filteredNumInputs = static_cast<uint32_t>(filtered_inputs.size());
#ifdef MOTION_VECTOR_REPROJECTION
    //TODO might process same resource twice per frame we to track last processed resource per frame

    if(supported_afr_feature(feature)) {
        /**
        * DLSS uses per-pixel motion vectors as a key component of its core algorithm. The motion vectors map a
        * pixel from the current frame to its position in the previous frame. That is, when the motion vector for
        * the pixel is added to the pixel's current location, the result is the location the pixel occupied in the
        * previous frame
        * MV = PAST - CURRENT
        * DLSS internally expect mvector to be in UV space notmalized to [-1, 1] it's not NDC space
        */
        instance->ReprojectMotionVectors(frame, inputs, numInputs, cmdBuffer);
    }
#endif

    if(supported_afr_feature(feature)) {
        sl::ViewportHandle target_viewport = (frame % 2 == 0 && instance->m_enabled->value())
            ? sl::ViewportHandle{afr_viewport_id()}
            : sl::ViewportHandle{stable_viewport_id()};
        std::vector<sl::BaseStructure*> remapped_inputs(numInputs);
        for (uint32_t i = 0; i < numInputs; i++) {
            remapped_inputs[i] = (inputs[i]->structType == sl::ViewportHandle::s_structType) ? &target_viewport : inputs[i];
        }
        return original_fn(feature, frame, remapped_inputs.data(), numInputs, cmdBuffer);
    }
    return original_fn(feature, frame, inputs, numInputs, cmdBuffer);
}

// TODO vr->get_current_render_eye() == VRRuntime::Eye::RIGHT in real is left eye texture
sl::Result UpscalerAfrNvidiaModule::on_slSetConstants(sl::Constants& values, const sl::FrameToken& frame, sl::ViewportHandle& viewport)
{
    static auto instance = UpscalerAfrNvidiaModule::Get();
    static auto original_fn = instance->m_set_constants_hook->get_original<decltype(UpscalerAfrNvidiaModule::on_slSetConstants)>();
#ifdef MOTION_VECTOR_REPROJECTION
    instance->m_motion_vector_reprojection.m_mvecScale.x = values.mvecScale.x;
    instance->m_motion_vector_reprojection.m_mvecScale.y = values.mvecScale.y;
#endif

#ifdef _DEBUG
    if(DebugUtils::config.debugShaders) {
        ShaderDebugOverlay::SetMvecScale(values.mvecScale.x, values.mvecScale.y);
    }
#endif


    if(frame % 2 == 0 && instance->m_enabled->value()) {
        sl::ViewportHandle afr_viewport_handle{afr_viewport_id()};
        return original_fn(values, frame, afr_viewport_handle);
    }
    sl::ViewportHandle stable_viewport{stable_viewport_id()};
    return original_fn(values, frame, stable_viewport);
}

//sl::Result UpscalerAfrNvidiaModule::on_slDVCSetOptions(const sl::ViewportHandle &viewport, const sl::DeepDVCOptions &options) {
//    static auto     instance         = UpscalerAfrNvidiaModule::Get();
//    static auto     original_fn      = instance->m_sl_dvc_set_options_hook->get_original<decltype(UpscalerAfrNvidiaModule::on_slDVCSetOptions)>();
//    {
//        sl::ViewportHandle afr_viewport_handle{instance->afr_viewport_id};
//        original_fn(afr_viewport_handle, options);
//    }
//    return original_fn(viewport, options);
//}

#ifdef STREAMLINE_DLSS_RR
sl::Result UpscalerAfrNvidiaModule::on_dlssrrSetOptions(const sl::ViewportHandle &viewport, const sl::DLSSDOptions &options) {
    static auto     instance         = UpscalerAfrNvidiaModule::Get();
    static auto     original_fn      = instance->m_dlssrr_set_options_hook->get_original<decltype(UpscalerAfrNvidiaModule::on_dlssrrSetOptions)>();
    if(instance->m_enabled->value()) {
        sl::ViewportHandle afr_viewport_handle{afr_viewport_id()};
        original_fn(afr_viewport_handle, options);
    }
    return original_fn(viewport, options);
}
#endif

sl::Result UpscalerAfrNvidiaModule::on_dlssSetOptions(const sl::ViewportHandle& viewport, const sl::DLSSOptions& options)
{
    static auto instance    = UpscalerAfrNvidiaModule::Get();
    static auto original_fn = instance->m_dlss_set_options_hook->get_original<decltype(UpscalerAfrNvidiaModule::on_dlssSetOptions)>();
    if(options.mode == sl::DLSSMode::eOff) {
        spdlog::debug("slDLSSSetOptions eOff suppressed for viewport {:x}", (UINT)viewport);
        return sl::Result::eOk;
    }
    const uint64_t new_wh = (uint64_t(options.outputWidth) << 32) | uint64_t(options.outputHeight);
    const bool     needs_reset = s_dlss_options_reset.exchange(false);
    const uint64_t old_wh     = s_dlss_stable_wh.exchange(new_wh);
    if(!needs_reset && old_wh == new_wh) {
        return sl::Result::eOk;
    }
    spdlog::info("slDLSSSetOptions APPLYING viewport {:x} mode {} output {}x{}", (UINT)viewport, (int)options.mode, options.outputWidth, options.outputHeight);

    // The game reconfigures DLSS to a different output resolution on every flat<->VR transition (menus,
    // terminals, Star Map). Destroying and recreating DLSS features on those transitions leaks ~2 GB per
    // open/close cycle until VRAM overcommits, no matter how the free is issued (same-handle sync 27.07-
    // 02.08.2026, same-handle deferred 25.07.2026, fresh-handle retire+redirect 02.08.2026): sl.dlss's
    // slFreeResources discards the NVSDK_NGX_D3D12_ReleaseFeature result and reports eOk while the
    // feature's VRAM is never returned, and sl.common's per-viewport resource clones (idToResourceMap)
    // are only ever recycled on a re-tag of the same viewport id - slFreeResources never reaches
    // sl.common, so clones of abandoned viewport ids stay resident until game shutdown. The only scheme
    // that stays flat (proven by 20 leak-free minutes between transitions in the 02.08 session) is to
    // never destroy anything: cache one shared viewport pair per (resolution, mode), configure it once,
    // and just switch the published pair on later transitions.
    if(old_wh != new_wh && instance->m_free_stale_on_reschange->value()) {
        uint64_t pair;
        bool     created = false;
        {
            std::scoped_lock lock{s_viewport_cache_mutex};
            auto it = std::find_if(s_viewport_pair_cache.begin(), s_viewport_pair_cache.end(),
                                   [&](const CachedViewportPair& e) { return e.wh == new_wh && e.mode == options.mode; });
            if(it != s_viewport_pair_cache.end()) {
                pair = it->pair;
            } else {
                if(old_wh == 0) {
                    pair = s_shared_viewports.load(std::memory_order_relaxed);
                } else {
                    ++s_viewport_generation;
                    const uint32_t stable_id = kRotatedViewportBase + s_viewport_generation * 2;
                    pair = (uint64_t(stable_id + 1) << 32) | uint64_t(stable_id);
                }
                s_viewport_pair_cache.push_back({new_wh, options.mode, pair});
                created = true;
                if(s_viewport_pair_cache.size() > 8) {
                    spdlog::warn("DLSS viewport pair cache unusually large ({} entries)", s_viewport_pair_cache.size());
                }
            }
        }
        if(created) {
            if(instance->m_enabled->value()) {
                original_fn(sl::ViewportHandle{(uint32_t)(pair >> 32)}, options);
            }
            const auto stable_result = original_fn(sl::ViewportHandle{(uint32_t)pair}, options);
            s_shared_viewports.store(pair);
            spdlog::info("Created DLSS shared viewport pair {:x}/{:x} for {}x{} mode {}", (uint32_t)pair, (uint32_t)(pair >> 32), options.outputWidth,
                         options.outputHeight, (int)options.mode);
            return stable_result;
        }
        s_shared_viewports.store(pair);
        spdlog::info("Switched to cached DLSS shared viewport pair {:x}/{:x} for {}x{} mode {} (no reconfigure)", (uint32_t)pair, (uint32_t)(pair >> 32),
                     options.outputWidth, options.outputHeight, (int)options.mode);
        return sl::Result::eOk;
    }

    if(instance->m_enabled->value()) {
        sl::ViewportHandle afr_viewport_handle{afr_viewport_id()};
        original_fn(afr_viewport_handle, options);
    }
    sl::ViewportHandle stable_viewport{stable_viewport_id()};
    return original_fn(stable_viewport, options);
}

sl::Result UpscalerAfrNvidiaModule::on_slFreeResources(sl::Feature feature, const sl::ViewportHandle& viewport)
{
    static auto instance    = UpscalerAfrNvidiaModule::Get();
    static auto original_fn = instance->m_free_resources_hook->get_original<decltype(UpscalerAfrNvidiaModule::on_slFreeResources)>();
    if(supported_afr_feature(feature)) {
        spdlog::info("slFreeResources suppressed for feature {:x} viewport {:x}", (UINT)feature, (UINT)viewport);
        return sl::Result::eOk;
    }
    return original_fn(feature, viewport);
}

sl::Result UpscalerAfrNvidiaModule::on_slAllocateResources(sl::CommandBuffer* cmdBuffer, sl::Feature feature, const sl::ViewportHandle& viewport)
{
    static auto instance    = UpscalerAfrNvidiaModule::Get();
    static auto original_fn = instance->m_allocate_resources_hook->get_original<decltype(UpscalerAfrNvidiaModule::on_slAllocateResources)>();
    if(supported_afr_feature(feature) && instance->m_enabled->value()) {
        sl::ViewportHandle afr_viewport_handle{afr_viewport_id()};
        original_fn(cmdBuffer, feature, afr_viewport_handle);
    }
    spdlog::info("slAllocateResources for feature {:x} viewport {:x}", (UINT)feature, (UINT)viewport);
    if(supported_afr_feature(feature)) {
        sl::ViewportHandle stable_viewport{stable_viewport_id()};
        return original_fn(cmdBuffer, feature, stable_viewport);
    }
    return original_fn(cmdBuffer, feature, viewport);
}
#endif