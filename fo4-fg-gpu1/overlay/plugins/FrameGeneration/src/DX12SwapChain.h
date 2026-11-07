#pragma once

#include <Windows.Foundation.h>
#include <stdio.h>
#include <winrt/base.h>
#include <wrl\client.h>
#include <wrl\wrappers\corewrappers.h>

#include <d3d11_4.h>
#include <d3d12.h>

#include "Buffer.h"
#include "CrossAdapterBridge.h"

class WrappedResource
{
public:
	WrappedResource(D3D11_TEXTURE2D_DESC a_texDesc, ID3D11Device5* a_d3d11Device, ID3D12Device* a_d3d12Device);

	ID3D11Texture2D* resource11 = nullptr;
	ID3D11ShaderResourceView* srv = nullptr;
	ID3D11UnorderedAccessView* uav = nullptr;
	ID3D11RenderTargetView* rtv = nullptr;
	winrt::com_ptr<ID3D12Resource> resource;
};

struct DXGISwapChainProxy : IDXGISwapChain
{
public:
	DXGISwapChainProxy(IDXGISwapChain4* a_swapChain);

	IDXGISwapChain4* swapChain;

	/****IUnknown****/
	virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObj) override;
	virtual ULONG STDMETHODCALLTYPE AddRef() override;
	virtual ULONG STDMETHODCALLTYPE Release() override;

	/****IDXGIObject****/
	virtual HRESULT STDMETHODCALLTYPE SetPrivateData(_In_ REFGUID Name, UINT DataSize, _In_reads_bytes_(DataSize) const void* pData) override;
	virtual HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(_In_ REFGUID Name, _In_opt_ const IUnknown* pUnknown) override;
	virtual HRESULT STDMETHODCALLTYPE GetPrivateData(_In_ REFGUID Name, _Inout_ UINT* pDataSize, _Out_writes_bytes_(*pDataSize) void* pData) override;
	virtual HRESULT STDMETHODCALLTYPE GetParent(_In_ REFIID riid, _COM_Outptr_ void** ppParent) override;

	/****IDXGIDeviceSubObject****/
	virtual HRESULT STDMETHODCALLTYPE GetDevice(_In_ REFIID riid, _COM_Outptr_ void** ppDevice) override;

	/****IDXGISwapChain****/
	virtual HRESULT STDMETHODCALLTYPE Present(UINT SyncInterval, UINT Flags);
	virtual HRESULT STDMETHODCALLTYPE GetBuffer(UINT Buffer, _In_ REFIID riid, _COM_Outptr_ void** ppSurface);
	virtual HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL Fullscreen, _In_opt_ IDXGIOutput* pTarget);
	virtual HRESULT STDMETHODCALLTYPE GetFullscreenState(_Out_opt_ BOOL* pFullscreen, _COM_Outptr_opt_result_maybenull_ IDXGIOutput** ppTarget);
	virtual HRESULT STDMETHODCALLTYPE GetDesc(_Out_ DXGI_SWAP_CHAIN_DESC* pDesc);
	virtual HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags);
	virtual HRESULT STDMETHODCALLTYPE ResizeTarget(_In_ const DXGI_MODE_DESC* pNewTargetParameters);
	virtual HRESULT STDMETHODCALLTYPE GetContainingOutput(_COM_Outptr_ IDXGIOutput** ppOutput);
	virtual HRESULT STDMETHODCALLTYPE GetFrameStatistics(_Out_ DXGI_FRAME_STATISTICS* pStats);
	virtual HRESULT STDMETHODCALLTYPE GetLastPresentCount(_Out_ UINT* pLastPresentCount);
};

class DX12SwapChain
{
public:
	static DX12SwapChain* GetSingleton()
	{
		static DX12SwapChain singleton;
		return &singleton;
	}

	winrt::com_ptr<ID3D12Device> d3d12Device;
	winrt::com_ptr<ID3D12CommandQueue> commandQueue;
	winrt::com_ptr<ID3D12CommandAllocator> commandAllocators[2];
	winrt::com_ptr<ID3D12GraphicsCommandList4> commandLists[2];

	IDXGISwapChain4* swapChain;

	// The swap chain as the game asked for it: its format is the one the game renders (RGBA8) and
	// the D3D11 side (interop textures, lanes) uses.
	DXGI_SWAP_CHAIN_DESC1 swapChainDesc;
	// The format the back buffers really have (read from the buffers: ReShade's GetDesc1 reports the
	// requested one). An HDR add-on (ReShade's AutoHDR) can make it R10G10B10A2 or
	// R16G16B16A16_FLOAT; everything that writes the back buffer follows it.
	DXGI_FORMAT displayFormat = DXGI_FORMAT_UNKNOWN;

	Texture2D* swapChainBufferProxy;
	WrappedResource* swapChainBufferProxyENB;

	WrappedResource* swapChainBufferWrapped[2];

	winrt::com_ptr<ID3D11Device5> d3d11Device;
	winrt::com_ptr<ID3D11DeviceContext4> d3d11Context;

	winrt::com_ptr<ID3D11Fence> d3d11Fence;
	winrt::com_ptr<ID3D12Fence> d3d12Fence;

	winrt::com_ptr<ID3D12Resource> swapChainBuffers[2];

	UINT frameIndex = 0;
	UINT64 fenceValue = 0;

	// Diagnostics: game frames rendered vs frames actually presented (the game's own plus
	// FSR3-generated ones), reported every 10 s by Present().
	UINT lastPresentCount = 0;
	UINT64 statGameFrames = 0;
	UINT64 statPresentedFrames = 0;
	LARGE_INTEGER statPresentStart{};

	LARGE_INTEGER qpf;

	DXGISwapChainProxy* swapChainProxy = nullptr;

	// Secondary-GPU mode (bFrameGenOnSecondaryGPU). d3d12Device is then the device on the
	// display GPU that runs FFX frame generation and owns the swap chain; interopDevice is
	// on the game's adapter and is the one that opens the D3D11 shared textures/fence.
	// In single-GPU mode interopDevice is null and GetInteropDevice() returns d3d12Device.
	winrt::com_ptr<ID3D12Device> interopDevice;
	bool secondaryGPU = false;
	int finalFrameLane = -1;

	ID3D12Device* GetInteropDevice() const { return interopDevice ? interopDevice.get() : d3d12Device.get(); }

	void CreateD3D12Device(IDXGIAdapter* a_adapter);
	// Tries the two-device setup; falls back to CreateD3D12Device(a_gameAdapter) on any failure.
	void CreateD3D12DevicesSecondaryGPU(IDXGIAdapter* a_gameAdapter, HWND a_window);
	void CreateD3D12CommandQueues();
	void CreateSwapChain(IDXGIFactory5* a_dxgiFactory, DXGI_SWAP_CHAIN_DESC swapChainDesc);
	void CreateSwapChainFSR3(IDXGIFactory5* a_dxgiFactory, DXGI_SWAP_CHAIN_DESC a_swapChainDesc);
	void CreateSwapChainDLSSG(IDXGIFactory5* a_dxgiFactory, DXGI_SWAP_CHAIN_DESC a_swapChainDesc);
	bool CreateSwapChainXeSS(IDXGIFactory5* a_dxgiFactory, DXGI_SWAP_CHAIN_DESC a_swapChainDesc);

	void CreateInterop();

	DXGISwapChainProxy* GetSwapChainProxy();
	void SetD3D11Device(ID3D11Device* a_d3d11Device);
	void SetD3D11DeviceContext(ID3D11DeviceContext* a_d3d11Context);

	HRESULT GetBuffer(void** ppSurface);
	// Frame generation on a second GPU: called from the frame hook at which a group of lanes
	// becomes final (world pass, HUD-less capture), so their PCIe copy starts before Present.
	void TransferEarlyLanes(CrossAdapterBridge::LaneTiming a_timing);
	HRESULT Present(UINT SyncInterval, UINT Flags);
	HRESULT GetDevice(_In_ REFIID riid, _COM_Outptr_ void** ppDevice);
};
