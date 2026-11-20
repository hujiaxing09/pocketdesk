#pragma once
#include <d3d11.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace pd {

using Microsoft::WRL::ComPtr;

struct OutputInfo {
	int index = 0;
	std::string adapterName;
	std::string deviceName;
	UINT vendorId = 0;
	RECT rect{};
	bool primary = false;
};

// 鼠标指针：位置相对于被截取的显示器；形状变化时 shapeVersion 加一
struct PointerState {
	bool visible = false;
	POINT pos{};
	uint64_t shapeVersion = 0;
	DXGI_OUTDUPL_POINTER_SHAPE_INFO shapeInfo{};
	std::vector<uint8_t> shape;
};

// 列出所有连接到桌面的显示器（按显卡、输出顺序编号）
std::vector<OutputInfo> listOutputs();

// DXGI Desktop Duplication：画面始终留在显卡里
class DesktopCapture {
public:
	enum class Result { Frame, Timeout, Lost };

	// outputIndex < 0 表示主显示器
	bool open(int outputIndex);
	void close();
	// 拿到新画面时把它复制到 frame()，presentQpc 是该画面的呈现时间（QPC）；
	// 指针移动或换形状时 pointerChanged 为 true（画面本身可能没变）
	Result acquire(UINT timeoutMs, int64_t &presentQpc, bool &pointerChanged);
	// Lost 之后调用；安全桌面（UAC、锁屏）期间会一直失败，需要隔一会儿重试
	bool reopenDuplication();

	ID3D11Device *device() const { return mDevice.Get(); }
	ID3D11DeviceContext *context() const { return mContext.Get(); }
	ID3D11Texture2D *frame() const { return mFrame.Get(); }
	bool hasDuplication() const { return mDup != nullptr; }

	const PointerState &pointer() const { return mPointer; }

	int width = 0, height = 0;
	int outputIndex = -1;
	bool isPrimary = false;
	RECT desktopRect{}; // 这块显示器在虚拟桌面上的位置（物理像素）
	UINT vendorId = 0;
	std::string adapterName;

private:
	ComPtr<IDXGIAdapter1> mAdapter;
	ComPtr<IDXGIOutput> mOutput;
	ComPtr<ID3D11Device> mDevice;
	ComPtr<ID3D11DeviceContext> mContext;
	ComPtr<IDXGIOutputDuplication> mDup;
	ComPtr<ID3D11Texture2D> mFrame;
	PointerState mPointer;
};

// 用 D3D11 视频处理器把 BGRA 转成 NV12（BT.709 有限范围），需要时顺便缩放
class Nv12Converter {
public:
	bool init(ID3D11Device *device, ID3D11DeviceContext *context, int inW, int inH, int outW,
	          int outH);
	bool convert(ID3D11Texture2D *src, ID3D11Texture2D *dst, UINT dstSlice);
	void reset();

private:
	ComPtr<ID3D11VideoDevice> mVideoDevice;
	ComPtr<ID3D11VideoContext> mVideoContext;
	ComPtr<ID3D11VideoProcessorEnumerator> mEnum;
	ComPtr<ID3D11VideoProcessor> mProcessor;
	std::map<ID3D11Texture2D *, std::pair<ComPtr<ID3D11Texture2D>, ComPtr<ID3D11VideoProcessorInputView>>>
	    mInputViews;
	std::map<std::pair<ID3D11Texture2D *, UINT>,
	         std::pair<ComPtr<ID3D11Texture2D>, ComPtr<ID3D11VideoProcessorOutputView>>>
	    mOutputViews;
};

} // namespace pd
