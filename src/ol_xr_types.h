#pragma once
// ol_xr_types.h - hand-coded OpenXR 1.0 ABI (XR_KHR_D3D11_enable) for Outlast (x64).
// Lifted from the Dishonored probe's x86-corrected header. It is ALSO x64-correct:
// handles as uint64_t are bit-identical to the 64-bit pointer handles OpenXR uses on x64,
// and __stdcall is a no-op in the x64 calling convention. Do not "tidy".
// No OpenXR SDK dependency; the x64 openxr_loader.dll is loaded at runtime.
//
// Outlast is D3D9, but OpenXR has no D3D9 binding, so a D3D11 device is created and bound
// instead, and D3D9 bridges to it via CPU readback. The XR types below are D3D11-bound.

#include <Windows.h>
#include <d3d11.h>
#include <cstdint>

// OpenXR uses __stdcall (XRAPI_PTR) on Win32; on x64 it is ignored. Harmless, kept for parity.
#define OL_XRAPI __stdcall

namespace OLXR
{
// ---- constants / enum values -------------------------------------------------------------
constexpr int32_t XR_SUCCESS_VALUE = 0;
constexpr int32_t XR_EVENT_UNAVAILABLE_VALUE = 4;
constexpr int32_t XR_ERROR_FORM_FACTOR_UNAVAILABLE_VALUE = -35;
constexpr int32_t XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED_VALUE = -41;
constexpr int32_t XR_ERROR_RUNTIME_UNAVAILABLE_VALUE = -51;
constexpr uint32_t XR_TYPE_API_LAYER_PROPERTIES_VALUE = 1;
constexpr uint32_t XR_TYPE_EXTENSION_PROPERTIES_VALUE = 2;
constexpr uint32_t XR_MAX_API_LAYER_NAME_SIZE_VALUE = 256;
constexpr uint32_t XR_MAX_API_LAYER_DESCRIPTION_SIZE_VALUE = 256;
constexpr uint32_t XR_TYPE_INSTANCE_CREATE_INFO_VALUE = 3;
constexpr uint32_t XR_TYPE_SYSTEM_GET_INFO_VALUE = 4;
constexpr uint32_t XR_TYPE_VIEW_LOCATE_INFO_VALUE = 6;
constexpr uint32_t XR_TYPE_VIEW_VALUE = 7;
constexpr uint32_t XR_TYPE_SESSION_CREATE_INFO_VALUE = 8;
constexpr uint32_t XR_TYPE_SWAPCHAIN_CREATE_INFO_VALUE = 9;
constexpr uint32_t XR_TYPE_SESSION_BEGIN_INFO_VALUE = 10;
constexpr uint32_t XR_TYPE_VIEW_STATE_VALUE = 11;
constexpr uint32_t XR_TYPE_FRAME_END_INFO_VALUE = 12;
constexpr uint32_t XR_TYPE_EVENT_DATA_BUFFER_VALUE = 16;
constexpr uint32_t XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED_VALUE = 18;
constexpr uint32_t XR_TYPE_INSTANCE_PROPERTIES_VALUE = 32;
constexpr uint32_t XR_TYPE_FRAME_WAIT_INFO_VALUE = 33;
constexpr uint32_t XR_TYPE_COMPOSITION_LAYER_PROJECTION_VALUE = 35;
constexpr uint32_t XR_TYPE_REFERENCE_SPACE_CREATE_INFO_VALUE = 37;
constexpr uint32_t XR_TYPE_VIEW_CONFIGURATION_VIEW_VALUE = 41;
constexpr uint32_t XR_TYPE_FRAME_STATE_VALUE = 44;
constexpr uint32_t XR_TYPE_FRAME_BEGIN_INFO_VALUE = 46;
constexpr uint32_t XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW_VALUE = 48;
constexpr uint32_t XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO_VALUE = 55;
constexpr uint32_t XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO_VALUE = 56;
constexpr uint32_t XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO_VALUE = 57;
constexpr uint32_t XR_TYPE_GRAPHICS_BINDING_D3D11_KHR_VALUE = 1000027000;
constexpr uint32_t XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR_VALUE = 1000027001;
constexpr uint32_t XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR_VALUE = 1000027002;
constexpr uint32_t XR_MAX_APPLICATION_NAME_SIZE_VALUE = 128;
constexpr uint32_t XR_MAX_ENGINE_NAME_SIZE_VALUE = 128;
constexpr uint32_t XR_MAX_EXTENSION_NAME_SIZE_VALUE = 128;
constexpr uint32_t XR_MAX_RUNTIME_NAME_SIZE_VALUE = 128;
constexpr uint32_t XR_MAX_EVENT_DATA_SIZE_VALUE = 4000;
constexpr int32_t XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY_VALUE = 1;
constexpr int32_t XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO_VALUE = 2;
constexpr int32_t XR_REFERENCE_SPACE_TYPE_VIEW_VALUE = 1;
constexpr int32_t XR_REFERENCE_SPACE_TYPE_LOCAL_VALUE = 2;
constexpr int32_t XR_ENVIRONMENT_BLEND_MODE_OPAQUE_VALUE = 1;
constexpr int32_t XR_EYE_VISIBILITY_BOTH_VALUE = 0;
constexpr int32_t XR_EYE_VISIBILITY_LEFT_VALUE = 1;
constexpr int32_t XR_EYE_VISIBILITY_RIGHT_VALUE = 2;
constexpr uint32_t XR_TYPE_COMPOSITION_LAYER_QUAD_VALUE = 36;
constexpr int32_t XR_SESSION_STATE_READY_VALUE = 2;
constexpr int32_t XR_SESSION_STATE_SYNCHRONIZED_VALUE = 3;
constexpr int32_t XR_SESSION_STATE_VISIBLE_VALUE = 4;
constexpr int32_t XR_SESSION_STATE_FOCUSED_VALUE = 5;
constexpr int32_t XR_SESSION_STATE_STOPPING_VALUE = 6;
constexpr int32_t XR_SESSION_STATE_LOSS_PENDING_VALUE = 7;
constexpr int32_t XR_SESSION_STATE_EXITING_VALUE = 8;
constexpr int64_t XR_INFINITE_DURATION_VALUE = 0x7fffffffffffffffLL;
constexpr uint64_t XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT_VALUE = 0x00000001;
constexpr uint64_t XR_SWAPCHAIN_USAGE_SAMPLED_BIT_VALUE = 0x00000020;
constexpr uint64_t XR_VIEW_STATE_ORIENTATION_VALID_BIT_VALUE = 0x00000001;
constexpr uint64_t XR_VIEW_STATE_POSITION_VALID_BIT_VALUE = 0x00000002;
constexpr int32_t XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT = 0x00000002;

// ---- [XRINPUT] action-set ABI (motion controls step 2) ------------------------------------
// Struct types and enums straight from the OpenXR 1.0 spec. Sizes matter more than names here:
// XrActionCreateInfo carries fixed-size name arrays INLINE, so a wrong array length silently
// shifts every field after it.
constexpr uint32_t XR_TYPE_ACTION_STATE_BOOLEAN_VALUE = 23;
constexpr uint32_t XR_TYPE_ACTION_STATE_FLOAT_VALUE = 24;
constexpr uint32_t XR_TYPE_ACTION_STATE_VECTOR2F_VALUE = 25;
constexpr uint32_t XR_TYPE_ACTION_STATE_POSE_VALUE = 27;
constexpr uint32_t XR_TYPE_ACTION_SET_CREATE_INFO_VALUE = 28;
constexpr uint32_t XR_TYPE_ACTION_CREATE_INFO_VALUE = 29;
constexpr uint32_t XR_TYPE_ACTION_SPACE_CREATE_INFO_VALUE = 38;
constexpr uint32_t XR_TYPE_SPACE_LOCATION_VALUE = 42;
constexpr uint32_t XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING_VALUE = 51;
constexpr uint32_t XR_TYPE_ACTION_STATE_GET_INFO_VALUE = 58;
constexpr uint32_t XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO_VALUE = 60;
constexpr uint32_t XR_TYPE_ACTIONS_SYNC_INFO_VALUE = 61;
constexpr uint32_t XR_MAX_ACTION_SET_NAME_SIZE_VALUE = 64;
constexpr uint32_t XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE_VALUE = 128;
constexpr uint32_t XR_MAX_ACTION_NAME_SIZE_VALUE = 64;
constexpr uint32_t XR_MAX_LOCALIZED_ACTION_NAME_SIZE_VALUE = 128;
constexpr int32_t XR_ACTION_TYPE_BOOLEAN_INPUT_VALUE = 1;
constexpr int32_t XR_ACTION_TYPE_FLOAT_INPUT_VALUE = 2;
constexpr int32_t XR_ACTION_TYPE_VECTOR2F_INPUT_VALUE = 3;
constexpr int32_t XR_ACTION_TYPE_POSE_INPUT_VALUE = 4;
constexpr uint64_t XR_SPACE_LOCATION_ORIENTATION_VALID_BIT_VALUE = 0x00000001;
constexpr uint64_t XR_SPACE_LOCATION_POSITION_VALID_BIT_VALUE = 0x00000002;
constexpr uint64_t XR_NULL_PATH_VALUE = 0;

constexpr char kD3D11ExtensionName[] = "XR_KHR_D3D11_enable";

// ---- handle / scalar aliases -------------------------------------------------------------
using XrVersion = uint64_t;
using XrFlags64 = uint64_t;
using XrResult = int32_t;
using XrStructureType = uint32_t;
using XrBool32 = uint32_t;
using XrTime = int64_t;
using XrDuration = int64_t;
using XrFormFactor = int32_t;
using XrEnvironmentBlendMode = int32_t;
using XrReferenceSpaceType = int32_t;
using XrSessionState = int32_t;
using XrViewStateFlags = XrFlags64;
using XrInstanceCreateFlags = XrFlags64;
using XrSessionCreateFlags = XrFlags64;
using XrSwapchainCreateFlags = XrFlags64;
using XrSwapchainUsageFlags = XrFlags64;
// OpenXR handles: 64-bit everywhere (pointer-sized on x64).
using XrInstance = uint64_t;
using XrSession = uint64_t;
using XrSpace = uint64_t;
using XrSwapchain = uint64_t;
using XrSystemId = uint64_t;
using XrViewConfigurationType = int32_t;
using XrEyeVisibility = int32_t;
using PFN_xrVoidFunction = void (OL_XRAPI *)();
// [XRINPUT] handles and scalars for the action set
using XrActionSet = uint64_t;
using XrAction = uint64_t;
using XrPath = uint64_t;
using XrActionType = int32_t;
using XrSpaceLocationFlags = XrFlags64;

// ---- structures (ABI-exact; compiler lays out per-arch) -----------------------------------
struct XrExtensionProperties { XrStructureType type; void* next; char extensionName[XR_MAX_EXTENSION_NAME_SIZE_VALUE]; uint32_t extensionVersion; };
struct XrApiLayerProperties { XrStructureType type; void* next; char layerName[XR_MAX_API_LAYER_NAME_SIZE_VALUE]; XrVersion specVersion; uint32_t layerVersion; char description[XR_MAX_API_LAYER_DESCRIPTION_SIZE_VALUE]; };
struct XrApplicationInfo { char applicationName[XR_MAX_APPLICATION_NAME_SIZE_VALUE]; uint32_t applicationVersion; char engineName[XR_MAX_ENGINE_NAME_SIZE_VALUE]; uint32_t engineVersion; XrVersion apiVersion; };
struct XrInstanceCreateInfo { XrStructureType type; const void* next; XrInstanceCreateFlags createFlags; XrApplicationInfo applicationInfo; uint32_t enabledApiLayerCount; const char* const* enabledApiLayerNames; uint32_t enabledExtensionCount; const char* const* enabledExtensionNames; };
struct XrInstanceProperties { XrStructureType type; void* next; XrVersion runtimeVersion; char runtimeName[XR_MAX_RUNTIME_NAME_SIZE_VALUE]; };
struct XrSystemGetInfo { XrStructureType type; const void* next; XrFormFactor formFactor; };
struct XrVector3f { float x; float y; float z; };
struct XrQuaternionf { float x; float y; float z; float w; };
struct XrPosef { XrQuaternionf orientation; XrVector3f position; };
struct XrReferenceSpaceCreateInfo { XrStructureType type; const void* next; XrReferenceSpaceType referenceSpaceType; XrPosef poseInReferenceSpace; };
struct XrViewConfigurationView { XrStructureType type; void* next; uint32_t recommendedImageRectWidth; uint32_t maxImageRectWidth; uint32_t recommendedImageRectHeight; uint32_t maxImageRectHeight; uint32_t recommendedSwapchainSampleCount; uint32_t maxSwapchainSampleCount; };
struct XrGraphicsRequirementsD3D11KHR { XrStructureType type; void* next; LUID adapterLuid; D3D_FEATURE_LEVEL minFeatureLevel; };
struct XrGraphicsBindingD3D11KHR { XrStructureType type; const void* next; ID3D11Device* device; };
struct XrSessionCreateInfo { XrStructureType type; const void* next; XrSessionCreateFlags createFlags; XrSystemId systemId; };
struct XrSwapchainCreateInfo { XrStructureType type; const void* next; XrSwapchainCreateFlags createFlags; XrSwapchainUsageFlags usageFlags; int64_t format; uint32_t sampleCount; uint32_t width; uint32_t height; uint32_t faceCount; uint32_t arraySize; uint32_t mipCount; };
struct XrSwapchainImageBaseHeader { XrStructureType type; void* next; };
struct XrSwapchainImageD3D11KHR { XrStructureType type; void* next; ID3D11Texture2D* texture; };
struct XrEventDataBuffer { XrStructureType type; const void* next; uint8_t varying[XR_MAX_EVENT_DATA_SIZE_VALUE]; };
struct XrEventDataSessionStateChanged { XrStructureType type; const void* next; XrSession session; XrSessionState state; XrTime time; };
struct XrSessionBeginInfo { XrStructureType type; const void* next; XrViewConfigurationType primaryViewConfigurationType; };
struct XrFrameWaitInfo { XrStructureType type; const void* next; };
struct XrFrameState { XrStructureType type; void* next; XrTime predictedDisplayTime; XrDuration predictedDisplayPeriod; XrBool32 shouldRender; };
struct XrFrameBeginInfo { XrStructureType type; const void* next; };
struct XrCompositionLayerBaseHeader { XrStructureType type; const void* next; XrFlags64 layerFlags; XrSpace space; };
struct XrFrameEndInfo { XrStructureType type; const void* next; XrTime displayTime; XrEnvironmentBlendMode environmentBlendMode; uint32_t layerCount; const XrCompositionLayerBaseHeader* const* layers; };
struct XrSwapchainImageAcquireInfo { XrStructureType type; const void* next; };
struct XrSwapchainImageWaitInfo { XrStructureType type; const void* next; XrDuration timeout; };
struct XrSwapchainImageReleaseInfo { XrStructureType type; const void* next; };
struct XrViewLocateInfo { XrStructureType type; const void* next; XrViewConfigurationType viewConfigurationType; XrTime displayTime; XrSpace space; };
struct XrViewState { XrStructureType type; void* next; XrViewStateFlags viewStateFlags; };
struct XrFovf { float angleLeft; float angleRight; float angleUp; float angleDown; };
struct XrView { XrStructureType type; void* next; XrPosef pose; XrFovf fov; };
struct XrOffset2Di { int32_t x; int32_t y; };
struct XrExtent2Di { int32_t width; int32_t height; };
struct XrRect2Di { XrOffset2Di offset; XrExtent2Di extent; };
struct XrSwapchainSubImage { XrSwapchain swapchain; XrRect2Di imageRect; uint32_t imageArrayIndex; };
struct XrCompositionLayerProjectionView { XrStructureType type; const void* next; XrPosef pose; XrFovf fov; XrSwapchainSubImage subImage; };
struct XrCompositionLayerProjection { XrStructureType type; const void* next; XrFlags64 layerFlags; XrSpace space; uint32_t viewCount; const XrCompositionLayerProjectionView* views; };
struct XrExtent2Df { float width; float height; };
struct XrCompositionLayerQuad { XrStructureType type; const void* next; XrFlags64 layerFlags; XrSpace space; XrEyeVisibility eyeVisibility; XrSwapchainSubImage subImage; XrPosef pose; XrExtent2Df size; };

// ---- [XRINPUT] action structures ----------------------------------------------------------
// The name arrays are INLINE fixed-size, not pointers - get a length wrong and every field
// after it shifts. Sizes are the spec's XR_MAX_* constants above.
struct XrVector2f { float x; float y; };
struct XrActionSetCreateInfo { XrStructureType type; const void* next; char actionSetName[XR_MAX_ACTION_SET_NAME_SIZE_VALUE]; char localizedActionSetName[XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE_VALUE]; uint32_t priority; };
struct XrActionCreateInfo { XrStructureType type; const void* next; char actionName[XR_MAX_ACTION_NAME_SIZE_VALUE]; XrActionType actionType; uint32_t countSubactionPaths; const XrPath* subactionPaths; char localizedActionName[XR_MAX_LOCALIZED_ACTION_NAME_SIZE_VALUE]; };
struct XrActionSuggestedBinding { XrAction action; XrPath binding; };
struct XrInteractionProfileSuggestedBinding { XrStructureType type; const void* next; XrPath interactionProfile; uint32_t countSuggestedBindings; const XrActionSuggestedBinding* suggestedBindings; };
struct XrSessionActionSetsAttachInfo { XrStructureType type; const void* next; uint32_t countActionSets; const XrActionSet* actionSets; };
struct XrActiveActionSet { XrActionSet actionSet; XrPath subactionPath; };
struct XrActionsSyncInfo { XrStructureType type; const void* next; uint32_t countActiveActionSets; const XrActiveActionSet* activeActionSets; };
struct XrActionStateGetInfo { XrStructureType type; const void* next; XrAction action; XrPath subactionPath; };
struct XrActionStateBoolean { XrStructureType type; void* next; XrBool32 currentState; XrBool32 changedSinceLastSync; XrTime lastChangeTime; XrBool32 isActive; };
struct XrActionStateFloat { XrStructureType type; void* next; float currentState; XrBool32 changedSinceLastSync; XrTime lastChangeTime; XrBool32 isActive; };
struct XrActionStateVector2f { XrStructureType type; void* next; XrVector2f currentState; XrBool32 changedSinceLastSync; XrTime lastChangeTime; XrBool32 isActive; };
struct XrActionStatePose { XrStructureType type; void* next; XrBool32 isActive; };
struct XrActionSpaceCreateInfo { XrStructureType type; const void* next; XrAction action; XrPath subactionPath; XrPosef poseInActionSpace; };
struct XrSpaceLocation { XrStructureType type; void* next; XrSpaceLocationFlags locationFlags; XrPosef pose; };

// ---- function-pointer typedefs -----------------------------------------------------------
using PFN_xrGetInstanceProcAddr = XrResult (OL_XRAPI *)(XrInstance, const char*, PFN_xrVoidFunction*);
using PFN_xrEnumerateInstanceExtensionProperties = XrResult (OL_XRAPI *)(const char*, uint32_t, uint32_t*, XrExtensionProperties*);
using PFN_xrCreateInstance = XrResult (OL_XRAPI *)(const XrInstanceCreateInfo*, XrInstance*);
using PFN_xrDestroyInstance = XrResult (OL_XRAPI *)(XrInstance);
using PFN_xrGetInstanceProperties = XrResult (OL_XRAPI *)(XrInstance, XrInstanceProperties*);
using PFN_xrPollEvent = XrResult (OL_XRAPI *)(XrInstance, XrEventDataBuffer*);
using PFN_xrGetSystem = XrResult (OL_XRAPI *)(XrInstance, const XrSystemGetInfo*, XrSystemId*);
using PFN_xrEnumerateViewConfigurationViews = XrResult (OL_XRAPI *)(XrInstance, XrSystemId, XrViewConfigurationType, uint32_t, uint32_t*, XrViewConfigurationView*);
using PFN_xrGetD3D11GraphicsRequirementsKHR = XrResult (OL_XRAPI *)(XrInstance, XrSystemId, XrGraphicsRequirementsD3D11KHR*);
using PFN_xrCreateSession = XrResult (OL_XRAPI *)(XrInstance, const XrSessionCreateInfo*, XrSession*);
using PFN_xrDestroySession = XrResult (OL_XRAPI *)(XrSession);
using PFN_xrEnumerateSwapchainFormats = XrResult (OL_XRAPI *)(XrSession, uint32_t, uint32_t*, int64_t*);
using PFN_xrCreateSwapchain = XrResult (OL_XRAPI *)(XrSession, const XrSwapchainCreateInfo*, XrSwapchain*);
using PFN_xrDestroySwapchain = XrResult (OL_XRAPI *)(XrSwapchain);
using PFN_xrEnumerateSwapchainImages = XrResult (OL_XRAPI *)(XrSwapchain, uint32_t, uint32_t*, XrSwapchainImageBaseHeader*);
using PFN_xrAcquireSwapchainImage = XrResult (OL_XRAPI *)(XrSwapchain, const XrSwapchainImageAcquireInfo*, uint32_t*);
using PFN_xrWaitSwapchainImage = XrResult (OL_XRAPI *)(XrSwapchain, const XrSwapchainImageWaitInfo*);
using PFN_xrReleaseSwapchainImage = XrResult (OL_XRAPI *)(XrSwapchain, const XrSwapchainImageReleaseInfo*);
using PFN_xrBeginSession = XrResult (OL_XRAPI *)(XrSession, const XrSessionBeginInfo*);
using PFN_xrEndSession = XrResult (OL_XRAPI *)(XrSession);
using PFN_xrWaitFrame = XrResult (OL_XRAPI *)(XrSession, const XrFrameWaitInfo*, XrFrameState*);
using PFN_xrBeginFrame = XrResult (OL_XRAPI *)(XrSession, const XrFrameBeginInfo*);
using PFN_xrEndFrame = XrResult (OL_XRAPI *)(XrSession, const XrFrameEndInfo*);
using PFN_xrCreateReferenceSpace = XrResult (OL_XRAPI *)(XrSession, const XrReferenceSpaceCreateInfo*, XrSpace*);
using PFN_xrDestroySpace = XrResult (OL_XRAPI *)(XrSpace);
using PFN_xrLocateViews = XrResult (OL_XRAPI *)(XrSession, const XrViewLocateInfo*, XrViewState*, uint32_t, uint32_t*, XrView*);
// [XRINPUT]
using PFN_xrStringToPath = XrResult (OL_XRAPI *)(XrInstance, const char*, XrPath*);
using PFN_xrCreateActionSet = XrResult (OL_XRAPI *)(XrInstance, const XrActionSetCreateInfo*, XrActionSet*);
using PFN_xrDestroyActionSet = XrResult (OL_XRAPI *)(XrActionSet);
using PFN_xrCreateAction = XrResult (OL_XRAPI *)(XrActionSet, const XrActionCreateInfo*, XrAction*);
using PFN_xrDestroyAction = XrResult (OL_XRAPI *)(XrAction);
using PFN_xrSuggestInteractionProfileBindings = XrResult (OL_XRAPI *)(XrInstance, const XrInteractionProfileSuggestedBinding*);
using PFN_xrAttachSessionActionSets = XrResult (OL_XRAPI *)(XrSession, const XrSessionActionSetsAttachInfo*);
using PFN_xrSyncActions = XrResult (OL_XRAPI *)(XrSession, const XrActionsSyncInfo*);
using PFN_xrGetActionStateBoolean = XrResult (OL_XRAPI *)(XrSession, const XrActionStateGetInfo*, XrActionStateBoolean*);
using PFN_xrGetActionStateFloat = XrResult (OL_XRAPI *)(XrSession, const XrActionStateGetInfo*, XrActionStateFloat*);
using PFN_xrGetActionStateVector2f = XrResult (OL_XRAPI *)(XrSession, const XrActionStateGetInfo*, XrActionStateVector2f*);
using PFN_xrCreateActionSpace = XrResult (OL_XRAPI *)(XrSession, const XrActionSpaceCreateInfo*, XrSpace*);
using PFN_xrLocateSpace = XrResult (OL_XRAPI *)(XrSpace, XrSpace, XrTime, XrSpaceLocation*);

struct Functions
{
    PFN_xrDestroyInstance destroyInstance = nullptr;
    PFN_xrGetInstanceProperties getInstanceProperties = nullptr;
    PFN_xrPollEvent pollEvent = nullptr;
    PFN_xrGetSystem getSystem = nullptr;
    PFN_xrEnumerateViewConfigurationViews enumerateViewConfigurationViews = nullptr;
    PFN_xrGetD3D11GraphicsRequirementsKHR getD3D11GraphicsRequirements = nullptr;
    PFN_xrCreateSession createSession = nullptr;
    PFN_xrDestroySession destroySession = nullptr;
    PFN_xrEnumerateSwapchainFormats enumerateSwapchainFormats = nullptr;
    PFN_xrCreateSwapchain createSwapchain = nullptr;
    PFN_xrDestroySwapchain destroySwapchain = nullptr;
    PFN_xrEnumerateSwapchainImages enumerateSwapchainImages = nullptr;
    PFN_xrAcquireSwapchainImage acquireSwapchainImage = nullptr;
    PFN_xrWaitSwapchainImage waitSwapchainImage = nullptr;
    PFN_xrReleaseSwapchainImage releaseSwapchainImage = nullptr;
    PFN_xrBeginSession beginSession = nullptr;
    PFN_xrEndSession endSession = nullptr;
    PFN_xrWaitFrame waitFrame = nullptr;
    PFN_xrBeginFrame beginFrame = nullptr;
    PFN_xrEndFrame endFrame = nullptr;
    PFN_xrCreateReferenceSpace createReferenceSpace = nullptr;
    PFN_xrDestroySpace destroySpace = nullptr;
    PFN_xrLocateViews locateViews = nullptr;
    // [XRINPUT] resolved only when the action set is built; every one is null-checked at use
    // so a runtime missing any of them degrades to "no motion controls", never a crash.
    PFN_xrStringToPath stringToPath = nullptr;
    PFN_xrCreateActionSet createActionSet = nullptr;
    PFN_xrDestroyActionSet destroyActionSet = nullptr;
    PFN_xrCreateAction createAction = nullptr;
    PFN_xrDestroyAction destroyAction = nullptr;
    PFN_xrSuggestInteractionProfileBindings suggestInteractionProfileBindings = nullptr;
    PFN_xrAttachSessionActionSets attachSessionActionSets = nullptr;
    PFN_xrSyncActions syncActions = nullptr;
    PFN_xrGetActionStateBoolean getActionStateBoolean = nullptr;
    PFN_xrGetActionStateFloat getActionStateFloat = nullptr;
    PFN_xrGetActionStateVector2f getActionStateVector2f = nullptr;
    PFN_xrCreateActionSpace createActionSpace = nullptr;
    PFN_xrLocateSpace locateSpace = nullptr;
};

inline bool XrSucceeded(XrResult r) noexcept { return r >= 0; }
inline XrVersion MakeXrVersion(uint16_t major, uint16_t minor, uint32_t patch) noexcept
{
    return (static_cast<XrVersion>(major) << 48) | (static_cast<XrVersion>(minor) << 32) | static_cast<XrVersion>(patch);
}
inline XrPosef IdentityPose() noexcept { XrPosef p = {}; p.orientation.w = 1.0f; return p; }
}
