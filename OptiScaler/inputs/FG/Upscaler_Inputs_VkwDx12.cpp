#include "pch.h"
#include "Upscaler_Inputs_VkwDx12.h"
#include "Upscaler_Inputs.h"

#include <State.h>
#include <Config.h>

#include <framegen/IFGFeature_Dx12.h>
#include <with_dx12/vk_with_dx12_sc.h>
#include <shaders/depth_transfer/DT_Vk.h>
#include <shaders/resource_copy/RC_Vk.h>

static VkDevice _device = VK_NULL_HANDLE;
static VkwDx12Image _velocity[BUFFER_COUNT] {};
static VkwDx12Image _depth[BUFFER_COUNT] {};
static std::unique_ptr<ResourceCopy_Vk> _velocityCopy;
static std::unique_ptr<DepthTransfer_Vk> _depthTransfer;
static VkFormat _depthTransferFormat = VK_FORMAT_UNDEFINED;

// Copies the input to the frame's shared image, depth as R32 float. Upscalers read their inputs as shader resources.
static ID3D12Resource* CopyInput(VkCommandBuffer cmdBuffer, NVSDK_NGX_Parameter* parameters, const char* name,
                                 bool depth, VkwDx12Image& target, VkPhysicalDevice physicalDevice,
                                 ID3D12Device* dx12Device)
{
    NVSDK_NGX_Resource_VK* ngxResource = nullptr;

    if (parameters->Get(name, (void**) &ngxResource) != NVSDK_NGX_Result_Success || ngxResource == nullptr)
        return nullptr;

    auto& info = ngxResource->Resource.ImageViewInfo;

    if (info.Image == VK_NULL_HANDLE || info.ImageView == VK_NULL_HANDLE || info.Width == 0 || info.Height == 0)
        return nullptr;

    // The copy shader converts other motion vector formats
    auto format = VK_FORMAT_R16G16_SFLOAT;

    if (depth)
        format = VK_FORMAT_R32_SFLOAT;
    else if (info.Format == VK_FORMAT_R32G32_SFLOAT || info.Format == VK_FORMAT_R16G16B16A16_SFLOAT)
        format = info.Format;

    if (target.image == VK_NULL_HANDLE || target.width != info.Width || target.height != info.Height ||
        target.format != format)
    {
        // D3D12 is done with the slot, so is the Vulkan copy it waited for
        VkwDx12SC::DestroySharedImage(_device, target);

        if (!VkwDx12SC::CreateSharedImage(_device, physicalDevice, dx12Device, target, format, info.Width, info.Height,
                                          VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT))
        {
            return nullptr;
        }
    }

    if (depth && (_depthTransfer == nullptr || _depthTransferFormat != info.Format))
    {
        _depthTransfer = std::make_unique<DepthTransfer_Vk>("FG DepthTransfer", _device, physicalDevice, info.Format);
        _depthTransferFormat = info.Format;
    }

    if (!depth && _velocityCopy == nullptr)
        _velocityCopy = std::make_unique<ResourceCopy_Vk>("FG VelocityCopy", _device, physicalDevice);

    VkMemoryBarrier inputBarrier { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    inputBarrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    inputBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    VkImageMemoryBarrier barrier { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = target.image;
    barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;

    vkCmdPipelineBarrier(cmdBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                         &inputBarrier, 0, nullptr, 1, &barrier);

    VkExtent2D extent { info.Width, info.Height };
    bool copied;

    if (depth)
        copied = _depthTransfer->CanRender() &&
                 _depthTransfer->Dispatch(_device, cmdBuffer, info.ImageView, target.view, extent);
    else
        copied = _velocityCopy->CanRender() &&
                 _velocityCopy->Dispatch(_device, cmdBuffer, info.ImageView, target.view, extent);

    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;

    vkCmdPipelineBarrier(cmdBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &barrier);

    if (!copied)
    {
        LOG_ERROR("Can't copy {} for FG", name);
        return nullptr;
    }

    target.layout = VK_IMAGE_LAYOUT_GENERAL;
    return target.resource;
}

void UpscalerInputsVkwDx12::UpscaleStart(VkCommandBuffer cmdBuffer, NVSDK_NGX_Parameter* parameters,
                                         IFeature_Vk* feature)
{
    auto& state = State::Instance();
    auto fg = state.currentFG;

    if (fg == nullptr || state.activeFgInput != FGInput::Upscaler ||
        state.swapchainInteropApi != SwapchainInteropApi::VkwDx12)
    {
        return;
    }

    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    ID3D12Device* dx12Device = nullptr;

    if (!VkwDx12SC::InteropDevices(device, physicalDevice, dx12Device))
        return;

    // A new device, the old one's objects went with it
    if (device != _device)
    {
        for (int i = 0; i < BUFFER_COUNT; i++)
        {
            for (auto image : { &_velocity[i], &_depth[i] })
            {
                if (image->resource != nullptr)
                    image->resource->Release();

                *image = {};
            }
        }

        (void) _velocityCopy.release();
        (void) _depthTransfer.release();
        _device = device;
    }

    auto constants = UpscalerInputs::GetFGConstants(feature);
    fg->EvaluateState(dx12Device, constants);

    // Present dispatches the previous frame's data
    if (fg->Mutex.getOwner() == 2)
    {
        LOG_TRACE("Waiting for present!");
        fg->Mutex.lock(4);
        fg->Mutex.unlockThis(4);
    }

    fg->StartNewFrame();

    int reset = 0;
    float mvScaleX = 0.0f;
    float mvScaleY = 0.0f;
    float jitterX = 0.0f;
    float jitterY = 0.0f;

    parameters->Get(NVSDK_NGX_Parameter_Reset, &reset);
    parameters->Get(NVSDK_NGX_Parameter_MV_Scale_X, &mvScaleX);
    parameters->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &mvScaleY);
    parameters->Get(NVSDK_NGX_Parameter_Jitter_Offset_X, &jitterX);
    parameters->Get(NVSDK_NGX_Parameter_Jitter_Offset_Y, &jitterY);

    auto camera = UpscalerInputs::GetCameraValues(*parameters, feature);
    auto aspectRatio = (float) feature->DisplayWidth() / (float) feature->DisplayHeight();
    fg->SetCameraValues(camera.cameraNear, camera.cameraFar, camera.cameraVFov, aspectRatio, camera.meterFactor);
    fg->SetFrameTimeDelta(state.lastFGFrameTime);
    fg->SetMVScale(mvScaleX, mvScaleY);
    fg->SetJitter(jitterX, jitterY);
    fg->SetReset(reset);
    fg->SetInterpolationRect(feature->DisplayWidth(), feature->DisplayHeight());

    if (state.isShuttingDown || !fg->IsActive() || !Config::Instance()->IsFGEnabled() ||
        state.currentSwapchain == nullptr)
    {
        return;
    }

    const auto frameIndex = fg->GetIndex();

    if (!VkwDx12SC::WaitForInputSlot(frameIndex))
        return;

    auto velocity = CopyInput(cmdBuffer, parameters, NVSDK_NGX_Parameter_MotionVectors, false, _velocity[frameIndex],
                              physicalDevice, dx12Device);
    auto depth = CopyInput(cmdBuffer, parameters, NVSDK_NGX_Parameter_Depth, true, _depth[frameIndex], physicalDevice,
                           dx12Device);

    // The FG copies them at present, after D3D12 waited for the Vulkan frame
    auto cmdList = fg->GetUICommandList();

    if (velocity != nullptr)
    {
        Dx12Resource setResource {};
        setResource.type = FG_ResourceType::Velocity;
        setResource.cmdList = cmdList;
        setResource.resource = velocity;
        setResource.state = D3D12_RESOURCE_STATE_COMMON;
        setResource.validity = FG_ResourceValidity::ValidNow;
        setResource.width = feature->LowResMV() ? feature->RenderWidth() : feature->TargetWidth();
        setResource.height = feature->LowResMV() ? feature->RenderHeight() : feature->TargetHeight();

        fg->SetResource(&setResource);
    }

    if (depth != nullptr)
    {
        Dx12Resource setResource {};
        setResource.type = FG_ResourceType::Depth;
        setResource.cmdList = cmdList;
        setResource.resource = depth;
        setResource.state = D3D12_RESOURCE_STATE_COMMON;
        setResource.validity = FG_ResourceValidity::ValidNow;
        setResource.width = feature->RenderWidth();
        setResource.height = feature->RenderHeight();

        fg->SetResource(&setResource);
    }

    VkwDx12SC::InputSlotUsed(frameIndex);

    LOG_DEBUG("(FG VkwDx12) inputs copied to slot {}, frame: {}", frameIndex, fg->FrameCount());
}

void UpscalerInputsVkwDx12::Shutdown()
{
    if (_device == VK_NULL_HANDLE)
        return;

    vkDeviceWaitIdle(_device);

    for (int i = 0; i < BUFFER_COUNT; i++)
    {
        VkwDx12SC::DestroySharedImage(_device, _velocity[i]);
        VkwDx12SC::DestroySharedImage(_device, _depth[i]);
    }

    _velocityCopy.reset();
    _depthTransfer.reset();
    _depthTransferFormat = VK_FORMAT_UNDEFINED;
    _device = VK_NULL_HANDLE;
}
