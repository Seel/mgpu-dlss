#pragma once

// SecondaryUpscale - FG-GPU1: FSR 4.1 upscaling on the display GPU.
//
// With Upscaling.dll's iUpscaleMethodPreference = 3, the render GPU draws the 3D scene at the
// quality mode's resolution with a sub-pixel jitter and no TAA (Upscaling.dll), and skips the
// upscale. This plugin then
//
//   render GPU   PostDisplay: copies the render-resolution scene (UI-free) into its lane and fills
//                the full-size frame buffer from it with a plain bilinear blit, so the UI is drawn
//                at full resolution over a background (SecondaryUpscaleCS.hlsl, Blit).
//   display GPU  as soon as the scene, depth and motion-vector lanes are read (before Present with
//                bEarlyUnpack): FSR 4.1 render -> display resolution on the direct queue.
//                At Present: the finished frame (blit + UI) is unpacked into a mirror; the composite
//                puts its UI over the FSR 4.1 result into the back buffer (Composite), and frame
//                generation reads the FSR 4.1 result as the UI-free frame.
//
// Frames Upscaling.dll renders natively (menus, before this is ready) are copied to the back
// buffer as they are, without frame generation: the depth and motion-vector lanes are sized for
// the render resolution. Everything runs on the game's render thread.

#include <d3d12.h>
#include <dxgi.h>
#include <winrt/base.h>

#include <array>
#include <cstdint>

class SecondaryUpscale
{
public:
	static constexpr UINT kSlots = 2;
	// FGGPU1_UpscaleApiVersion / FO4Upscaling_SecondaryGPUApiVersion: both plugins must agree.
	static constexpr uint32_t kApiVersion = 1;

	static SecondaryUpscale* GetSingleton()
	{
		static SecondaryUpscale singleton;
		return &singleton;
	}

	// At swap-chain creation, after the FSR frame-generation contexts. When Upscaling.dll's settings
	// ask for it and everything it needs is there, creates the FSR 4.1 context, its outputs and the
	// composite. False (logged) leaves the session exactly as before.
	bool Initialize(ID3D12Device* a_device, ID3D12CommandQueue* a_queue, UINT a_displayWidth, UINT a_displayHeight,
		DXGI_FORMAT a_backBufferFormat);

	// Set up for it: the lanes are sized for the render resolution and the finished frame is
	// unpacked into a mirror. Fixed for the session.
	[[nodiscard]] bool Planned() const noexcept { return planned; }
	// Frames can be upscaled: planned, lanes bound, nothing has failed.
	[[nodiscard]] bool Ready() const noexcept { return planned && bound && !failed; }
	[[nodiscard]] UINT RenderWidth() const noexcept { return renderWidth; }
	[[nodiscard]] UINT RenderHeight() const noexcept { return renderHeight; }

	// Upscaling.dll asks every frame before it renders at a_width x a_height.
	bool ReadyFor(uint32_t a_width, uint32_t a_height);

	// The finished frame's lane (CreateInterop), and the scene/depth/motion-vector lanes
	// (CreateFrameGenerationResources). BindLanes false (logged) means frames cannot be upscaled.
	void SetFinalLane(int a_lane) { finalLane = a_lane; }
	[[nodiscard]] bool HasFinalMirror() const noexcept;
	bool BindLanes(int a_sceneLane, int a_depthLane, int a_motionLane);
	void Fail(const char* a_reason);

	// PostDisplay of a frame the render GPU drew for upscaling, after its scene lane was sent.
	// Queues the upscale as soon as the three lanes' reads are queued.
	void BeginFrame(UINT a_slot, float a_jitterX, float a_jitterY, float a_sharpness);
	// Present, after the bridge's Unpack: queues the upscale if BeginFrame could not yet. True when
	// the frame was upscaled.
	bool FinishSubmit(UINT a_slot);
	// On the frame's direct list: the composite (a_upscaled) or a copy of the finished frame into
	// the back buffer (PRESENT before and after).
	void RecordPresent(ID3D12GraphicsCommandList* a_list, UINT a_slot, bool a_upscaled, ID3D12Resource* a_backBuffer);
	// The FSR 4.1 result of a_slot: the UI-free frame for frame generation.
	[[nodiscard]] ID3D12Resource* Output(UINT a_slot) const noexcept;
	// After the frame's list was executed.
	void EndFrame(UINT a_slot, bool a_upscaled);

	// Every ~10 s from Present.
	void LogStats();

private:
	static constexpr UINT kMarkUpscaleBegin = 0;
	static constexpr UINT kMarkUpscaleEnd = 1;
	static constexpr UINT kMarkPresentBegin = 2;
	static constexpr UINT kMarkPresentEnd = 3;
	static constexpr UINT kMarks = 4;
	static constexpr UINT kDescriptorsPerSlot = 4;  // t0 scene, t1 finished frame, t2 FSR output, u0 composite

	struct FrameParams
	{
		float jitterX = 0.0f;
		float jitterY = 0.0f;
		float sharpness = 0.0f;
		float cameraNear = 0.0f;
		float cameraFar = 0.0f;
		float fovY = 1.0f;
	};

	bool CreatePipeline();
	bool TrySubmit(UINT a_slot);
	void CollectTimings(UINT a_slot);

	bool planned = false;
	bool bound = false;
	bool failed = false;

	UINT renderWidth = 0;
	UINT renderHeight = 0;
	UINT displayWidth = 0;
	UINT displayHeight = 0;
	int qualityMode = 1;

	int finalLane = -1;
	int sceneLane = -1;
	int depthLane = -1;
	int motionLane = -1;

	winrt::com_ptr<ID3D12Device> device;
	ID3D12CommandQueue* queue = nullptr;  // the FFX game queue, not owned

	std::array<winrt::com_ptr<ID3D12Resource>, kSlots> outputs;  // FSR 4.1 results (UI-free, display size)
	winrt::com_ptr<ID3D12Resource> composite;                    // UI over the FSR 4.1 result, copied to the back buffer

	winrt::com_ptr<ID3D12RootSignature> rootSignature;
	winrt::com_ptr<ID3D12PipelineState> pipeline;
	winrt::com_ptr<ID3D12DescriptorHeap> descriptors;
	UINT descriptorSize = 0;

	// The upscale's own list per slot, submitted ahead of the frame's list on the same queue.
	std::array<winrt::com_ptr<ID3D12CommandAllocator>, kSlots> allocators;
	winrt::com_ptr<ID3D12GraphicsCommandList> list;

	std::array<bool, kSlots> pending{};    // BeginFrame ran, upscale not queued yet
	std::array<bool, kSlots> submitted{};  // upscale queued for this frame
	std::array<FrameParams, kSlots> params{};
	bool lastFrameUpscaled = false;
	LARGE_INTEGER lastDispatch{};

	// Timestamps on the direct queue (durations only).
	winrt::com_ptr<ID3D12QueryHeap> queryHeap;
	winrt::com_ptr<ID3D12Resource> readback;
	UINT64 timestampFrequency = 0;
	std::array<UINT, kSlots> written{};  // bit per mark recorded since the slot was last read

	struct Average
	{
		double sum = 0.0;
		double max = 0.0;
		UINT64 count = 0;
		void Add(double a_value)
		{
			max = (count == 0 || a_value > max) ? a_value : max;
			sum += a_value;
			++count;
		}
		[[nodiscard]] double Mean() const { return count ? sum / static_cast<double>(count) : 0.0; }
	};

	Average statUpscale;    // ms, FSR 4.1 on the display GPU
	Average statComposite;  // ms, composite + copy into the back buffer
	UINT64 statUpscaled = 0;
	UINT64 statNative = 0;
	UINT64 statResets = 0;
	LARGE_INTEGER statLastLog{};
	bool loggedFirstDispatch = false;
	bool loggedSizeMismatch = false;
};
