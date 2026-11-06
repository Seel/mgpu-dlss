#pragma once

#include "Buffer.h"

#include "SimpleIni.h"

class Upscaling
{
public:
	enum class FrameGenType : int { kFSR3 = 0, kDLSSG = 1, kXeSSFG = 2 };

	static Upscaling* GetSingleton()
	{
		static Upscaling singleton;
		return &singleton;
	}

	struct Settings
	{
		bool frameGenerationMode = true;
		bool frameLimitMode = true;
		bool disableInMenus = true;  ///< Disable frame generation in menus to avoid interpolation artifacts
		bool debugLogging = false;
		int frameGenType = 0;    // 0=FSR3, 1=DLSS-G, 2=XeSS-FG
		int frameGenFrames = 1;  // 1=2x, 2=3x, 3=4x (MFG, RTX 50+ only)
		bool secondaryGPU = false;       ///< Run FSR3 FG and present on a second GPU (the one driving the monitor)
		std::string secondaryGPUName;    ///< Optional: substring of the secondary GPU's name, if auto-selection is ambiguous
		bool sendHudlessFrame = true;    ///< Send the extra UI-free frame (off = less PCIe, HUD smears)
	};

	Settings settings;

	bool highFPSPhysicsFixLoaded = false;

	FrameGenType activeFrameGenType = FrameGenType::kFSR3;
	bool d3d12Interop = false;
	double refreshRate = 0.0f;

	Texture2D* HUDLessBufferShared[2];
	Texture2D* depthBufferShared[2];
	Texture2D* motionVectorBufferShared[2];
	Texture2D* UIColorAlphaShared[2];

	winrt::com_ptr<ID3D12Resource> HUDLessBufferShared12[2];
	winrt::com_ptr<ID3D12Resource> depthBufferShared12[2];
	winrt::com_ptr<ID3D12Resource> motionVectorBufferShared12[2];
	winrt::com_ptr<ID3D12Resource> UIColorAlphaShared12[2];

	ID3D11ComputeShader* copyDepthToSharedBufferCS = nullptr;
	ID3D11ComputeShader* generateSharedBuffersCS = nullptr;
	ID3D11ComputeShader* generateUIBufferCS = nullptr;

	bool setupBuffers = false;

	bool imagespaceComplete = false;

	void LoadSettings();
	void ReloadSettingsIfNeeded();

	void PostPostLoad();

	void CreateFrameGenerationResources();
	void PreAlpha();
	void PostAlpha();
	void CopyBuffersToSharedResources();
	void GenerateUIBuffer();

	static void TimerSleepQPC(int64_t targetQPC);

	void FrameLimiter(bool a_useFrameGeneration);

	void GameFrameLimiter();

	static double GetRefreshRate(HWND a_window);

	void PostDisplay();

	void Reset();

	static void InstallHooks();
};
