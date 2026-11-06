#pragma once

// FSR SDK 2.4 API headers, vendored unmodified under include/ffx2 (MIT). They describe both
// runtimes this plugin can load: the struct type IDs and layouts used with FidelityFX SDK 1.1.4
// are unchanged in 2.4, so the old runtime is driven with the same headers and simply never
// receives the structs that only 2.x knows (version descriptors, PrepareV2, HUD-less format).
#include <ffx2/api/include/ffx_api.hpp>
#include <ffx2/api/include/ffx_api_loader.h>
#include <ffx2/api/include/ffx_api_types.h>
#include <ffx2/api/include/dx12/ffx_api_dx12.hpp>
#include <ffx2/framegeneration/include/ffx_framegeneration.hpp>
#include <ffx2/framegeneration/include/dx12/ffx_api_framegeneration_dx12.hpp>

#include "Buffer.h"

// Entry points of whichever runtime DLL LoadFFX() loaded. The SDK's own ffx:: call helpers call
// the ffx* exports directly (meant for linking an import library), so calls go through here.
extern ffxFunctions ffxModule;

namespace ffxm
{
	template <class... Desc>
	inline ffx::ReturnCode CreateContext(ffx::Context& a_context, Desc&... a_desc)
	{
		return static_cast<ffx::ReturnCode>(ffxModule.CreateContext(&a_context, ffx::LinkHeaders(a_desc.header...), nullptr));
	}

	template <class... Desc>
	inline ffx::ReturnCode Configure(ffx::Context& a_context, Desc&... a_desc)
	{
		return static_cast<ffx::ReturnCode>(ffxModule.Configure(&a_context, ffx::LinkHeaders(a_desc.header...)));
	}

	template <class... Desc>
	inline ffx::ReturnCode Dispatch(ffx::Context& a_context, Desc&... a_desc)
	{
		return static_cast<ffx::ReturnCode>(ffxModule.Dispatch(&a_context, ffx::LinkHeaders(a_desc.header...)));
	}

	template <class... Desc>
	inline ffx::ReturnCode Query(ffx::Context& a_context, Desc&... a_desc)
	{
		return static_cast<ffx::ReturnCode>(ffxModule.Query(&a_context, ffx::LinkHeaders(a_desc.header...)));
	}

	template <class... Desc>
	inline ffx::ReturnCode QueryGlobal(Desc&... a_desc)
	{
		return static_cast<ffx::ReturnCode>(ffxModule.Query(nullptr, ffx::LinkHeaders(a_desc.header...)));
	}

	const char* ReturnCodeName(uint32_t a_rc);
}

class FidelityFX
{
public:
	static FidelityFX* GetSingleton()
	{
		static FidelityFX singleton;
		return &singleton;
	}

	// iFSRRuntime. kLegacy is what this plugin shipped with and is driven exactly as before.
	enum class Runtime : int
	{
		kLegacy = 0,  // FidelityFX SDK 1.1.4: amd_fidelityfx_dx12.dll, FSR 3.1.4
		kSDK24 = 1    // FSR SDK 2.4: amd_fidelityfx_loader_dx12.dll + amd_fidelityfx_framegeneration_dx12.dll
	};

	HMODULE module = nullptr;
	Runtime runtime = Runtime::kLegacy;

	ffx::Context swapChainContext{};
	ffx::Context frameGenContext{};
	HWND window = nullptr;

	void LoadFFX();
	// Creates the FFX frame-generation swap chain and the frame generation context. With the SDK
	// 2.4 runtime a creation failure falls back to SDK 1.1.4 once, so the game still starts.
	void CreateSwapChain(IDXGIFactory5* a_dxgiFactory, DXGI_SWAP_CHAIN_DESC1* a_desc, HWND a_window,
		ID3D12CommandQueue* a_queue, IDXGISwapChain4** a_swapChain);
	void Present(bool a_useFrameGeneration);

	[[nodiscard]] const char* RuntimeName() const;

private:
	bool LoadRuntime(Runtime a_runtime);
	bool CreateContexts(IDXGIFactory5* a_dxgiFactory, DXGI_SWAP_CHAIN_DESC1* a_desc, HWND a_window,
		ID3D12CommandQueue* a_queue, IDXGISwapChain4** a_swapChain);
	void DestroyContexts(IDXGISwapChain4** a_swapChain);
	void LogEnvironment(HWND a_window);
};
