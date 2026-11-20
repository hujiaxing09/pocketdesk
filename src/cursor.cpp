#include "cursor.h"
#include "log.h"

#include <d3dcompiler.h>

#include <cstring>

namespace pd {

namespace {

const char *kShader = R"(
cbuffer Rect : register(b0) { float4 rect; }  // 左、上、右、下（NDC）
struct VsOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VsOut vs(uint id : SV_VertexID) {
	float2 t = float2(id & 1, id >> 1);
	VsOut o;
	o.pos = float4(lerp(rect.x, rect.z, t.x), lerp(rect.y, rect.w, t.y), 0, 1);
	o.uv = t;
	return o;
}
Texture2D tex : register(t0);
SamplerState smp : register(s0);
float4 ps(VsOut i) : SV_Target { return tex.Sample(smp, i.uv); }
)";

ComPtr<ID3DBlob> compile(const char *entry, const char *target) {
	ComPtr<ID3DBlob> code, errors;
	HRESULT hr = D3DCompile(kShader, strlen(kShader), "cursor", nullptr, nullptr, entry, target,
	                        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
	if (FAILED(hr)) {
		log("指针着色器编译失败 {}: {}", hexhr(hr),
		    errors ? static_cast<const char *>(errors->GetBufferPointer()) : "");
		return nullptr;
	}
	return code;
}

} // namespace

bool CursorRenderer::init(ID3D11Device *device, ID3D11DeviceContext *context) {
	reset();
	mDevice = device;
	mContext = context;
	auto vs = compile("vs", "vs_4_0");
	auto ps = compile("ps", "ps_4_0");
	if (!vs || !ps)
		return false;
	HRESULT hr = device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &mVs);
	if (SUCCEEDED(hr))
		hr = device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &mPs);

	D3D11_BUFFER_DESC bd{};
	bd.ByteWidth = 16;
	bd.Usage = D3D11_USAGE_DEFAULT;
	bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	if (SUCCEEDED(hr))
		hr = device->CreateBuffer(&bd, nullptr, &mRectBuffer);

	D3D11_SAMPLER_DESC sd{};
	sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
	sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	sd.MaxLOD = D3D11_FLOAT32_MAX;
	if (SUCCEEDED(hr))
		hr = device->CreateSamplerState(&sd, &mSampler);

	D3D11_RASTERIZER_DESC rd{};
	rd.FillMode = D3D11_FILL_SOLID;
	rd.CullMode = D3D11_CULL_NONE;
	rd.DepthClipEnable = TRUE;
	if (SUCCEEDED(hr))
		hr = device->CreateRasterizerState(&rd, &mRaster);

	// 透明度混合；目标的 alpha 保持不变
	D3D11_BLEND_DESC blend{};
	auto &rt = blend.RenderTarget[0];
	rt.BlendEnable = TRUE;
	rt.SrcBlend = D3D11_BLEND_SRC_ALPHA;
	rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
	rt.BlendOp = D3D11_BLEND_OP_ADD;
	rt.SrcBlendAlpha = D3D11_BLEND_ZERO;
	rt.DestBlendAlpha = D3D11_BLEND_ONE;
	rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
	rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	if (SUCCEEDED(hr))
		hr = device->CreateBlendState(&blend, &mAlphaBlend);
	// 反色：结果 = 源 × (1 - 目标) + 目标 × (1 - 源)，源为白色时就是把底色反过来
	rt.SrcBlend = D3D11_BLEND_INV_DEST_COLOR;
	rt.DestBlend = D3D11_BLEND_INV_SRC_COLOR;
	if (SUCCEEDED(hr))
		hr = device->CreateBlendState(&blend, &mInvertBlend);

	if (FAILED(hr)) {
		log("指针绘制初始化失败 {}", hexhr(hr));
		reset();
		return false;
	}
	return true;
}

void CursorRenderer::reset() {
	mTargets.clear();
	mAlphaSrv.Reset();
	mInvertSrv.Reset();
	mInvertBlend.Reset();
	mAlphaBlend.Reset();
	mRaster.Reset();
	mSampler.Reset();
	mRectBuffer.Reset();
	mPs.Reset();
	mVs.Reset();
	mContext.Reset();
	mDevice.Reset();
	mWidth = mHeight = 0;
}

bool CursorRenderer::createTexture(const std::vector<uint8_t> &bgra, int w, int h,
                                   ComPtr<ID3D11ShaderResourceView> &srv) {
	D3D11_TEXTURE2D_DESC td{};
	td.Width = UINT(w);
	td.Height = UINT(h);
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_IMMUTABLE;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	D3D11_SUBRESOURCE_DATA data{bgra.data(), UINT(w * 4), 0};
	ComPtr<ID3D11Texture2D> tex;
	HRESULT hr = mDevice->CreateTexture2D(&td, &data, &tex);
	if (SUCCEEDED(hr))
		hr = mDevice->CreateShaderResourceView(tex.Get(), nullptr, &srv);
	if (FAILED(hr)) {
		log("创建指针纹理失败 {}", hexhr(hr));
		return false;
	}
	return true;
}

void CursorRenderer::setShape(const DXGI_OUTDUPL_POINTER_SHAPE_INFO &info,
                              const std::vector<uint8_t> &buf) {
	if (!mDevice)
		return;
	mAlphaSrv.Reset();
	mInvertSrv.Reset();
	const int w = int(info.Width);
	const int h = info.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME ? int(info.Height / 2)
	                                                                       : int(info.Height);
	if (w <= 0 || h <= 0)
		return;
	std::vector<uint8_t> alpha(size_t(w) * h * 4, 0), invert(size_t(w) * h * 4, 0);
	bool anyInvert = false;
	auto put = [](std::vector<uint8_t> &img, size_t i, uint8_t b, uint8_t g, uint8_t r, uint8_t a) {
		img[i] = b;
		img[i + 1] = g;
		img[i + 2] = r;
		img[i + 3] = a;
	};

	for (int y = 0; y < h; ++y) {
		for (int x = 0; x < w; ++x) {
			const size_t o = (size_t(y) * w + x) * 4;
			if (info.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME) {
				const uint8_t bit = uint8_t(0x80 >> (x % 8));
				const bool andBit = buf[size_t(y) * info.Pitch + x / 8] & bit;
				const bool xorBit = buf[size_t(y + h) * info.Pitch + x / 8] & bit;
				if (!andBit)
					put(alpha, o, xorBit ? 255 : 0, xorBit ? 255 : 0, xorBit ? 255 : 0, 255);
				else if (xorBit) {
					put(invert, o, 255, 255, 255, 255);
					anyInvert = true;
				}
			} else {
				const uint8_t *p = &buf[size_t(y) * info.Pitch + size_t(x) * 4];
				if (info.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR) {
					put(alpha, o, p[0], p[1], p[2], p[3]);
				} else { // MASKED_COLOR：alpha 字节是掩码，0 表示直接用颜色，0xFF 表示与底色异或
					if (p[3] == 0)
						put(alpha, o, p[0], p[1], p[2], 255);
					else if (p[0] || p[1] || p[2]) {
						put(invert, o, p[0], p[1], p[2], 255);
						anyInvert = true;
					}
				}
			}
		}
	}
	createTexture(alpha, w, h, mAlphaSrv);
	if (anyInvert)
		createTexture(invert, w, h, mInvertSrv);
	mWidth = w;
	mHeight = h;
}

void CursorRenderer::draw(ID3D11ShaderResourceView *srv, ID3D11BlendState *blend) {
	const float factor[4] = {0, 0, 0, 0};
	mContext->OMSetBlendState(blend, factor, 0xFFFFFFFF);
	mContext->PSSetShaderResources(0, 1, &srv);
	mContext->Draw(4, 0);
}

bool CursorRenderer::compose(ID3D11Texture2D *src, ID3D11Texture2D *dst, POINT pos) {
	mContext->CopyResource(dst, src);
	if (!mAlphaSrv)
		return true;

	auto it = mTargets.find(dst);
	if (it == mTargets.end()) {
		ComPtr<ID3D11RenderTargetView> rtv;
		HRESULT hr = mDevice->CreateRenderTargetView(dst, nullptr, &rtv);
		if (FAILED(hr)) {
			log("创建指针渲染目标失败 {}", hexhr(hr));
			return false;
		}
		it = mTargets.emplace(dst, std::make_pair(ComPtr<ID3D11Texture2D>(dst), rtv)).first;
	}
	D3D11_TEXTURE2D_DESC td{};
	dst->GetDesc(&td);
	const float fw = float(td.Width), fh = float(td.Height);
	const float rect[4] = {pos.x / fw * 2 - 1, 1 - pos.y / fh * 2, (pos.x + mWidth) / fw * 2 - 1,
	                       1 - (pos.y + mHeight) / fh * 2};
	mContext->UpdateSubresource(mRectBuffer.Get(), 0, nullptr, rect, 0, 0);

	D3D11_VIEWPORT vp{0, 0, fw, fh, 0, 1};
	ID3D11RenderTargetView *rtv = it->second.second.Get();
	mContext->OMSetRenderTargets(1, &rtv, nullptr);
	mContext->RSSetViewports(1, &vp);
	mContext->RSSetState(mRaster.Get());
	mContext->IASetInputLayout(nullptr);
	mContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	mContext->VSSetShader(mVs.Get(), nullptr, 0);
	mContext->VSSetConstantBuffers(0, 1, mRectBuffer.GetAddressOf());
	mContext->PSSetShader(mPs.Get(), nullptr, 0);
	mContext->PSSetSamplers(0, 1, mSampler.GetAddressOf());

	draw(mAlphaSrv.Get(), mAlphaBlend.Get());
	if (mInvertSrv)
		draw(mInvertSrv.Get(), mInvertBlend.Get());

	ID3D11ShaderResourceView *nullSrv = nullptr;
	mContext->PSSetShaderResources(0, 1, &nullSrv);
	mContext->OMSetRenderTargets(0, nullptr, nullptr);
	return true;
}

} // namespace pd
