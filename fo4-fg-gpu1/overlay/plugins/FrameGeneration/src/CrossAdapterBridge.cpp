#include "CrossAdapterBridge.h"

#include <algorithm>
#include <cctype>

namespace
{
	constexpr UINT64 kHeapAlignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;  // 64 KiB

	UINT64 AlignUp(UINT64 a_value, UINT64 a_alignment)
	{
		return (a_value + a_alignment - 1) & ~(a_alignment - 1);
	}

	bool LuidEqual(const LUID& a_left, const LUID& a_right)
	{
		return a_left.LowPart == a_right.LowPart && a_left.HighPart == a_right.HighPart;
	}

	std::string Narrow(const wchar_t* a_text)
	{
		std::string out;
		for (; a_text && *a_text; ++a_text)
			out += (*a_text < 0x80) ? static_cast<char>(*a_text) : '?';
		return out;
	}

	std::string Lower(std::string a_text)
	{
		std::transform(a_text.begin(), a_text.end(), a_text.begin(),
			[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return a_text;
	}

	bool OwnsMonitor(IDXGIAdapter1* a_adapter, HMONITOR a_monitor)
	{
		if (!a_monitor)
			return false;
		for (UINT index = 0;; ++index) {
			winrt::com_ptr<IDXGIOutput> output;
			const HRESULT hr = a_adapter->EnumOutputs(index, output.put());
			if (hr == DXGI_ERROR_NOT_FOUND)
				return false;
			if (FAILED(hr))
				return false;
			DXGI_OUTPUT_DESC desc{};
			// An output that is not attached to the desktop cannot be the monitor the game
			// window is on; its Monitor field is not a live handle.
			if (output && SUCCEEDED(output->GetDesc(&desc)) && desc.AttachedToDesktop && desc.Monitor == a_monitor)
				return true;
		}
	}

	bool HasAttachedOutput(IDXGIAdapter1* a_adapter)
	{
		for (UINT index = 0;; ++index) {
			winrt::com_ptr<IDXGIOutput> output;
			const HRESULT hr = a_adapter->EnumOutputs(index, output.put());
			if (hr == DXGI_ERROR_NOT_FOUND)
				return false;
			if (FAILED(hr))
				return false;
			DXGI_OUTPUT_DESC desc{};
			if (SUCCEEDED(output->GetDesc(&desc)) && desc.AttachedToDesktop)
				return true;
		}
	}

	void LogOutputs(IDXGIAdapter1* a_adapter, const std::string& a_name)
	{
		UINT count = 0;
		for (UINT index = 0;; ++index) {
			winrt::com_ptr<IDXGIOutput> output;
			const HRESULT hr = a_adapter->EnumOutputs(index, output.put());
			if (hr == DXGI_ERROR_NOT_FOUND)
				break;
			if (FAILED(hr)) {
				REX::WARN("[FG-GPU1]   output[{}] EnumOutputs failed hr={:#010x}", index, static_cast<std::uint32_t>(hr));
				break;
			}
			DXGI_OUTPUT_DESC desc{};
			if (SUCCEEDED(output->GetDesc(&desc))) {
				REX::INFO("[FG-GPU1]   output[{}] \"{}\" attached={} monitor={:#x}", index, Narrow(desc.DeviceName),
					desc.AttachedToDesktop ? 1 : 0, reinterpret_cast<std::uintptr_t>(desc.Monitor));
				++count;
			}
		}
		REX::INFO("[FG-GPU1]   \"{}\" outputs: {}", a_name, count);
	}

	D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* a_resource, D3D12_RESOURCE_STATES a_before,
		D3D12_RESOURCE_STATES a_after)
	{
		D3D12_RESOURCE_BARRIER barrier{};
		barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
		barrier.Transition.pResource = a_resource;
		barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		barrier.Transition.StateBefore = a_before;
		barrier.Transition.StateAfter = a_after;
		return barrier;
	}

	double QpcMs(const LARGE_INTEGER& a_from, const LARGE_INTEGER& a_to, const LARGE_INTEGER& a_frequency)
	{
		return static_cast<double>(a_to.QuadPart - a_from.QuadPart) * 1000.0 /
		       static_cast<double>(a_frequency.QuadPart);
	}
}

winrt::com_ptr<IDXGIAdapter1> CrossAdapterBridge::PickRenderAdapter(IDXGIAdapter* a_gameAdapter)
{
	if (!a_gameAdapter)
		return nullptr;

	DXGI_ADAPTER_DESC gameDesc{};
	if (FAILED(a_gameAdapter->GetDesc(&gameDesc)))
		return nullptr;

	winrt::com_ptr<IDXGIFactory1> factory;
	if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(factory.put()))))
		return nullptr;

	winrt::com_ptr<IDXGIAdapter1> display;
	std::vector<winrt::com_ptr<IDXGIAdapter1>> headless;
	for (UINT index = 0;; ++index) {
		winrt::com_ptr<IDXGIAdapter1> adapter;
		if (factory->EnumAdapters1(index, adapter.put()) == DXGI_ERROR_NOT_FOUND)
			break;
		DXGI_ADAPTER_DESC1 desc{};
		if (FAILED(adapter->GetDesc1(&desc)))
			continue;
		if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0 || desc.VendorId == 0x1414)
			continue;
		if (HasAttachedOutput(adapter.get()))
			display = adapter;
		else
			headless.push_back(adapter);
	}

	// Only act when the split is unambiguous: one GPU drives the display, one drives nothing.
	if (!display || headless.size() != 1)
		return nullptr;

	DXGI_ADAPTER_DESC1 displayDesc{};
	if (FAILED(display->GetDesc1(&displayDesc)))
		return nullptr;

	// Only move rendering when the game picked the display GPU. The target has no display
	// attached, so this cannot invert the intended roles if detection is off.
	if (!LuidEqual(gameDesc.AdapterLuid, displayDesc.AdapterLuid))
		return nullptr;

	return headless.front();
}

std::string CrossAdapterBridge::AdapterName(IDXGIAdapter* a_adapter){
	DXGI_ADAPTER_DESC desc{};
	if (!a_adapter || FAILED(a_adapter->GetDesc(&desc)))
		return "<unknown>";
	return Narrow(desc.Description);
}

winrt::com_ptr<IDXGIAdapter1> CrossAdapterBridge::SelectSecondaryAdapter(IDXGIAdapter* a_gameAdapter,
	HWND a_window, const std::string& a_nameFilter)
{
	DXGI_ADAPTER_DESC gameDesc{};
	if (!a_gameAdapter || FAILED(a_gameAdapter->GetDesc(&gameDesc))) {
		REX::ERROR("[FG-GPU1] Cannot read the game's adapter; staying on one GPU");
		return nullptr;
	}

	winrt::com_ptr<IDXGIFactory1> factory;
	if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(factory.put())))) {
		REX::ERROR("[FG-GPU1] CreateDXGIFactory1 failed; staying on one GPU");
		return nullptr;
	}

	const HMONITOR monitor = a_window ? MonitorFromWindow(a_window, MONITOR_DEFAULTTONEAREST) : nullptr;
	const std::string filter = Lower(a_nameFilter);

	{
		MONITORINFOEXW info{};
		info.cbSize = sizeof(info);
		const bool resolved = monitor && GetMonitorInfoW(monitor, &info);
		REX::INFO("[FG-GPU1] game window monitor {:#x} \"{}\"", reinterpret_cast<std::uintptr_t>(monitor),
			resolved ? Narrow(info.szDevice) : "unresolved");
	}

	std::vector<winrt::com_ptr<IDXGIAdapter1>> candidates;
	std::vector<winrt::com_ptr<IDXGIAdapter1>> named;
	std::vector<winrt::com_ptr<IDXGIAdapter1>> displaying;
	std::vector<winrt::com_ptr<IDXGIAdapter1>> withOutputs;

	for (UINT index = 0;; ++index) {
		winrt::com_ptr<IDXGIAdapter1> adapter;
		if (factory->EnumAdapters1(index, adapter.put()) == DXGI_ERROR_NOT_FOUND)
			break;
		DXGI_ADAPTER_DESC1 desc{};
		if (FAILED(adapter->GetDesc1(&desc)))
			continue;

		const std::string name = Narrow(desc.Description);
		const bool software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0 || desc.VendorId == 0x1414;
		const bool isGame = LuidEqual(desc.AdapterLuid, gameDesc.AdapterLuid);
		const bool ownsMonitor = OwnsMonitor(adapter.get(), monitor);
		const bool hasOutput = HasAttachedOutput(adapter.get());

		REX::INFO("[FG-GPU1] adapter[{}] \"{}\" vendor={:#06x} luid={:#010x}-{:#010x} vram={}MB{}{}{}{}",
			index, name, desc.VendorId,
			static_cast<std::uint32_t>(desc.AdapterLuid.HighPart), desc.AdapterLuid.LowPart,
			desc.DedicatedVideoMemory / (1024 * 1024),
			isGame ? " [game]" : "", software ? " [software]" : "", ownsMonitor ? " [game window's monitor]" : "",
			hasOutput ? " [display]" : "");
		LogOutputs(adapter.get(), name);

		if (software || isGame)
			continue;
		candidates.push_back(adapter);
		if (!filter.empty() && Lower(name).find(filter) != std::string::npos)
			named.push_back(adapter);
		if (ownsMonitor)
			displaying.push_back(adapter);
		if (hasOutput)
			withOutputs.push_back(adapter);
	}

	const auto pick = [](const std::vector<winrt::com_ptr<IDXGIAdapter1>>& a_list, const char* a_rule)
		-> winrt::com_ptr<IDXGIAdapter1> {
		if (a_list.size() != 1)
			return nullptr;
		REX::INFO("[FG-GPU1] SELECTED \"{}\" ({})", AdapterName(a_list.front().get()), a_rule);
		return a_list.front();
	};

	if (!filter.empty()) {
		if (auto adapter = pick(named, "sSecondaryGPUName match"))
			return adapter;
		REX::ERROR("[FG-GPU1] sSecondaryGPUName=\"{}\" matched {} non-game hardware adapters (need exactly 1); staying on one GPU",
			a_nameFilter, named.size());
		return nullptr;
	}
	if (auto adapter = pick(displaying, "owns the monitor the game window is on"))
		return adapter;
	if (auto adapter = pick(withOutputs, "drives a display"))
		return adapter;
	if (auto adapter = pick(candidates, "only other hardware adapter"))
		return adapter;

	REX::ERROR("[FG-GPU1] {} candidate adapters and none is unambiguous; set sSecondaryGPUName. Staying on one GPU",
		candidates.size());
	return nullptr;
}

void CrossAdapterBridge::Fail(const char* a_where, HRESULT a_hr)
{
	if (active)
		REX::CRITICAL("[FG-GPU1] {} failed (hr={:#010x}); cross-adapter transport disabled", a_where,
			static_cast<std::uint32_t>(a_hr));
	active = false;
}

HRESULT CrossAdapterBridge::CreateSharedFencePair(UINT64 a_initial, winrt::com_ptr<ID3D12Fence>& a_onA,
	winrt::com_ptr<ID3D12Fence>& a_onB)
{
	// Create on one GPU, open on the other. Render GPU -> secondary first; the other
	// direction is the one MGPU Bridge proved for AMD -> NVIDIA, kept as a fallback.
	const auto attempt = [a_initial](ID3D12Device* a_owner, ID3D12Device* a_other,
							 winrt::com_ptr<ID3D12Fence>& a_created, winrt::com_ptr<ID3D12Fence>& a_opened) {
		a_created = nullptr;
		a_opened = nullptr;
		HRESULT hr = a_owner->CreateFence(a_initial,
			D3D12_FENCE_FLAG_SHARED | D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER, IID_PPV_ARGS(a_created.put()));
		if (FAILED(hr))
			return hr;
		HANDLE handle = nullptr;
		hr = a_owner->CreateSharedHandle(a_created.get(), nullptr, GENERIC_ALL, nullptr, &handle);
		if (FAILED(hr))
			return hr;
		hr = a_other->OpenSharedHandle(handle, IID_PPV_ARGS(a_opened.put()));
		CloseHandle(handle);
		return hr;
	};
	HRESULT hr = attempt(deviceA.get(), deviceB.get(), a_onA, a_onB);
	if (FAILED(hr)) {
		REX::WARN("[FG-GPU1] cross-adapter fence render->secondary failed (hr={:#010x}), trying secondary->render",
			static_cast<std::uint32_t>(hr));
		hr = attempt(deviceB.get(), deviceA.get(), a_onB, a_onA);
	}
	return hr;
}

HRESULT CrossAdapterBridge::CreateSharedHeapPair(UINT64 a_size, winrt::com_ptr<ID3D12Heap>& a_onA,
	winrt::com_ptr<ID3D12Heap>& a_onB)
{
	D3D12_HEAP_DESC heapDesc{};
	heapDesc.SizeInBytes = a_size;
	heapDesc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
	heapDesc.Alignment = kHeapAlignment;
	heapDesc.Flags = D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER;

	const auto attempt = [&heapDesc](ID3D12Device* a_owner, ID3D12Device* a_other,
							 winrt::com_ptr<ID3D12Heap>& a_created, winrt::com_ptr<ID3D12Heap>& a_opened) {
		a_created = nullptr;
		a_opened = nullptr;
		HRESULT hr = a_owner->CreateHeap(&heapDesc, IID_PPV_ARGS(a_created.put()));
		if (FAILED(hr))
			return hr;
		HANDLE handle = nullptr;
		hr = a_owner->CreateSharedHandle(a_created.get(), nullptr, GENERIC_ALL, nullptr, &handle);
		if (FAILED(hr))
			return hr;
		hr = a_other->OpenSharedHandle(handle, IID_PPV_ARGS(a_opened.put()));
		CloseHandle(handle);
		return hr;
	};
	HRESULT hr = attempt(deviceA.get(), deviceB.get(), a_onA, a_onB);
	if (FAILED(hr)) {
		REX::WARN("[FG-GPU1] cross-adapter heap render->secondary failed (hr={:#010x}), trying secondary->render",
			static_cast<std::uint32_t>(hr));
		hr = attempt(deviceB.get(), deviceA.get(), a_onB, a_onA);
	}
	return hr;
}

HRESULT CrossAdapterBridge::WaitCpu(ID3D12Fence* a_fence, UINT64 a_value)
{
	if (a_fence->GetCompletedValue() >= a_value)
		return S_OK;
	++statCpuWaits;
	HRESULT hr = a_fence->SetEventOnCompletion(a_value, fenceEvent);
	if (FAILED(hr))
		return hr;
	if (WaitForSingleObject(fenceEvent, 2000) != WAIT_OBJECT_0)
		return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
	return S_OK;
}

bool CrossAdapterBridge::Initialize(ID3D12Device* a_deviceA, ID3D12Device* a_deviceB,
	ID3D11Device5* a_device11, ID3D11DeviceContext4* a_context11)
{
	Shutdown();
	if (!a_deviceA || !a_deviceB || !a_device11 || !a_context11)
		return false;

	deviceA.copy_from(a_deviceA);
	deviceB.copy_from(a_deviceB);
	context11.copy_from(a_context11);
	active = true;

	const auto check = [this](HRESULT a_hr, const char* a_what) {
		if (FAILED(a_hr))
			Fail(a_what, a_hr);
		return SUCCEEDED(a_hr);
	};

	D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
	if (SUCCEEDED(deviceB->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options))))
		REX::INFO("[FG-GPU1] secondary device: CrossNodeSharingTier={} CrossAdapterRowMajorTextureSupported={}",
			static_cast<int>(options.CrossNodeSharingTier), options.CrossAdapterRowMajorTextureSupported ? 1 : 0);

	D3D12_COMMAND_QUEUE_DESC queueDesc{};
	queueDesc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
	queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
	if (!check(deviceA->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(copyQueueA.put())), "copy queue on the render GPU"))
		return false;
	copyQueueA->SetName(L"FG-GPU1 transfer (render GPU copy queue)");

	for (auto& allocator : allocatorsA) {
		if (!check(deviceA->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(allocator.put())),
				"copy allocator"))
			return false;
	}
	if (!check(deviceA->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, allocatorsA[0].get(), nullptr,
				   IID_PPV_ARGS(listA.put())),
			"copy command list"))
		return false;
	listA->Close();

	if (!check(CreateSharedFencePair(0, transferFenceA, transferFenceB), "cross-adapter transfer fence"))
		return false;
	if (!check(CreateSharedFencePair(0, consumedFenceA, consumedFenceB), "cross-adapter consumed fence"))
		return false;

	if (!check(deviceA->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(copyDoneFenceA.put())), "copy-done fence"))
		return false;
	{
		HANDLE handle = nullptr;
		if (!check(deviceA->CreateSharedHandle(copyDoneFenceA.get(), nullptr, GENERIC_ALL, nullptr, &handle),
				"copy-done fence handle"))
			return false;
		const HRESULT hr = a_device11->OpenSharedFence(handle, IID_PPV_ARGS(copyDoneFence11.put()));
		CloseHandle(handle);
		if (!check(hr, "opening the copy-done fence on D3D11"))
			return false;
	}

	fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	if (!fenceEvent) {
		Fail("CreateEvent", HRESULT_FROM_WIN32(GetLastError()));
		return false;
	}

	// Copy-queue timestamps are optional; the transport works without them.
	D3D12_FEATURE_DATA_D3D12_OPTIONS3 options3{};
	if (SUCCEEDED(deviceA->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3, &options3, sizeof(options3))) &&
		options3.CopyQueueTimestampQueriesSupported &&
		SUCCEEDED(copyQueueA->GetTimestampFrequency(&timestampFrequency)) && timestampFrequency) {
		D3D12_QUERY_HEAP_DESC heapDesc{};
		heapDesc.Type = D3D12_QUERY_HEAP_TYPE_COPY_QUEUE_TIMESTAMP;
		heapDesc.Count = kSlots * 2;
		D3D12_HEAP_PROPERTIES readbackProps{};
		readbackProps.Type = D3D12_HEAP_TYPE_READBACK;
		D3D12_RESOURCE_DESC bufferDesc{};
		bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		bufferDesc.Width = sizeof(UINT64) * kSlots * 2;
		bufferDesc.Height = 1;
		bufferDesc.DepthOrArraySize = 1;
		bufferDesc.MipLevels = 1;
		bufferDesc.SampleDesc.Count = 1;
		bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		if (FAILED(deviceA->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(timestampHeap.put()))) ||
			FAILED(deviceA->CreateCommittedResource(&readbackProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
				D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(timestampReadback.put())))) {
			timestampHeap = nullptr;
			timestampReadback = nullptr;
		}
	}
	if (!timestampHeap)
		REX::INFO("[FG-GPU1] copy-queue timestamps unavailable; copy time will not be reported");

	QueryPerformanceFrequency(&qpcFrequency);
	QueryPerformanceCounter(&statLastLog);

	REX::INFO("[FG-GPU1] bridge ready: render GPU copy queue -> cross-adapter heap -> secondary GPU");
	return true;
}

int CrossAdapterBridge::AddLane(const char* a_name, ID3D12Resource* a_sourceSlot0, ID3D12Resource* a_sourceSlot1,
	bool a_createMirror)
{
	if (!active)
		return -1;
	if (!a_sourceSlot0 || !a_sourceSlot1) {
		Fail("AddLane (null source)", E_POINTER);
		return -1;
	}

	Lane lane;
	lane.name = a_name;
	lane.sourceA[0].copy_from(a_sourceSlot0);
	lane.sourceA[1].copy_from(a_sourceSlot1);
	lane.desc = a_sourceSlot0->GetDesc();

	if (lane.desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || lane.desc.MipLevels != 1 ||
		lane.desc.DepthOrArraySize != 1 || lane.desc.SampleDesc.Count != 1) {
		REX::CRITICAL("[FG-GPU1] lane '{}' is not a single-subresource 2D texture", lane.name);
		Fail("AddLane (unsupported texture)", E_INVALIDARG);
		return -1;
	}

	D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprintA{};
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprintB{};
	UINT64 totalA = 0;
	UINT64 totalB = 0;
	deviceA->GetCopyableFootprints(&lane.desc, 0, 1, 0, &footprintA, nullptr, nullptr, &totalA);
	deviceB->GetCopyableFootprints(&lane.desc, 0, 1, 0, &footprintB, nullptr, nullptr, &totalB);
	if (footprintA.Footprint.RowPitch != footprintB.Footprint.RowPitch || totalA != totalB || totalA == 0) {
		REX::CRITICAL("[FG-GPU1] lane '{}': footprints differ between GPUs (pitch {} vs {}, size {} vs {})",
			lane.name, footprintA.Footprint.RowPitch, footprintB.Footprint.RowPitch, totalA, totalB);
		Fail("AddLane (footprint mismatch)", E_FAIL);
		return -1;
	}
	lane.footprint = footprintA;
	lane.bytes = totalA;
	lane.slotStride = AlignUp(totalA, kHeapAlignment);

	const UINT64 heapSize = lane.slotStride * kSlots;
	HRESULT hr = CreateSharedHeapPair(heapSize, lane.heapA, lane.heapB);
	if (FAILED(hr)) {
		Fail("cross-adapter heap (CreateHeap SHARED | SHARED_CROSS_ADAPTER / OpenSharedHandle)", hr);
		return -1;
	}

	D3D12_RESOURCE_DESC bufferDesc{};
	bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	bufferDesc.Width = heapSize;
	bufferDesc.Height = 1;
	bufferDesc.DepthOrArraySize = 1;
	bufferDesc.MipLevels = 1;
	bufferDesc.SampleDesc.Count = 1;
	bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	bufferDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;

	hr = deviceA->CreatePlacedResource(lane.heapA.get(), 0, &bufferDesc, D3D12_RESOURCE_STATE_COMMON, nullptr,
		IID_PPV_ARGS(lane.stagingA.put()));
	if (FAILED(hr)) {
		Fail("CreatePlacedResource(staging) on the render GPU", hr);
		return -1;
	}
	hr = deviceB->CreatePlacedResource(lane.heapB.get(), 0, &bufferDesc, D3D12_RESOURCE_STATE_COMMON, nullptr,
		IID_PPV_ARGS(lane.stagingB.put()));
	if (FAILED(hr)) {
		Fail("CreatePlacedResource(staging) on the secondary GPU", hr);
		return -1;
	}

	if (a_createMirror) {
		D3D12_RESOURCE_DESC mirrorDesc = lane.desc;
		mirrorDesc.Alignment = 0;
		mirrorDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		mirrorDesc.Flags &= ~(D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER);
		D3D12_HEAP_PROPERTIES defaultHeap{};
		defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
		for (UINT slot = 0; slot < kSlots; ++slot) {
			hr = deviceB->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &mirrorDesc,
				D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(lane.mirrorB[slot].put()));
			if (FAILED(hr)) {
				Fail("CreateCommittedResource(mirror) on the secondary GPU", hr);
				return -1;
			}
		}
	}

	bytesPerFrame += lane.bytes;
	REX::INFO("[FG-GPU1] lane[{}] '{}' {}x{} format={} pitch={} bytes={} ({:.1f} MB/frame total){}",
		lanes.size(), lane.name, lane.desc.Width, lane.desc.Height, static_cast<int>(lane.desc.Format),
		lane.footprint.Footprint.RowPitch, lane.bytes, static_cast<double>(bytesPerFrame) / (1024.0 * 1024.0),
		a_createMirror ? "" : " (unpacked into caller's destination)");

	lanes.push_back(std::move(lane));
	return static_cast<int>(lanes.size() - 1);
}

ID3D12Resource* CrossAdapterBridge::Mirror(int a_lane, UINT a_slot) const noexcept
{
	if (a_lane < 0 || static_cast<size_t>(a_lane) >= lanes.size() || a_slot >= kSlots)
		return nullptr;
	return lanes[static_cast<size_t>(a_lane)].mirrorB[a_slot].get();
}

HRESULT CrossAdapterBridge::Transfer(ID3D12Fence* a_producerFenceA, UINT64 a_producerValue, UINT a_slot,
	ID3D12CommandQueue* a_queueB)
{
	if (!active)
		return E_FAIL;
	if (a_slot >= kSlots || !a_producerFenceA || !a_queueB) {
		Fail("Transfer (bad arguments)", E_INVALIDARG);
		return E_INVALIDARG;
	}

	LARGE_INTEGER submitStart{};
	QueryPerformanceCounter(&submitStart);

	// The allocator and timestamp pair of this slot were last used by slotTransfer[a_slot].
	const UINT64 previous = slotTransfer[a_slot];
	if (previous) {
		HRESULT hr = WaitCpu(copyDoneFenceA.get(), previous);
		if (FAILED(hr)) {
			Fail("waiting for the previous copy of this slot", hr);
			return hr;
		}
		if (timestampHeap) {
			const D3D12_RANGE range{ sizeof(UINT64) * a_slot * 2, sizeof(UINT64) * (a_slot * 2 + 2) };
			void* mapped = nullptr;
			if (SUCCEEDED(timestampReadback->Map(0, &range, &mapped))) {
				const auto* ticks = reinterpret_cast<const UINT64*>(static_cast<const std::uint8_t*>(mapped) + range.Begin);
				if (ticks[1] > ticks[0]) {
					const double ms = static_cast<double>(ticks[1] - ticks[0]) * 1000.0 /
					                  static_cast<double>(timestampFrequency);
					statGpuCopyMs += ms;
					statGpuCopyMaxMs = (std::max)(statGpuCopyMaxMs, ms);
					++statGpuCopySamples;
				}
				const D3D12_RANGE nothingWritten{ 0, 0 };
				timestampReadback->Unmap(0, &nothingWritten);
			}
		}
	}

	const UINT64 value = ++transferValue;
	auto* allocator = allocatorsA[a_slot].get();
	HRESULT hr = allocator->Reset();
	if (SUCCEEDED(hr))
		hr = listA->Reset(allocator, nullptr);
	if (FAILED(hr)) {
		Fail("resetting the copy list", hr);
		return hr;
	}

	if (timestampHeap)
		listA->EndQuery(timestampHeap.get(), D3D12_QUERY_TYPE_TIMESTAMP, a_slot * 2);

	for (auto& lane : lanes) {
		D3D12_TEXTURE_COPY_LOCATION dst{};
		dst.pResource = lane.stagingA.get();
		dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		dst.PlacedFootprint = lane.footprint;
		dst.PlacedFootprint.Offset = lane.slotStride * a_slot;

		D3D12_TEXTURE_COPY_LOCATION src{};
		src.pResource = lane.sourceA[a_slot].get();
		src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		src.SubresourceIndex = 0;

		// Sources are D3D11-shared textures in COMMON; a copy queue promotes them to
		// COPY_SOURCE implicitly and they decay back when the list completes.
		listA->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
	}

	if (timestampHeap) {
		listA->EndQuery(timestampHeap.get(), D3D12_QUERY_TYPE_TIMESTAMP, a_slot * 2 + 1);
		listA->ResolveQueryData(timestampHeap.get(), D3D12_QUERY_TYPE_TIMESTAMP, a_slot * 2, 2,
			timestampReadback.get(), sizeof(UINT64) * a_slot * 2);
	}

	hr = listA->Close();
	if (FAILED(hr)) {
		Fail("closing the copy list", hr);
		return hr;
	}

	// The game's D3D11 work for this frame (the producer of every source).
	hr = copyQueueA->Wait(a_producerFenceA, a_producerValue);
	// The secondary GPU finished reading this slot's staging area the last time round.
	if (SUCCEEDED(hr) && previous)
		hr = copyQueueA->Wait(consumedFenceA.get(), previous);
	if (FAILED(hr)) {
		Fail("copy queue Wait", hr);
		return hr;
	}

	ID3D12CommandList* lists[] = { listA.get() };
	copyQueueA->ExecuteCommandLists(1, lists);

	hr = copyQueueA->Signal(transferFenceA.get(), value);
	if (SUCCEEDED(hr))
		hr = copyQueueA->Signal(copyDoneFenceA.get(), value);
	if (SUCCEEDED(hr))
		hr = a_queueB->Wait(transferFenceB.get(), value);
	if (FAILED(hr)) {
		Fail("transfer fence Signal/Wait", hr);
		return hr;
	}

	slotTransfer[a_slot] = value;

	LARGE_INTEGER submitEnd{};
	QueryPerformanceCounter(&submitEnd);
	statCpuSubmitMs += QpcMs(submitStart, submitEnd, qpcFrequency);
	++statFrames;
	const UINT64 consumed = consumedFenceA->GetCompletedValue();
	statMaxLag = (std::max)(statMaxLag, value > consumed ? value - consumed : 0);
	return S_OK;
}

void CrossAdapterBridge::RecordUnpack(ID3D12GraphicsCommandList* a_listB, UINT a_slot)
{
	if (!active || !a_listB || a_slot >= kSlots)
		return;

	std::vector<D3D12_RESOURCE_BARRIER> before;
	std::vector<D3D12_RESOURCE_BARRIER> after;
	for (auto& lane : lanes) {
		if (auto* mirror = lane.mirrorB[a_slot].get()) {
			before.push_back(Transition(mirror, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST));
			after.push_back(Transition(mirror, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON));
		}
	}
	if (before.empty())
		return;

	a_listB->ResourceBarrier(static_cast<UINT>(before.size()), before.data());
	for (auto& lane : lanes) {
		auto* mirror = lane.mirrorB[a_slot].get();
		if (!mirror)
			continue;
		D3D12_TEXTURE_COPY_LOCATION dst{};
		dst.pResource = mirror;
		dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		dst.SubresourceIndex = 0;

		D3D12_TEXTURE_COPY_LOCATION src{};
		src.pResource = lane.stagingB.get();
		src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		src.PlacedFootprint = lane.footprint;
		src.PlacedFootprint.Offset = lane.slotStride * a_slot;

		a_listB->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
	}
	a_listB->ResourceBarrier(static_cast<UINT>(after.size()), after.data());
}

void CrossAdapterBridge::RecordUnpackTo(ID3D12GraphicsCommandList* a_listB, int a_lane, UINT a_slot,
	ID3D12Resource* a_destB, D3D12_RESOURCE_STATES a_destState)
{
	if (!active || !a_listB || !a_destB || a_slot >= kSlots || a_lane < 0 ||
		static_cast<size_t>(a_lane) >= lanes.size())
		return;
	auto& lane = lanes[static_cast<size_t>(a_lane)];

	const D3D12_RESOURCE_DESC destDesc = a_destB->GetDesc();
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT destFootprint{};
	deviceB->GetCopyableFootprints(&destDesc, 0, 1, 0, &destFootprint, nullptr, nullptr, nullptr);
	if (destDesc.Width != lane.desc.Width || destDesc.Height != lane.desc.Height ||
		destFootprint.Footprint.RowPitch != lane.footprint.Footprint.RowPitch) {
		// e.g. an HDR add-on promoted the swap chain to a 64-bit format.
		static bool logged = false;
		if (!logged) {
			REX::ERROR("[FG-GPU1] lane '{}' is {}x{} format {} but its destination is {}x{} format {}; skipping the copy",
				lane.name, lane.desc.Width, lane.desc.Height, static_cast<int>(lane.desc.Format), destDesc.Width,
				destDesc.Height, static_cast<int>(destDesc.Format));
			logged = true;
		}
		return;
	}

	auto barrier = Transition(a_destB, a_destState, D3D12_RESOURCE_STATE_COPY_DEST);
	a_listB->ResourceBarrier(1, &barrier);

	D3D12_TEXTURE_COPY_LOCATION dst{};
	dst.pResource = a_destB;
	dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	dst.SubresourceIndex = 0;

	D3D12_TEXTURE_COPY_LOCATION src{};
	src.pResource = lane.stagingB.get();
	src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	src.PlacedFootprint = lane.footprint;
	src.PlacedFootprint.Offset = lane.slotStride * a_slot;
	// Same width/height; a typeless/UNORM/SRGB difference between the back buffer and the
	// D3D11 texture is allowed by CopyTextureRegion as long as the formats are copy-compatible.
	src.PlacedFootprint.Footprint.Format = destDesc.Format;

	a_listB->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

	barrier = Transition(a_destB, D3D12_RESOURCE_STATE_COPY_DEST, a_destState);
	a_listB->ResourceBarrier(1, &barrier);
}

HRESULT CrossAdapterBridge::SignalConsumed(ID3D12CommandQueue* a_queueB)
{
	if (!active || !a_queueB)
		return E_FAIL;
	const HRESULT hr = a_queueB->Signal(consumedFenceB.get(), transferValue);
	if (FAILED(hr))
		Fail("consumed fence Signal", hr);
	return hr;
}

HRESULT CrossAdapterBridge::GateD3D11(UINT a_nextSlot)
{
	if (!active || a_nextSlot >= kSlots || !slotTransfer[a_nextSlot])
		return S_OK;
	const HRESULT hr = context11->Wait(copyDoneFence11.get(), slotTransfer[a_nextSlot]);
	if (FAILED(hr))
		Fail("D3D11 Wait on the copy-done fence", hr);
	return hr;
}

void CrossAdapterBridge::LogStats()
{
	if (!active || !statFrames)
		return;
	LARGE_INTEGER now{};
	QueryPerformanceCounter(&now);
	const double elapsedMs = QpcMs(statLastLog, now, qpcFrequency);
	if (elapsedMs < 10000.0)
		return;

	const double fps = static_cast<double>(statFrames) * 1000.0 / elapsedMs;
	const double mbPerFrame = static_cast<double>(bytesPerFrame) / (1024.0 * 1024.0);
	if (statGpuCopySamples) {
		const double avgCopy = statGpuCopyMs / static_cast<double>(statGpuCopySamples);
		REX::INFO("[FG-GPU1] {:.1f} game fps | {:.1f} MB/frame = {:.2f} GB/s | copy on render GPU avg {:.2f} ms max {:.2f} ms "
				  "(~{:.2f} GB/s link) | submit {:.3f} ms | max lag {} | cpu waits {}",
			fps, mbPerFrame, mbPerFrame * fps / 1024.0, avgCopy, statGpuCopyMaxMs,
			avgCopy > 0.0 ? mbPerFrame / 1024.0 / (avgCopy / 1000.0) : 0.0,
			statCpuSubmitMs / static_cast<double>(statFrames), statMaxLag, statCpuWaits);
	} else {
		REX::INFO("[FG-GPU1] {:.1f} game fps | {:.1f} MB/frame = {:.2f} GB/s | submit {:.3f} ms | max lag {} | cpu waits {}",
			fps, mbPerFrame, mbPerFrame * fps / 1024.0, statCpuSubmitMs / static_cast<double>(statFrames), statMaxLag,
			statCpuWaits);
	}

	statFrames = 0;
	statGpuCopyMs = 0.0;
	statGpuCopySamples = 0;
	statGpuCopyMaxMs = 0.0;
	statCpuSubmitMs = 0.0;
	statMaxLag = 0;
	statCpuWaits = 0;
	statLastLog = now;
}

void CrossAdapterBridge::Shutdown() noexcept
{
	if (copyQueueA && copyDoneFenceA && fenceEvent && transferValue)
		WaitCpu(copyDoneFenceA.get(), transferValue);
	active = false;
	lanes.clear();
	bytesPerFrame = 0;
	transferValue = 0;
	slotTransfer = {};
	timestampHeap = nullptr;
	timestampReadback = nullptr;
	listA = nullptr;
	for (auto& allocator : allocatorsA)
		allocator = nullptr;
	copyQueueA = nullptr;
	transferFenceA = nullptr;
	transferFenceB = nullptr;
	consumedFenceA = nullptr;
	consumedFenceB = nullptr;
	copyDoneFenceA = nullptr;
	copyDoneFence11 = nullptr;
	if (fenceEvent) {
		CloseHandle(fenceEvent);
		fenceEvent = nullptr;
	}
	context11 = nullptr;
	deviceA = nullptr;
	deviceB = nullptr;
}
