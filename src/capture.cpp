#include "capture.h"
#include "desktop.h"
#include "log.h"

#include <d3d11_4.h>

namespace pd {

static std::string narrow(const wchar_t *w) {
	int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
	std::string s(n > 0 ? n - 1 : 0, '\0');
	if (n > 1)
		WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
	return s;
}

namespace {
struct FoundOutput {
	OutputInfo info;
	ComPtr<IDXGIAdapter1> adapter;
	ComPtr<IDXGIOutput> output;
};

std::vector<FoundOutput> enumerate() {
	std::vector<FoundOutput> list;
	ComPtr<IDXGIFactory1> factory;
	if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
		return list;
	ComPtr<IDXGIAdapter1> adapter;
	for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
		DXGI_ADAPTER_DESC1 ad{};
		adapter->GetDesc1(&ad);
		ComPtr<IDXGIOutput> output;
		for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
			DXGI_OUTPUT_DESC od{};
			output->GetDesc(&od);
			if (!od.AttachedToDesktop)
				continue;
			FoundOutput f;
			f.info.index = int(list.size());
			f.info.adapterName = narrow(ad.Description);
			f.info.deviceName = narrow(od.DeviceName);
			f.info.vendorId = ad.VendorId;
			f.info.rect = od.DesktopCoordinates;
			f.info.primary = od.DesktopCoordinates.left == 0 && od.DesktopCoordinates.top == 0;
			f.adapter = adapter;
			f.output = output;
			list.push_back(std::move(f));
		}
	}
	return list;
}
} // namespace

std::vector<OutputInfo> listOutputs() {
	std::vector<OutputInfo> result;
	for (auto &f : enumerate())
		result.push_back(f.info);
	return result;
}

bool DesktopCapture::open(int outputIndex) {
	close();
	auto outputs = enumerate();
	const FoundOutput *chosen = nullptr;
	for (auto &f : outputs) {
		if ((outputIndex < 0 && f.info.primary) || f.info.index == outputIndex) {
			chosen = &f;
			break;
		}
	}
	if (!chosen) {
		// 一个显示器都没有时由调用方记日志（会反复重试，这里不刷屏）
		if (!outputs.empty())
			log("找不到显示器 {}", outputIndex);
		return false;
	}
	mAdapter = chosen->adapter;
	mOutput = chosen->output;
	vendorId = chosen->info.vendorId;
	adapterName = chosen->info.adapterName;
	this->outputIndex = chosen->info.index; // 参数同名，必须写 this->
	isPrimary = chosen->info.primary;
	desktopRect = chosen->info.rect;
	mPointer = PointerState{};

	const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
	// VIDEO_SUPPORT：FFmpeg 的 D3D11 硬件上下文和视频处理器都需要
	UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
	HRESULT hr = D3D11CreateDevice(mAdapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels,
	                               ARRAYSIZE(levels), D3D11_SDK_VERSION, &mDevice, nullptr,
	                               &mContext);
	if (FAILED(hr)) {
		log("D3D11CreateDevice 失败 {}", hexhr(hr));
		return false;
	}
	ComPtr<ID3D11Multithread> mt;
	if (SUCCEEDED(mContext.As(&mt)))
		mt->SetMultithreadProtected(TRUE);

	log("截屏显示器 {}（{}），显卡 {}", chosen->info.index, chosen->info.deviceName, adapterName);
	return reopenDuplication();
}

bool DesktopCapture::reopenDuplication() {
	mDup.Reset();
	// 桌面切换（锁屏、UAC、回到桌面）后复制会失效，要在新桌面上重新开始；只有 SYSTEM 能切到 Winlogon
	syncThreadDesktop();
	HRESULT hr = E_FAIL;
	ComPtr<IDXGIOutput5> out5;
	if (SUCCEEDED(mOutput.As(&out5))) {
		const DXGI_FORMAT formats[] = {DXGI_FORMAT_B8G8R8A8_UNORM};
		hr = out5->DuplicateOutput1(mDevice.Get(), 0, ARRAYSIZE(formats), formats, &mDup);
	}
	if (!mDup) {
		ComPtr<IDXGIOutput1> out1;
		if (SUCCEEDED(mOutput.As(&out1)))
			hr = out1->DuplicateOutput(mDevice.Get(), &mDup);
	}
	if (!mDup) {
		log("DuplicateOutput 失败 {}", hexhr(hr));
		return false;
	}
	DXGI_OUTDUPL_DESC dd{};
	mDup->GetDesc(&dd);
	width = int(dd.ModeDesc.Width);
	height = int(dd.ModeDesc.Height);
	log("桌面复制已开始 {}x{}，刷新率 {}/{}", width, height, dd.ModeDesc.RefreshRate.Numerator,
	    dd.ModeDesc.RefreshRate.Denominator);
	return true;
}

void DesktopCapture::close() {
	mDup.Reset();
	mFrame.Reset();
	mContext.Reset();
	mDevice.Reset();
	mOutput.Reset();
	mAdapter.Reset();
}

DesktopCapture::Result DesktopCapture::acquire(UINT timeoutMs, int64_t &presentQpc,
                                               bool &pointerChanged) {
	pointerChanged = false;
	if (!mDup)
		return Result::Lost;
	DXGI_OUTDUPL_FRAME_INFO info{};
	ComPtr<IDXGIResource> resource;
	HRESULT hr = mDup->AcquireNextFrame(timeoutMs, &info, &resource);
	if (hr == DXGI_ERROR_WAIT_TIMEOUT)
		return Result::Timeout;
	if (FAILED(hr)) {
		log("AcquireNextFrame 失败 {}，重新建立桌面复制", hexhr(hr));
		mDup.Reset();
		return Result::Lost;
	}

	// 指针：位置只在 LastMouseUpdateTime 非 0 时有效；Visible 为假表示指针不在这块屏上或被隐藏
	if (info.LastMouseUpdateTime.QuadPart != 0) {
		const bool visible = info.PointerPosition.Visible != FALSE;
		const POINT pos = info.PointerPosition.Position;
		if (visible != mPointer.visible ||
		    (visible && (pos.x != mPointer.pos.x || pos.y != mPointer.pos.y)))
			pointerChanged = true;
		mPointer.visible = visible;
		if (visible)
			mPointer.pos = pos;
	}
	if (info.PointerShapeBufferSize > 0) {
		mPointer.shape.resize(info.PointerShapeBufferSize);
		UINT required = 0;
		hr = mDup->GetFramePointerShape(UINT(mPointer.shape.size()), mPointer.shape.data(), &required,
		                                &mPointer.shapeInfo);
		if (SUCCEEDED(hr)) {
			++mPointer.shapeVersion;
			pointerChanged = true;
		}
	}

	Result result = Result::Timeout;
	// LastPresentTime 为 0 表示只有鼠标指针变化，画面没变
	if (info.LastPresentTime.QuadPart != 0) {
		ComPtr<ID3D11Texture2D> tex;
		if (SUCCEEDED(resource.As(&tex))) {
			D3D11_TEXTURE2D_DESC src{};
			tex->GetDesc(&src);
			D3D11_TEXTURE2D_DESC cur{};
			if (mFrame)
				mFrame->GetDesc(&cur);
			if (!mFrame || cur.Width != src.Width || cur.Height != src.Height ||
			    cur.Format != src.Format) {
				D3D11_TEXTURE2D_DESC d{};
				d.Width = src.Width;
				d.Height = src.Height;
				d.MipLevels = 1;
				d.ArraySize = 1;
				d.Format = src.Format;
				d.SampleDesc.Count = 1;
				d.Usage = D3D11_USAGE_DEFAULT;
				d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
				mFrame.Reset();
				hr = mDevice->CreateTexture2D(&d, nullptr, &mFrame);
				if (FAILED(hr))
					log("创建画面纹理失败 {}", hexhr(hr));
			}
			if (mFrame) {
				mContext->CopyResource(mFrame.Get(), tex.Get());
				presentQpc = info.LastPresentTime.QuadPart;
				result = Result::Frame;
			}
		}
	}
	mDup->ReleaseFrame();
	return result;
}

bool Nv12Converter::init(ID3D11Device *device, ID3D11DeviceContext *context, int inW, int inH,
                         int outW, int outH) {
	reset();
	if (FAILED(device->QueryInterface(IID_PPV_ARGS(&mVideoDevice))) ||
	    FAILED(context->QueryInterface(IID_PPV_ARGS(&mVideoContext)))) {
		log("显卡不支持 D3D11 视频接口");
		return false;
	}
	D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
	cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
	cd.InputWidth = UINT(inW);
	cd.InputHeight = UINT(inH);
	cd.OutputWidth = UINT(outW);
	cd.OutputHeight = UINT(outH);
	cd.InputFrameRate = {60, 1};
	cd.OutputFrameRate = {60, 1};
	cd.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
	HRESULT hr = mVideoDevice->CreateVideoProcessorEnumerator(&cd, &mEnum);
	if (SUCCEEDED(hr))
		hr = mVideoDevice->CreateVideoProcessor(mEnum.Get(), 0, &mProcessor);
	if (FAILED(hr)) {
		log("创建视频处理器失败 {}", hexhr(hr));
		return false;
	}

	auto *vp = mProcessor.Get();
	ComPtr<ID3D11VideoContext1> vc1;
	if (SUCCEEDED(mVideoContext.As(&vc1))) {
		vc1->VideoProcessorSetStreamColorSpace1(vp, 0, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
		vc1->VideoProcessorSetOutputColorSpace1(vp, DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
	} else {
		D3D11_VIDEO_PROCESSOR_COLOR_SPACE in{}, out{};
		in.RGB_Range = 0; // 0-255
		out.YCbCr_Matrix = 1; // BT.709
		out.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
		mVideoContext->VideoProcessorSetStreamColorSpace(vp, 0, &in);
		mVideoContext->VideoProcessorSetOutputColorSpace(vp, &out);
	}
	mVideoContext->VideoProcessorSetStreamFrameFormat(vp, 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
	mVideoContext->VideoProcessorSetStreamAutoProcessingMode(vp, 0, FALSE);
	// 编码器的纹理可能比画面大（Intel QSV 按 16 对齐，1080 变 1088），只画到左上角 outW×outH，
	// 不然画面会被拉伸，编码器再裁掉多出来的部分
	mOutRect = RECT{0, 0, outW, outH};
	mVideoContext->VideoProcessorSetOutputTargetRect(vp, TRUE, &mOutRect);
	mVideoContext->VideoProcessorSetStreamDestRect(vp, 0, TRUE, &mOutRect);
	return true;
}

bool Nv12Converter::convert(ID3D11Texture2D *src, ID3D11Texture2D *dst, UINT dstSlice) {
	auto in = mInputViews.find(src);
	if (in == mInputViews.end()) {
		D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC d{};
		d.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
		ComPtr<ID3D11VideoProcessorInputView> view;
		HRESULT hr = mVideoDevice->CreateVideoProcessorInputView(src, mEnum.Get(), &d, &view);
		if (FAILED(hr)) {
			log("CreateVideoProcessorInputView 失败 {}", hexhr(hr));
			return false;
		}
		in = mInputViews.emplace(src, std::make_pair(ComPtr<ID3D11Texture2D>(src), view)).first;
	}

	auto key = std::make_pair(dst, dstSlice);
	auto out = mOutputViews.find(key);
	if (out == mOutputViews.end()) {
		D3D11_TEXTURE2D_DESC td{};
		dst->GetDesc(&td);
		D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC d{};
		if (td.ArraySize > 1) {
			d.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2DARRAY;
			d.Texture2DArray.FirstArraySlice = dstSlice;
			d.Texture2DArray.ArraySize = 1;
		} else {
			d.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
		}
		ComPtr<ID3D11VideoProcessorOutputView> view;
		HRESULT hr = mVideoDevice->CreateVideoProcessorOutputView(dst, mEnum.Get(), &d, &view);
		if (FAILED(hr)) {
			log("CreateVideoProcessorOutputView 失败 {}", hexhr(hr));
			return false;
		}
		out = mOutputViews.emplace(key, std::make_pair(ComPtr<ID3D11Texture2D>(dst), view)).first;
	}

	D3D11_VIDEO_PROCESSOR_STREAM stream{};
	stream.Enable = TRUE;
	stream.pInputSurface = in->second.second.Get();
	HRESULT hr = mVideoContext->VideoProcessorBlt(mProcessor.Get(), out->second.second.Get(), 0, 1,
	                                              &stream);
	if (FAILED(hr)) {
		log("VideoProcessorBlt 失败 {}", hexhr(hr));
		return false;
	}
	return true;
}

void Nv12Converter::reset() {
	mInputViews.clear();
	mOutputViews.clear();
	mProcessor.Reset();
	mEnum.Reset();
	mVideoContext.Reset();
	mVideoDevice.Reset();
}

} // namespace pd
