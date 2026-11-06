#include "Upscaling.h"

#include <d3dcompiler.h>

#include "CrossAdapterBridge.h"
#include "DX12SwapChain.h"
#include "DirectXMath.h"

enum class RenderTarget
{
	kFrameBuffer = 0,

	kRefractionNormal = 1,
	
	kMainPreAlpha = 2,
	kMain = 3,
	kMainTemp = 4,

	kSSRRaw = 7,
	kSSRBlurred = 8,
	kSSRBlurredExtra = 9,

	kMainVerticalBlur = 14,
	kMainHorizontalBlur = 15,

	kSSRDirection = 10,
	kSSRMask = 11,

	kUI = 17,
	kUITemp = 18,

	kGbufferNormal = 20,
	kGbufferNormalSwap = 21,
	kGbufferAlbedo = 22,
	kGbufferEmissive = 23,
	kGbufferMaterial = 24, //  Glossiness, Specular, Backlighting, SSS

	kSSAO = 28,

	kTAAAccumulation = 26,
	kTAAAccumulationSwap = 27,

	kMotionVectors = 29,

	kUIDownscaled = 36,
	kUIDownscaledComposite = 37,

	kMainDepthMips = 39,

	kUnkMask = 57,

	kSSAOTemp = 48,
	kSSAOTemp2 = 49,
	kSSAOTemp3 = 50,

	kDiffuseBuffer = 58,
	kSpecularBuffer = 59,

	kDownscaledHDR = 64,
	kDownscaledHDRLuminance2 = 65,
	kDownscaledHDRLuminance3 = 66,
	kDownscaledHDRLuminance4 = 67,
	kDownscaledHDRLuminance5Adaptation = 68,
	kDownscaledHDRLuminance6AdaptationSwap = 69,
	kDownscaledHDRLuminance6 = 70,

	kCount = 101
};

enum class DepthStencilTarget
{
	kMainOtherOther = 0,
	kMainOther = 1,
	kMain = 2,
	kMainCopy = 3,
	kMainCopyCopy = 4,

	kShadowMap = 8,

	kCount = 13
};

ID3D11DeviceChild* CompileShader(const wchar_t* FilePath, const char* ProgramType, const char* Program = "main",
	const D3D_SHADER_MACRO* Defines = nullptr)
{
	auto rendererData = RE::BSGraphics::GetRendererData();
	auto device = reinterpret_cast<ID3D11Device*>(rendererData->device);

	// Compiler setup
	uint32_t flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;

	ID3DBlob* shaderBlob = nullptr;
	ID3DBlob* shaderErrors = nullptr;

	std::string str;
	std::wstring path{ FilePath };
	std::transform(path.begin(), path.end(), std::back_inserter(str), [](wchar_t c) {
		return (char)c;
	});
	if (!std::filesystem::exists(FilePath)) {
		REX::ERROR("Failed to compile shader; {} does not exist", str);
		return nullptr;
	}
	if (FAILED(D3DCompileFromFile(FilePath, Defines, D3D_COMPILE_STANDARD_FILE_INCLUDE, Program, ProgramType, flags, 0, &shaderBlob, &shaderErrors))) {
		REX::WARN("Shader compilation failed:\n\n{}", shaderErrors ? static_cast<char*>(shaderErrors->GetBufferPointer()) : "Unknown error");
		if (shaderErrors) shaderErrors->Release();
		if (shaderBlob) shaderBlob->Release();
		return nullptr;
	}
	if (shaderErrors) {
		REX::DEBUG("Shader logs:\n{}", static_cast<char*>(shaderErrors->GetBufferPointer()));
		shaderErrors->Release();
	}

	ID3D11ComputeShader* regShader;
	DX::ThrowIfFailed(device->CreateComputeShader(shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize(), nullptr, &regShader));
	shaderBlob->Release();
	return regShader;
}

void Upscaling::LoadSettings()
{
	CSimpleIniA ini;
	ini.SetUnicode();
	// Read defaults from MCM Config, then overlay user changes from MCM Settings
	ini.LoadFile("Data\\MCM\\Config\\FrameGeneration\\settings.ini");
	ini.LoadFile("Data\\MCM\\Settings\\FrameGeneration.ini");

	settings.frameGenerationMode = ini.GetBoolValue("Settings", "bFrameGenerationMode", true);
	settings.frameLimitMode = ini.GetBoolValue("Settings", "bFrameLimitMode", true);
	settings.disableInMenus = ini.GetBoolValue("Settings", "bDisableInMenus", true);
	settings.debugLogging = ini.GetBoolValue("Settings", "bEnableDebugLogging", false);
	settings.frameGenType = (int)ini.GetLongValue("Settings", "iFrameGenType", 0);
	// MCM stepper is 0-indexed (0=2x, 1=3x, 2=4x) but we store as numFramesToGenerate (1, 2, 3)
	settings.frameGenFrames = std::clamp((int)ini.GetLongValue("Settings", "iFrameGenFrames", 0) + 1, 1, 3);
	settings.secondaryGPU = ini.GetBoolValue("Settings", "bFrameGenOnSecondaryGPU", false);
	settings.secondaryGPUName = ini.GetValue("Settings", "sSecondaryGPUName", "");
	// bTransportHudLess was the old name of this switch; keep reading it so an existing
	// settings.ini (or a test build someone edited) is not silently reset to the default.
	if (ini.KeyExists("Settings", "bSendHudlessFrame"))
		settings.sendHudlessFrame = ini.GetBoolValue("Settings", "bSendHudlessFrame", true);
	else
		settings.sendHudlessFrame = ini.GetBoolValue("Settings", "bTransportHudLess", true);
	settings.asyncUnpack = ini.GetBoolValue("Settings", "bAsyncUnpack", true);
	settings.earlyTransfer = ini.GetBoolValue("Settings", "bEarlyTransfer", true);
	settings.earlyUnpack = ini.GetBoolValue("Settings", "bEarlyUnpack", true);
	settings.halfResDepthMV = std::clamp(static_cast<int>(ini.GetLongValue("Settings", "iHalfResDepthMV", 1)), 0, 2);

	static bool loggedOnce = false;
	if (!loggedOnce) {
		REX::INFO("[Frame Generation] bFrameGenerationMode: {}", settings.frameGenerationMode);
		REX::INFO("[Frame Generation] bFrameLimitMode: {}", settings.frameLimitMode);
		REX::INFO("[Frame Generation] iFrameGenType: {} (0=FSR3, 1=DLSS-G, 2=XeSS-FG)", settings.frameGenType);
		REX::INFO("[Frame Generation] iFrameGenFrames: {} (1=2x, 2=3x MFG, 3=4x MFG)", settings.frameGenFrames);
		REX::INFO("[Frame Generation] bFrameGenOnSecondaryGPU: {} sSecondaryGPUName: \"{}\"",
			settings.secondaryGPU, settings.secondaryGPUName);
		REX::INFO("[Frame Generation] bSendHudlessFrame: {} (extra UI-free frame; off = less PCIe, HUD smears)",
			settings.sendHudlessFrame);
		REX::INFO("[Frame Generation] bAsyncUnpack: {} (secondary GPU unpacks on a copy queue as each buffer arrives)",
			settings.asyncUnpack);
		REX::INFO("[Frame Generation] bEarlyTransfer: {} (depth/MVs/HUD-less cross during the frame, not at Present)",
			settings.earlyTransfer);
		REX::INFO("[Frame Generation] bEarlyUnpack: {} (secondary GPU reads depth/MVs/HUD-less as they land, not at Present)",
			settings.earlyUnpack);
		REX::INFO("[Frame Generation] iHalfResDepthMV: {} (0=full, 1=half-res depth rebuilt on the display GPU, "
				  "2=half-res depth and motion vectors)",
			settings.halfResDepthMV);
		loggedOnce = true;
	}
}

void Upscaling::ReloadSettingsIfNeeded()
{
	static int frameCounter = 0;
	if (++frameCounter % 60 != 0) return;
	LoadSettings();
}

void Upscaling::PostPostLoad()
{
	highFPSPhysicsFixLoaded = GetModuleHandleA("Data\\F4SE\\Plugins\\HighFPSPhysicsFix.dll") != nullptr;

	if (highFPSPhysicsFixLoaded)
		REX::INFO("[Frame Generation] HighFPSPhysicsFix.dll is loaded");
	else
		REX::INFO("[Frame Generation] HighFPSPhysicsFix.dll is not loaded");

	InstallHooks();
}

void Upscaling::CreateFrameGenerationResources()
{
	REX::INFO("[Frame Generation] Creating resources");
	
	setupBuffers = true;

	auto rendererData = RE::BSGraphics::GetRendererData();
	auto& main = rendererData->renderTargets[(uint)RenderTarget::kMain];
	auto dx12SwapChain = DX12SwapChain::GetSingleton();

	D3D11_TEXTURE2D_DESC mainDesc{};
	reinterpret_cast<ID3D11Texture2D*>(main.texture)->GetDesc(&mainDesc);
	SetupHalfResDepthMV(mainDesc.Width, mainDesc.Height);
	// Latched with the lanes below; a later settings reload must not change it mid-session.
	hudlessActive = settings.sendHudlessFrame;
	const UINT halfWidth = (mainDesc.Width + 1) / 2;
	const UINT halfHeight = (mainDesc.Height + 1) / 2;

	for (int index = 0; index < 2; index++) {
		D3D11_TEXTURE2D_DESC texDesc{};
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};

		reinterpret_cast<ID3D11Texture2D*>(main.texture)->GetDesc(&texDesc);
		reinterpret_cast<ID3D11ShaderResourceView*>(main.srView)->GetDesc(&srvDesc);
		reinterpret_cast<ID3D11RenderTargetView*>(main.rtView)->GetDesc(&rtvDesc);

		texDesc.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;

		uavDesc.Format = texDesc.Format;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
		uavDesc.Texture2D.MipSlice = 0;

		texDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

		// Force RGBA8 for shared buffer — FSR3 requires it
		texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		srvDesc.Format = texDesc.Format;
		rtvDesc.Format = texDesc.Format;
		uavDesc.Format = texDesc.Format;

		HUDLessBufferShared[index] = new Texture2D(texDesc);
		HUDLessBufferShared[index]->CreateSRV(srvDesc);
		HUDLessBufferShared[index]->CreateRTV(rtvDesc);
		HUDLessBufferShared[index]->CreateUAV(uavDesc);

		texDesc.Format = DXGI_FORMAT_R32_FLOAT;
		srvDesc.Format = texDesc.Format;
		rtvDesc.Format = texDesc.Format;
		uavDesc.Format = texDesc.Format;

		// FG-GPU1 iHalfResDepthMV: depth crosses PCIe at half resolution (modes 1 and 2).
		texDesc.Width = halfResMode ? halfWidth : mainDesc.Width;
		texDesc.Height = halfResMode ? halfHeight : mainDesc.Height;

		depthBufferShared[index] = new Texture2D(texDesc);
		depthBufferShared[index]->CreateSRV(srvDesc);
		depthBufferShared[index]->CreateRTV(rtvDesc);
		depthBufferShared[index]->CreateUAV(uavDesc);

		auto& motionVector = rendererData->renderTargets[(uint)RenderTarget::kMotionVectors];
		D3D11_TEXTURE2D_DESC texDescMotionVector{};
		reinterpret_cast<ID3D11Texture2D*>(motionVector.texture)->GetDesc(&texDescMotionVector);

		texDesc.Format = texDescMotionVector.Format;
		srvDesc.Format = texDesc.Format;
		rtvDesc.Format = texDesc.Format;
		uavDesc.Format = texDesc.Format;

		// Motion vectors only at half resolution in mode 2.
		texDesc.Width = halfResMode == 2 ? halfWidth : mainDesc.Width;
		texDesc.Height = halfResMode == 2 ? halfHeight : mainDesc.Height;

		motionVectorBufferShared[index] = new Texture2D(texDesc);
		motionVectorBufferShared[index]->CreateSRV(srvDesc);
		motionVectorBufferShared[index]->CreateRTV(rtvDesc);
		motionVectorBufferShared[index]->CreateUAV(uavDesc);

		// UIColorAndAlpha buffer — R8G8B8A8_UNORM for premultiplied alpha UI extraction
		texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		srvDesc.Format = texDesc.Format;
		rtvDesc.Format = texDesc.Format;
		uavDesc.Format = texDesc.Format;
		texDesc.Width = mainDesc.Width;
		texDesc.Height = mainDesc.Height;

		UIColorAlphaShared[index] = new Texture2D(texDesc);
		UIColorAlphaShared[index]->CreateSRV(srvDesc);
		UIColorAlphaShared[index]->CreateRTV(rtvDesc);
		UIColorAlphaShared[index]->CreateUAV(uavDesc);

		{
			IDXGIResource1* dxgiResource = nullptr;
			DX::ThrowIfFailed(HUDLessBufferShared[index]->resource->QueryInterface(IID_PPV_ARGS(&dxgiResource)));

			if (dx12SwapChain->swapChain) {
				HANDLE sharedHandle = nullptr;
				DX::ThrowIfFailed(dxgiResource->CreateSharedHandle(
					nullptr,
					DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
					nullptr,
					&sharedHandle));

				DX::ThrowIfFailed(dx12SwapChain->GetInteropDevice()->OpenSharedHandle(
					sharedHandle,
					IID_PPV_ARGS(&HUDLessBufferShared12[index])));

				CloseHandle(sharedHandle);
			}
		}

		{
			IDXGIResource1* dxgiResource = nullptr;
			DX::ThrowIfFailed(depthBufferShared[index]->resource->QueryInterface(IID_PPV_ARGS(&dxgiResource)));

			if (dx12SwapChain->swapChain) {
				HANDLE sharedHandle = nullptr;
				DX::ThrowIfFailed(dxgiResource->CreateSharedHandle(
					nullptr,
					DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
					nullptr,
					&sharedHandle));

				DX::ThrowIfFailed(dx12SwapChain->GetInteropDevice()->OpenSharedHandle(
					sharedHandle,
					IID_PPV_ARGS(&depthBufferShared12[index])));

				CloseHandle(sharedHandle);
			}
		}

		{
			IDXGIResource1* dxgiResource = nullptr;
			DX::ThrowIfFailed(motionVectorBufferShared[index]->resource->QueryInterface(IID_PPV_ARGS(&dxgiResource)));

			if (dx12SwapChain->swapChain) {
				HANDLE sharedHandle = nullptr;
				DX::ThrowIfFailed(dxgiResource->CreateSharedHandle(
					nullptr,
					DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
					nullptr,
					&sharedHandle));

				DX::ThrowIfFailed(dx12SwapChain->GetInteropDevice()->OpenSharedHandle(
					sharedHandle,
					IID_PPV_ARGS(&motionVectorBufferShared12[index])));

				CloseHandle(sharedHandle);
			}
		}

		{
			IDXGIResource1* dxgiResource = nullptr;
			DX::ThrowIfFailed(UIColorAlphaShared[index]->resource->QueryInterface(IID_PPV_ARGS(&dxgiResource)));

			if (dx12SwapChain->swapChain) {
				HANDLE sharedHandle = nullptr;
				DX::ThrowIfFailed(dxgiResource->CreateSharedHandle(
					nullptr,
					DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
					nullptr,
					&sharedHandle));

				DX::ThrowIfFailed(dx12SwapChain->GetInteropDevice()->OpenSharedHandle(
					sharedHandle,
					IID_PPV_ARGS(&UIColorAlphaShared12[index])));

				CloseHandle(sharedHandle);
			}
		}
	}

	// Secondary-GPU mode: the resources above were opened on the render GPU. Register them
	// with the bridge and hand the frame-generation backends the copies on the secondary GPU
	// instead (FidelityFX::Present reads the *Shared12 arrays). UIColorAlpha is DLSS-G only.
	if (dx12SwapChain->secondaryGPU && dx12SwapChain->swapChain) {
		auto bridge = CrossAdapterBridge::GetSingleton();
		const auto transport = [bridge](const char* a_name, winrt::com_ptr<ID3D12Resource> (&a_resources)[2],
			CrossAdapterBridge::LaneTiming a_timing) {
			const int lane = bridge->AddLane(a_name, a_resources[0].get(), a_resources[1].get(), true, a_timing);
			if (lane < 0) {
				REX::CRITICAL("[FG-GPU1] cannot transport '{}'; frame generation will read stale data", a_name);
				return lane;
			}
			for (UINT slot = 0; slot < 2; ++slot)
				a_resources[slot].copy_from(bridge->Mirror(lane, slot));
			return lane;
		};
		// Registered in the order the data becomes final: the display GPU starts reading the
		// world-pass lanes while the 3090 is still finishing the frame. Depth first: in
		// iHalfResDepthMV = 1 the display GPU rebuilds it while the other lanes still cross.
		const int depthLane = transport(halfResMode ? "depth (half res)" : "depth", depthBufferShared12,
			CrossAdapterBridge::LaneTiming::kWorld);
		if (halfResMode == 1 && depthLane >= 0) {
			// FSR3 reads the full-resolution depth rebuilt from the half-resolution mirror.
			bridge->SetDepthUpsampleLane(depthLane);
			if (bridge->UpsampledDepth(0) && bridge->UpsampledDepth(1)) {
				for (UINT slot = 0; slot < 2; ++slot)
					depthBufferShared12[slot].copy_from(bridge->UpsampledDepth(slot));
			} else {
				REX::CRITICAL("[FG-GPU1] depth rebuild unavailable; FSR3 gets half-resolution depth at full render size. Set iHalfResDepthMV=0 or 2");
			}
		}
		transport(halfResMode == 2 ? "motion vectors (half res)" : "motion vectors", motionVectorBufferShared12,
			CrossAdapterBridge::LaneTiming::kWorld);
		if (hudlessActive)
			transport("HUD-less colour", HUDLessBufferShared12, CrossAdapterBridge::LaneTiming::kHudless);
	}
	if (dx12SwapChain->secondaryGPU && dx12SwapChain->swapChain && !hudlessActive) {
		// FSR3 treats an empty HUDLessColor as "use the back buffer", so the UI is
		// interpolated with the scene and smears a little. Saves one full frame across PCIe.
		for (auto& resource : HUDLessBufferShared12)
			resource = nullptr;
		REX::INFO("[FG-GPU1] bSendHudlessFrame=0: the extra UI-free frame is not sent (HUD will smear, ~25% less PCIe)");
	}

	copyDepthToSharedBufferCS = (ID3D11ComputeShader*)CompileShader(L"Data\\F4SE\\Plugins\\FrameGeneration\\CopyDepthToSharedBufferCS.hlsl", "cs_5_0");
	generateSharedBuffersCS = (ID3D11ComputeShader*)CompileShader(L"Data\\F4SE\\Plugins\\FrameGeneration\\GenerateSharedBuffersCS.hlsl", "cs_5_0");
	generateUIBufferCS = (ID3D11ComputeShader*)CompileShader(L"Data\\F4SE\\Plugins\\FrameGeneration\\GenerateUIBufferCS.hlsl", "cs_5_0");

	REX::INFO("[FG] Frame generation resources created (HUDLess + Depth + MVec + UIColorAlpha)");
}

void Upscaling::SetupHalfResDepthMV(UINT a_width, UINT a_height)
{
	auto dx12SwapChain = DX12SwapChain::GetSingleton();
	auto bridge = CrossAdapterBridge::GetSingleton();

	// Only worth it when depth and motion vectors cross PCIe. Read once: the shared buffers and
	// lanes are sized for it, so a later settings reload cannot change it.
	halfResMode = (dx12SwapChain->secondaryGPU && dx12SwapChain->swapChain && bridge->IsActive()) ?
	                  settings.halfResDepthMV :
	                  0;
	if (!halfResMode)
		return;

	const char* halfMotion = halfResMode == 2 ? "1" : "0";
	const D3D_SHADER_MACRO postAlpha[] = { { "ALPHA_MASK", "1" }, { "HALF_MV", halfMotion }, { nullptr, nullptr } };
	const D3D_SHADER_MACRO copyOnly[] = { { "ALPHA_MASK", "0" }, { "HALF_MV", halfMotion }, { nullptr, nullptr } };
	constexpr auto kShader = L"Data\\F4SE\\Plugins\\FrameGeneration\\HalfResDepthMVCS.hlsl";
	halfResPostAlphaCS = (ID3D11ComputeShader*)CompileShader(kShader, "cs_5_0", "main", postAlpha);
	halfResCopyCS = (ID3D11ComputeShader*)CompileShader(kShader, "cs_5_0", "main", copyOnly);

	bool ready = halfResPostAlphaCS && halfResCopyCS;
	if (!ready)
		REX::ERROR("[FG-GPU1] HalfResDepthMVCS.hlsl did not compile; sending depth and motion vectors at full resolution");
	if (ready && halfResMode == 1)
		ready = bridge->CreateDepthUpsampler(a_width, a_height, L"Data\\F4SE\\Plugins\\FrameGeneration\\DepthFromHalfResCS.hlsl");

	if (!ready) {
		if (halfResPostAlphaCS)
			halfResPostAlphaCS->Release();
		if (halfResCopyCS)
			halfResCopyCS->Release();
		halfResPostAlphaCS = nullptr;
		halfResCopyCS = nullptr;
		halfResMode = 0;
		return;
	}

	REX::INFO("[FG-GPU1] iHalfResDepthMV={}: depth{} cross at {}x{}{}", halfResMode,
		halfResMode == 2 ? " and motion vectors" : "", (a_width + 1) / 2, (a_height + 1) / 2,
		halfResMode == 1 ? "; depth is rebuilt at full resolution on the display GPU" :
	                       "; FSR3 works at half resolution");
}

void Upscaling::PreAlpha()
{
	auto rendererData = RE::BSGraphics::GetRendererData();
	auto context = reinterpret_cast<ID3D11DeviceContext*>(rendererData->context);
	
	auto& colorMain = rendererData->renderTargets[(uint)RenderTarget::kMain];
	auto& colorPostAlpha = rendererData->renderTargets[(uint)RenderTarget::kMainTemp];

	context->CopyResource(reinterpret_cast<ID3D11Texture2D*>(colorMain.texture), reinterpret_cast<ID3D11Texture2D*>(colorPostAlpha.texture));
}

void Upscaling::PostAlpha()
{
	if (!d3d12Interop)
		return;

	if (!setupBuffers)
		CreateFrameGenerationResources();

	auto rendererData = RE::BSGraphics::GetRendererData();

	auto context = reinterpret_cast<ID3D11DeviceContext*>(rendererData->context);
	auto dx12SwapChain = DX12SwapChain::GetSingleton();

	context->OMSetRenderTargets(0, nullptr, nullptr);

	{
		auto& colorPreAlpha = rendererData->renderTargets[(uint)RenderTarget::kMain];
		auto& colorPostAlpha = rendererData->renderTargets[(uint)RenderTarget::kMainTemp];

		auto& motionVector = rendererData->renderTargets[(uint)RenderTarget::kMotionVectors];
		auto& depth = rendererData->depthStencilTargets[(uint)DepthStencilTarget::kMain];

		{
			// iHalfResDepthMV: one thread per 2x2 block, same inputs and outputs.
			const UINT threadsX = halfResMode ? (dx12SwapChain->swapChainDesc.Width + 1) / 2 : dx12SwapChain->swapChainDesc.Width;
			const UINT threadsY = halfResMode ? (dx12SwapChain->swapChainDesc.Height + 1) / 2 : dx12SwapChain->swapChainDesc.Height;
			uint32_t dispatchX = (threadsX + 7) / 8;
			uint32_t dispatchY = (threadsY + 7) / 8;

			ID3D11ShaderResourceView* views[4] = {
				reinterpret_cast<ID3D11ShaderResourceView*>(colorPreAlpha.srView),
				reinterpret_cast<ID3D11ShaderResourceView*>(colorPostAlpha.srView),
				reinterpret_cast<ID3D11ShaderResourceView*>(motionVector.srView),
				reinterpret_cast<ID3D11ShaderResourceView*>(depth.srViewDepth)
			};

			context->CSSetShaderResources(0, ARRAYSIZE(views), views);

			ID3D11UnorderedAccessView* uavs[2] = { motionVectorBufferShared[dx12SwapChain->frameIndex]->uav.get(), depthBufferShared[dx12SwapChain->frameIndex]->uav.get()};
			context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);

			context->CSSetShader(halfResMode ? halfResPostAlphaCS : generateSharedBuffersCS, nullptr, 0);

			context->Dispatch(dispatchX, dispatchY, 1);
		}

		ID3D11ShaderResourceView* views[4] = { nullptr, nullptr, nullptr, nullptr };
		context->CSSetShaderResources(0, ARRAYSIZE(views), views);

		ID3D11UnorderedAccessView* uavs[2] = { nullptr, nullptr };
		context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);

		ID3D11ComputeShader* shader = nullptr;
		context->CSSetShader(shader, nullptr, 0);
	}

	// Depth and motion vectors are final now: start their PCIe copy instead of waiting for Present.
	dx12SwapChain->TransferEarlyLanes(CrossAdapterBridge::LaneTiming::kWorld);
}

void Upscaling::CopyBuffersToSharedResources()
{
	if (!d3d12Interop)
		return;

	if (!setupBuffers)
		CreateFrameGenerationResources();

	auto rendererData = RE::BSGraphics::GetRendererData();

	auto context = reinterpret_cast<ID3D11DeviceContext*>(rendererData->context);
	auto dx12SwapChain = DX12SwapChain::GetSingleton();
	
	context->OMSetRenderTargets(0, nullptr, nullptr);

	auto& motionVector = rendererData->renderTargets[(uint)RenderTarget::kMotionVectors];

	if (halfResMode) {
		// iHalfResDepthMV: the shared buffers are smaller than the game's, so one pass shrinks
		// depth (and, in mode 2, motion vectors) instead of the copies below.
		auto& depth = rendererData->depthStencilTargets[(uint)DepthStencilTarget::kMain];
		const UINT threadsX = (dx12SwapChain->swapChainDesc.Width + 1) / 2;
		const UINT threadsY = (dx12SwapChain->swapChainDesc.Height + 1) / 2;

		ID3D11ShaderResourceView* views[4] = { nullptr, nullptr,
			reinterpret_cast<ID3D11ShaderResourceView*>(motionVector.srView),
			reinterpret_cast<ID3D11ShaderResourceView*>(depth.srViewDepth) };
		context->CSSetShaderResources(0, ARRAYSIZE(views), views);
		ID3D11UnorderedAccessView* uavs[2] = { motionVectorBufferShared[dx12SwapChain->frameIndex]->uav.get(),
			depthBufferShared[dx12SwapChain->frameIndex]->uav.get() };
		context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);
		context->CSSetShader(halfResCopyCS, nullptr, 0);
		context->Dispatch((threadsX + 7) / 8, (threadsY + 7) / 8, 1);

		ID3D11ShaderResourceView* nullViews[4] = { nullptr, nullptr, nullptr, nullptr };
		context->CSSetShaderResources(0, ARRAYSIZE(nullViews), nullViews);
		ID3D11UnorderedAccessView* nullUavs[2] = { nullptr, nullptr };
		context->CSSetUnorderedAccessViews(0, ARRAYSIZE(nullUavs), nullUavs, nullptr);
		context->CSSetShader(nullptr, nullptr, 0);
		return;
	}

	context->CopyResource(motionVectorBufferShared[dx12SwapChain->frameIndex]->resource.get(), reinterpret_cast<ID3D11Texture2D*>(motionVector.texture));

	{
		auto& depth = rendererData->depthStencilTargets[(uint)DepthStencilTarget::kMain];

		{
			uint32_t dispatchX = (uint32_t)std::ceil(float(dx12SwapChain->swapChainDesc.Width) / 8.0f);
			uint32_t dispatchY = (uint32_t)std::ceil(float(dx12SwapChain->swapChainDesc.Height) / 8.0f);


			ID3D11ShaderResourceView* views[1] = { reinterpret_cast<ID3D11ShaderResourceView*>(depth.srViewDepth) };
			context->CSSetShaderResources(0, ARRAYSIZE(views), views);

			ID3D11UnorderedAccessView* uavs[1] = { depthBufferShared[dx12SwapChain->frameIndex]->uav.get() };
			context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);

			context->CSSetShader(copyDepthToSharedBufferCS, nullptr, 0);

			context->Dispatch(dispatchX, dispatchY, 1);
		}

		ID3D11ShaderResourceView* views[1] = { nullptr };
		context->CSSetShaderResources(0, ARRAYSIZE(views), views);

		ID3D11UnorderedAccessView* uavs[1] = { nullptr };
		context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);

		ID3D11ComputeShader* shader = nullptr;
		context->CSSetShader(shader, nullptr, 0);
	}	
}

void Upscaling::TimerSleepQPC(int64_t targetQPC)
{
	LARGE_INTEGER currentQPC;
	do {
		QueryPerformanceCounter(&currentQPC);
	} while (currentQPC.QuadPart < targetQPC);
}

void Upscaling::FrameLimiter(bool a_useFrameGeneration)
{
	static LARGE_INTEGER lastFrame = {};

	if (d3d12Interop && settings.frameLimitMode) {

		// Stick within VRR bounds
		double bestRefreshRate = refreshRate - (refreshRate * refreshRate) / 3600.0;

		LARGE_INTEGER qpf;
		QueryPerformanceFrequency(&qpf);

		int64_t targetFrameTicks = int64_t(double(qpf.QuadPart) / (bestRefreshRate * (a_useFrameGeneration ? 0.5 : 1.0)));

		LARGE_INTEGER timeNow;
		QueryPerformanceCounter(&timeNow);
		int64_t delta = timeNow.QuadPart - lastFrame.QuadPart;
		if (delta < targetFrameTicks) {
			TimerSleepQPC(lastFrame.QuadPart + targetFrameTicks);
		}
	}

	QueryPerformanceCounter(&lastFrame);
}

void Upscaling::GameFrameLimiter()
{
	double bestRefreshRate = 60.0f;

	LARGE_INTEGER qpf;
	QueryPerformanceFrequency(&qpf);

	int64_t targetFrameTicks = int64_t(double(qpf.QuadPart) / bestRefreshRate);

	static LARGE_INTEGER lastFrame = {};
	LARGE_INTEGER timeNow;
	QueryPerformanceCounter(&timeNow);
	int64_t delta = timeNow.QuadPart - lastFrame.QuadPart;
	if (delta < targetFrameTicks) {
		TimerSleepQPC(lastFrame.QuadPart + targetFrameTicks);
	}
	QueryPerformanceCounter(&lastFrame);	
}

/*
* Copyright (c) 2022-2023 NVIDIA CORPORATION. All rights reserved
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to deal
* in the Software without restriction, including without limitation the rights
* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
* copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in all
* copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
* SOFTWARE.
*/

double Upscaling::GetRefreshRate(HWND a_window)
{
	HMONITOR monitor = MonitorFromWindow(a_window, MONITOR_DEFAULTTONEAREST);
	MONITORINFOEXW info;
	info.cbSize = sizeof(info);
	if (GetMonitorInfoW(monitor, &info) != 0) {
		// using the CCD get the associated path and display configuration
		UINT32 requiredPaths, requiredModes;
		if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &requiredPaths, &requiredModes) == ERROR_SUCCESS) {
			std::vector<DISPLAYCONFIG_PATH_INFO> paths(requiredPaths);
			std::vector<DISPLAYCONFIG_MODE_INFO> modes2(requiredModes);
			if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &requiredPaths, paths.data(), &requiredModes, modes2.data(), nullptr) == ERROR_SUCCESS) {
				// iterate through all the paths until find the exact source to match
				for (auto& p : paths) {
					DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName;
					sourceName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
					sourceName.header.size = sizeof(sourceName);
					sourceName.header.adapterId = p.sourceInfo.adapterId;
					sourceName.header.id = p.sourceInfo.id;
					if (DisplayConfigGetDeviceInfo(&sourceName.header) == ERROR_SUCCESS) {
						// find the matched device which is associated with current device
						// there may be the possibility that display may be duplicated and windows may be one of them in such scenario
						// there may be two callback because source is same target will be different
						// as window is on both the display so either selecting either one is ok
						if (wcscmp(info.szDevice, sourceName.viewGdiDeviceName) == 0) {
							// get the refresh rate
							UINT numerator = p.targetInfo.refreshRate.Numerator;
							UINT denominator = p.targetInfo.refreshRate.Denominator;
							return (double)numerator / (double)denominator;
						}
					}
				}
			}
		}
	}
	REX::ERROR("Failed to retrieve refresh rate from swap chain");
	return 60;
}

void Upscaling::PostDisplay()
{
	if (!d3d12Interop)
		return;

	if (!setupBuffers)
		CreateFrameGenerationResources();

	auto dx12SwapChain = DX12SwapChain::GetSingleton();
	if (dx12SwapChain->secondaryGPU && !hudlessActive)
		return;  // nothing reads the HUD-less buffer, so don't spend bandwidth on the copy

	auto rendererData = RE::BSGraphics::GetRendererData();

	auto& swapChain = rendererData->renderTargets[(uint)RenderTarget::kFrameBuffer];
	ID3D11Resource* swapChainResource;
	reinterpret_cast<ID3D11RenderTargetView*>(swapChain.rtView)->GetResource(&swapChainResource);

	auto context = reinterpret_cast<ID3D11DeviceContext*>(rendererData->context);
	context->CopyResource(HUDLessBufferShared[dx12SwapChain->frameIndex]->resource.get(), swapChainResource);

	static bool loggedOnce = false;
	if (!loggedOnce) {
		REX::INFO("[FG] PostDisplay captured HUDLess (frameIdx={})", dx12SwapChain->frameIndex);
		loggedOnce = true;
	}

	// The UI-free frame is final now: start its PCIe copy instead of waiting for Present.
	dx12SwapChain->TransferEarlyLanes(CrossAdapterBridge::LaneTiming::kHudless);
}

void Upscaling::GenerateUIBuffer()
{
	if (!d3d12Interop || !setupBuffers || !generateUIBufferCS)
		return;

	auto dx12SwapChain = DX12SwapChain::GetSingleton();

	// Get the backbuffer SRV (final frame with UI composited)
	ID3D11ShaderResourceView* backbufferSRV = nullptr;
	extern bool enbLoaded;
	if (enbLoaded)
		backbufferSRV = dx12SwapChain->swapChainBufferProxyENB->srv;
	else
		backbufferSRV = dx12SwapChain->swapChainBufferProxy->srv.get();

	if (!backbufferSRV) {
		static bool loggedOnce = false;
		if (!loggedOnce) {
			REX::WARN("[FG] GenerateUIBuffer: no backbuffer SRV available");
			loggedOnce = true;
		}
		return;
	}

	auto rendererData = RE::BSGraphics::GetRendererData();
	auto context = reinterpret_cast<ID3D11DeviceContext*>(rendererData->context);

	// Unbind render targets — the proxy backbuffer may still be bound as RTV from game rendering.
	// D3D11 silently returns zeros when reading a resource that's simultaneously bound as RTV.
	context->OMSetRenderTargets(0, nullptr, nullptr);

	uint32_t dispatchX = (uint32_t)std::ceil(float(dx12SwapChain->swapChainDesc.Width) / 8.0f);
	uint32_t dispatchY = (uint32_t)std::ceil(float(dx12SwapChain->swapChainDesc.Height) / 8.0f);

	// t0 = HUDLess (clean scene), t1 = Backbuffer (scene + UI)
	ID3D11ShaderResourceView* srvs[2] = {
		HUDLessBufferShared[dx12SwapChain->frameIndex]->srv.get(),
		backbufferSRV
	};
	context->CSSetShaderResources(0, 2, srvs);

	// u0 = UIColorAndAlpha output
	ID3D11UnorderedAccessView* uavs[1] = {
		UIColorAlphaShared[dx12SwapChain->frameIndex]->uav.get()
	};
	context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

	context->CSSetShader(generateUIBufferCS, nullptr, 0);
	context->Dispatch(dispatchX, dispatchY, 1);

	// Unbind
	ID3D11ShaderResourceView* nullSRVs[2] = { nullptr, nullptr };
	context->CSSetShaderResources(0, 2, nullSRVs);
	ID3D11UnorderedAccessView* nullUAVs[1] = { nullptr };
	context->CSSetUnorderedAccessViews(0, 1, nullUAVs, nullptr);
	context->CSSetShader(nullptr, nullptr, 0);

	static bool loggedOnce = false;
	if (!loggedOnce) {
		REX::INFO("[FG] GenerateUIBuffer: dispatch={}x{}", dispatchX, dispatchY);
		loggedOnce = true;
	}
}

void Upscaling::Reset()
{
	if (!d3d12Interop)
		return;

	if (!setupBuffers)
		CreateFrameGenerationResources();

	auto rendererData = RE::BSGraphics::GetRendererData();
	auto context = reinterpret_cast<ID3D11DeviceContext*>(rendererData->context);

	auto dx12SwapChain = DX12SwapChain::GetSingleton();

	FLOAT clearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	context->ClearRenderTargetView(HUDLessBufferShared[dx12SwapChain->frameIndex]->rtv.get(), clearColor);
	context->ClearRenderTargetView(depthBufferShared[dx12SwapChain->frameIndex]->rtv.get(), clearColor);
	context->ClearRenderTargetView(motionVectorBufferShared[dx12SwapChain->frameIndex]->rtv.get(), clearColor);
	context->ClearRenderTargetView(UIColorAlphaShared[dx12SwapChain->frameIndex]->rtv.get(), clearColor);
}

struct WindowSizeChanged
{
	static void thunk(RE::BSGraphics::Renderer*, unsigned int)
	{
	}
	static inline REL::Relocation<decltype(thunk)> func;
};

struct SetUseDynamicResolutionViewportAsDefaultViewport
{
	static void thunk(RE::BSGraphics::RenderTargetManager* This, bool a_true)
	{
		auto upscaling = Upscaling::GetSingleton();

		func(This, a_true);

		if (!a_true) {
			// Imagespace just completed — capture HUDLess (pre-UI scene)
			upscaling->imagespaceComplete = true;
			upscaling->PostDisplay();
		} else {
			upscaling->imagespaceComplete = false;
		}
	}
	static inline REL::Relocation<decltype(thunk)> func;
};

bool reticleFix = false;

struct DrawWorld_Forward
{
	static void thunk(void* a1)
	{		
		func(a1);

		if (!reticleFix)
			Upscaling::GetSingleton()->CopyBuffersToSharedResources();

		reticleFix = false;
	}
	static inline REL::Relocation<decltype(thunk)> func;
};

struct DrawWorld_Reticle
{
	static void thunk(void* a1)
	{
		auto upscaling = Upscaling::GetSingleton();
		upscaling->PreAlpha();
		func(a1);
		reticleFix = true;
		upscaling->PostAlpha();
	}
	static inline REL::Relocation<decltype(thunk)> func;
};

void Upscaling::InstallHooks()
{
	auto runtimeIdx = static_cast<std::uint8_t>(REX::FModule::GetRuntimeIndex());

	// Fix game initialising twice
	stl::detour_thunk<WindowSizeChanged>(REL::ID({ 212827, 2276824, 2276824 }));

	// Watch frame presentation
	constexpr std::ptrdiff_t dynResOffsets[] = { 0xE1, 0xC5, 0xC5 };
	stl::write_thunk_call<SetUseDynamicResolutionViewportAsDefaultViewport>(
		REL::ID({ 587723, 2318322, 2318322 }).address() + dynResOffsets[runtimeIdx]);

	// Fix reticles on motion vectors and depth
	stl::detour_thunk<DrawWorld_Forward>(REL::ID({ 656535, 2318315, 2318315 }));

	constexpr std::ptrdiff_t reticleOffsets[] = { 0x253, 0x53D, 0x53D };
	stl::write_thunk_call<DrawWorld_Reticle>(
		REL::ID({ 338205, 2318315, 2318315 }).address() + reticleOffsets[runtimeIdx]);

	REX::INFO("[Upscaling] Installed hooks");
}
