#pragma once

// CrossAdapterBridge - frame generation on a second GPU.
//
// The game renders with D3D11 on the render GPU (e.g. RTX 3090). Stock
// AAAFrameGeneration opens the game's shared D3D11 textures on a D3D12 device
// on that SAME adapter and runs FSR3 FG + presents there. In secondary-GPU mode
// there are two D3D12 devices:
//
//   device A  "interop"  render GPU   opens the D3D11 NT-handle textures (as before)
//   device B  "fg"       display GPU  FFX FG context, FFX swap chain, present
//
// and this class moves every input from A to B once per game frame:
//
//   D3D11 Signal(producer) -> A copy queue: Wait(producer), Wait(consumed[slot]),
//   CopyTextureRegion texture -> row-major buffer placed in a
//   D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER heap, Signal(transfer) + Signal(copyDone)
//   -> B direct queue: Wait(transfer), buffer -> texture (mirror or back buffer),
//   ... FFX ..., Signal(consumed)
//   D3D11 context: Wait(copyDone of the previous transfer) before the game
//   writes the slot again, so the render GPU never waits on PCIe for the
//   frame it is working on.
//
// Everything runs on the game's render thread (Present and the render hooks).
// No method throws; failures are logged and reported through the return value.

#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <winrt/base.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

class CrossAdapterBridge
{
public:
	static constexpr UINT kSlots = 2;  // AAAFrameGeneration double-buffers every input by frameIndex

	static CrossAdapterBridge* GetSingleton()
	{
		static CrossAdapterBridge singleton;
		return &singleton;
	}

	// Picks the adapter that frame generation should run and present on.
	// Never the game's adapter, never a software adapter. With a name filter, the
	// single hardware adapter whose description contains it (case-insensitive).
	// Without one, the adapter that owns the monitor a_window is on; failing that,
	// the only remaining hardware adapter. Ambiguity returns null rather than guessing.
	static winrt::com_ptr<IDXGIAdapter1> SelectSecondaryAdapter(IDXGIAdapter* a_gameAdapter,
		HWND a_window, const std::string& a_nameFilter);

	static std::string AdapterName(IDXGIAdapter* a_adapter);

	// The game is usually placed on the GPU that drives the display, because old games
	// enumerate adapters and take the first one. For secondary-GPU mode the game must render
	// on the headless GPU instead. Returns that GPU when a_gameAdapter is the display GPU and
	// there is exactly one hardware adapter with no display attached; otherwise null.
	static winrt::com_ptr<IDXGIAdapter1> PickRenderAdapter(IDXGIAdapter* a_gameAdapter);

	// a_deviceA: on the game's adapter. a_deviceB: on the secondary adapter.
	// a_device11/a_context11: the game's D3D11 device and immediate context (adapter A).
	bool Initialize(ID3D12Device* a_deviceA, ID3D12Device* a_deviceB,
		ID3D11Device5* a_device11, ID3D11DeviceContext4* a_context11);

	[[nodiscard]] bool IsActive() const noexcept { return active; }

	// Registers one double-buffered input. a_sourcesA[slot] are single-subresource
	// textures on device A. With a_createMirror, same-sized textures are created on
	// device B and returned by Mirror(); without it, the caller unpacks the lane into
	// its own destination with RecordUnpackTo (used for the back buffer).
	// Returns the lane index, or -1 (logged; the bridge is deactivated).
	int AddLane(const char* a_name, ID3D12Resource* a_sourceSlot0, ID3D12Resource* a_sourceSlot1,
		bool a_createMirror);

	[[nodiscard]] ID3D12Resource* Mirror(int a_lane, UINT a_slot) const noexcept;

	// Once per game frame, after the D3D11 context signalled a_producerFenceA
	// (the fence object opened on device A) to a_producerValue. Records and
	// submits the A-side copies for a_slot and makes a_queueB wait for them.
	HRESULT Transfer(ID3D12Fence* a_producerFenceA, UINT64 a_producerValue, UINT a_slot,
		ID3D12CommandQueue* a_queueB);

	// On B's command list for this frame: staging -> mirror for every mirrored lane.
	void RecordUnpack(ID3D12GraphicsCommandList* a_listB, UINT a_slot);

	// On B's command list: staging -> a_destB for an unmirrored lane. a_destB is in
	// a_destState before and after.
	void RecordUnpackTo(ID3D12GraphicsCommandList* a_listB, int a_lane, UINT a_slot,
		ID3D12Resource* a_destB, D3D12_RESOURCE_STATES a_destState);

	// After B executed the list that unpacked this frame.
	HRESULT SignalConsumed(ID3D12CommandQueue* a_queueB);

	// After Present, before the game's D3D11 work for the next frame is recorded:
	// the D3D11 context waits (on the GPU) for the transfer that last read
	// a_nextSlot, which the next frame is about to overwrite. With strict
	// alternation that is the previous frame's transfer, so the render GPU does
	// not wait for the copy of the frame it just finished.
	HRESULT GateD3D11(UINT a_nextSlot);

	// Every ~10 s: payload size, copy time, queue lag.
	void LogStats();

	void Shutdown() noexcept;

private:
	struct Lane
	{
		std::string name;
		D3D12_RESOURCE_DESC desc{};
		D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};  // Offset relative to the slot base
		UINT64 slotStride = 0;
		UINT64 bytes = 0;
		std::array<winrt::com_ptr<ID3D12Resource>, kSlots> sourceA;
		winrt::com_ptr<ID3D12Heap> heapA;
		winrt::com_ptr<ID3D12Heap> heapB;
		winrt::com_ptr<ID3D12Resource> stagingA;
		winrt::com_ptr<ID3D12Resource> stagingB;
		std::array<winrt::com_ptr<ID3D12Resource>, kSlots> mirrorB;
	};

	HRESULT CreateSharedFencePair(UINT64 a_initial, winrt::com_ptr<ID3D12Fence>& a_onA,
		winrt::com_ptr<ID3D12Fence>& a_onB);
	HRESULT CreateSharedHeapPair(UINT64 a_size, winrt::com_ptr<ID3D12Heap>& a_onA,
		winrt::com_ptr<ID3D12Heap>& a_onB);
	HRESULT WaitCpu(ID3D12Fence* a_fence, UINT64 a_value);
	void Fail(const char* a_where, HRESULT a_hr);

	bool active = false;

	winrt::com_ptr<ID3D12Device> deviceA;
	winrt::com_ptr<ID3D12Device> deviceB;
	winrt::com_ptr<ID3D11DeviceContext4> context11;

	winrt::com_ptr<ID3D12CommandQueue> copyQueueA;
	std::array<winrt::com_ptr<ID3D12CommandAllocator>, kSlots> allocatorsA;
	winrt::com_ptr<ID3D12GraphicsCommandList> listA;

	// A signals, B waits. Cross-adapter.
	winrt::com_ptr<ID3D12Fence> transferFenceA;
	winrt::com_ptr<ID3D12Fence> transferFenceB;
	// B signals, A waits. Cross-adapter.
	winrt::com_ptr<ID3D12Fence> consumedFenceA;
	winrt::com_ptr<ID3D12Fence> consumedFenceB;
	// A signals, D3D11 waits. Same adapter.
	winrt::com_ptr<ID3D12Fence> copyDoneFenceA;
	winrt::com_ptr<ID3D11Fence> copyDoneFence11;

	HANDLE fenceEvent = nullptr;

	UINT64 transferValue = 0;                        // value of the latest Transfer
	std::array<UINT64, kSlots> slotTransfer{};       // transfer value that last used each slot

	std::vector<Lane> lanes;
	UINT64 bytesPerFrame = 0;

	// Copy-queue GPU timestamps, when the driver supports them.
	winrt::com_ptr<ID3D12QueryHeap> timestampHeap;
	winrt::com_ptr<ID3D12Resource> timestampReadback;
	UINT64 timestampFrequency = 0;

	// Statistics since the last LogStats.
	UINT64 statFrames = 0;
	double statGpuCopyMs = 0.0;
	UINT64 statGpuCopySamples = 0;
	double statGpuCopyMaxMs = 0.0;
	double statCpuSubmitMs = 0.0;
	UINT64 statMaxLag = 0;
	UINT64 statCpuWaits = 0;
	LARGE_INTEGER statLastLog{};
	LARGE_INTEGER qpcFrequency{};
};
