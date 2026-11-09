#include "pch.h"
#include "ll_xell.h"

#include <chrono>
#include <magic_enum.hpp>
#include <proxies/XeLL_Proxy.h>
#include <nvapi/fakenvapi/log.h>

void XeLL::xell_sleep(uint32_t frame_id)
{
    // Don't call XeLL when trying to disable XeLL with XeFG active, or when another output does the sleeping
    if (!markers_only && (!forced_mode || is_enabled()))
    {
        LOG_TRACE_LOWLATENCY("Sleeping with frame_id: {}", frame_id);
        o_xellSleep(xell_context, frame_id);
    }
}

void XeLL::add_marker(uint32_t frame_id, xell_latency_marker_type_t marker)
{
    if (markers_only || !forced_mode || is_enabled())
        o_xellAddMarkerData(xell_context, frame_id, marker);
}

bool XeLL::init(IUnknown* pDevice)
{
    if (!pDevice)
    {
        LOG_ERROR("Invalid pointer");
        return false;
    }

    if (!o_xellD3D12CreateContext)
    {
        return false;
    }

    HRESULT hr = pDevice->QueryInterface(__uuidof(ID3D12Device), reinterpret_cast<void**>(&dx12_device));
    if (hr != S_OK)
        return false;

    // Created as OptiScaler's own, it goes straight to the real XeLL. Opti's XeFG gets it too.
    auto result = InputXeLL::D3D12CreateLocalContext(dx12_device, (InputXeLL::xell_input_handle_t*) &xell_context) ==
                  XELL_RESULT_SUCCESS;

    if (!result)
    {
        dx12_device->Release();
        dx12_device = nullptr;
    }

    return result;
}

void XeLL::deinit()
{
    o_xellDestroyContext(xell_context);

    fg_reported = false;

    if (app_queue != nullptr)
    {
        app_queue->Release();
        app_queue = nullptr;
    }

    // Held while the context lives
    if (dx12_device != nullptr)
    {
        dx12_device->Release();
        dx12_device = nullptr;
    }

    LOG_INFO("XeLL deinitialized");
}

void* XeLL::get_tech_context() { return xell_context; }

void XeLL::get_sleep_status(SleepParams* sleep_params)
{
    xell_sleep_params_t xell_sleep_params {};
    auto result = o_xellGetSleepMode(xell_context, &xell_sleep_params) == XELL_RESULT_SUCCESS;

    sleep_params->low_latency_enabled = xell_sleep_params.bLowLatencyMode;
    sleep_params->fullscreen_vrr = true;
    sleep_params->control_panel_vsync_override = false;
}

void XeLL::set_sleep_mode(SleepMode* sleep_mode)
{
    xell_sleep_params_t xell_sleep_params {};

    low_latency_enabled = sleep_mode->low_latency_enabled;

    // Always report XeLL as enabled when XeFG is enabled
    if (forced_mode)
        xell_sleep_params.bLowLatencyMode = true;
    else
        xell_sleep_params.bLowLatencyMode = is_enabled();

    xell_sleep_params.minimumIntervalUs = sleep_mode->minimum_interval_us;
    xell_sleep_params.bLowLatencyBoost = sleep_mode->low_latency_boost;

    // With ForceXeLL we have FG enabled but not actually working
    // but their FPS limit thinks that the FG is working
    if (Config::Instance()->ForceXeLL.value_or_default())
        xell_sleep_params.minimumIntervalUs /= 2;

    if (xell_sleep_params.bLowLatencyMode != last_low_latency_mode ||
        xell_sleep_params.minimumIntervalUs != last_minimum_interval_us ||
        xell_sleep_params.bLowLatencyBoost != last_low_latency_boost)
    {
        auto result = o_xellSetSleepMode(xell_context, &xell_sleep_params) == XELL_RESULT_SUCCESS;

        last_low_latency_mode = xell_sleep_params.bLowLatencyMode;
        last_minimum_interval_us = xell_sleep_params.minimumIntervalUs;
        last_low_latency_boost = xell_sleep_params.bLowLatencyBoost;
    }
}

void XeLL::set_marker(IUnknown* pDevice, const MarkerParams& marker_params)
{
    if (!pDevice)
    {
        LOG_ERROR("Invalid pointer");
        return;
    }

    // XeLL frame ids are uint64_t
    auto frame_id = (uint32_t) marker_params.frame_id;

    switch (marker_params.marker_type)
    {
    case MarkerType::SIMULATION_START:
        simulation_start_last_id = marker_params.frame_id;

        // Call sleep just before simulation start if sleep isn't getting called
        if (sleep_last_id + 10 < simulation_start_last_id)
            xell_sleep(frame_id);

        add_marker(frame_id, XELL_SIMULATION_START);
        break;
    case MarkerType::SIMULATION_END:
        add_marker(frame_id, XELL_SIMULATION_END);
        break;
    case MarkerType::RENDERSUBMIT_START:
        add_marker(frame_id, XELL_RENDERSUBMIT_START);
        break;
    case MarkerType::RENDERSUBMIT_END:
        add_marker(frame_id, XELL_RENDERSUBMIT_END);
        break;
    case MarkerType::PRESENT_START:
        add_marker(frame_id, XELL_PRESENT_START);
        break;
    case MarkerType::PRESENT_END:
        add_marker(frame_id, XELL_PRESENT_END);
        break;
    // case MarkerType::INPUT_SAMPLE:
    //     add_marker(marker_params.frame_id, XELL_INPUT_SAMPLE);
    // break;
    default:
        break;
    }
}

void XeLL::set_async_marker(IUnknown* pCommandQueue, const MarkerParams& marker_params)
{
    // The command queue isn't needed, XeLL inputs without an app queue send these too

    // XeLL frame ids are uint64_t
    auto frame_id = (uint32_t) marker_params.frame_id;

    switch (marker_params.marker_type)
    {
    case MarkerType::OUT_OF_BAND_RENDERSUBMIT_START:
        add_marker(frame_id, (xell_latency_marker_type_t) 80860000);
        break;
    case MarkerType::OUT_OF_BAND_RENDERSUBMIT_END:
        add_marker(frame_id, (xell_latency_marker_type_t) 80860001);
        break;
    default:
        break;
    }
}

void XeLL::sleep(std::optional<uint32_t> frame_id)
{
    if (frame_id.has_value())
    {
        sleep_last_id = frame_id.value();
    }
    else
    {
        // This can either be better than sleeping in XELL_SIMULATION_START
        // or be a total mess if +1 is not correct
        sleep_last_id = simulation_start_last_id + 1;
    }

    if (!fg_reported)
    {
        xell_sleep((uint32_t) sleep_last_id);
        return;
    }

    const auto start = std::chrono::steady_clock::now();
    xell_sleep((uint32_t) sleep_last_id);
    const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();

    constexpr uint32_t reportSleeps = 600;
    constexpr uint32_t reportLogLimit = 16;

    reported_sleep_ms += ms;

    if (ms >= 0.5)
        reported_slept++;

    if (++reported_sleeps == reportSleeps)
    {
        if (reported_sleep_logs < reportLogLimit)
        {
            reported_sleep_logs++;
            LOG_INFO("XeLL for frame generation: slept {:.2f} ms per frame, {} of {} frames",
                     reported_sleep_ms / reportSleeps, reported_slept, reportSleeps);
        }

        reported_sleep_ms = 0.0;
        reported_sleeps = 0;
        reported_slept = 0;
    }
}

void XeLL::report_frame_generation(ID3D12CommandQueue* gameQueue, bool enabled, uint32_t generatedFrames)
{
    auto context = (InputXeLL::xell_input_handle_t) xell_context;

    // XeLL times the GPU with direct command lists on it
    if (gameQueue != nullptr && gameQueue != app_queue && gameQueue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
    {
        LOG_WARN("XeLL can't follow the frame generation on a queue that isn't a direct one");
        return;
    }

    if (gameQueue != app_queue)
    {
        // Goes before the display info, XeLL has nowhere to keep that without a queue
        if (gameQueue != nullptr)
            gameQueue->AddRef();

        auto queueResult = InputXeLL::D3D12SetAppQueue(context, gameQueue);

        if (app_queue != nullptr)
            app_queue->Release();

        app_queue = gameQueue;

        auto displayResult = gameQueue != nullptr ? InputXeLL::SetVsyncOffDisplayInfo(context) : XELL_RESULT_SUCCESS;

        // The other calls follow from these, a libxell without them just never sleeps
        if (queueResult != XELL_RESULT_SUCCESS || displayResult != XELL_RESULT_SUCCESS)
        {
            LOG_WARN("XeLL can't follow the frame generation: app queue {}, display info {}", (int) queueResult,
                     (int) displayResult);
        }
    }

    // The count stays with XeLL while frame generation is off
    if (enabled != fg_reported || (enabled && generatedFrames != 0 && generatedFrames != fg_reported_frames))
    {
        auto result = InputXeLL::SetFgEnabled(context, enabled ? 1 : 0, generatedFrames);

        if (enabled != fg_reported)
        {
            LOG_INFO("XeLL follows the frame generation: enabled {}, {} generated frames, result {}", enabled,
                     generatedFrames, (int) result);
        }

        fg_reported_frames = generatedFrames;
        fg_reported = enabled;
    }
}

void XeLL::report_present(uint64_t frame_id, uint32_t generatedFrames, bool presented)
{
    if (frame_id == 0 || !fg_reported)
        return;

    // XeLL frame ids are uint32_t here too
    auto id = (uint32_t) frame_id;

    if (presented)
    {
        add_marker(id, (xell_latency_marker_type_t) 80860001);
        return;
    }

    InputXeLL::SetGeneratedFramesCount((InputXeLL::xell_input_handle_t) xell_context, id, generatedFrames);
    add_marker(id, (xell_latency_marker_type_t) 80860000);
}

xell_result_t XeLL::xellGetFramesReports(xell_frame_report_t* outdata) const
{
    if (!o_xellGetFramesReports)
        return XELL_RESULT_ERROR_UNKNOWN;

    return o_xellGetFramesReports(xell_context, outdata);
}
