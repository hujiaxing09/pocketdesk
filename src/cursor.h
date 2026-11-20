#pragma once
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cstdint>
#include <map>
#include <vector>

namespace pd {

using Microsoft::WRL::ComPtr;

// 桌面复制拿到的画面不含鼠标指针，需要自己画上去。做法参考 Sunshine：
// 把指针形状拆成两张纹理，一张按透明度混合，一张做反色（例如文本输入时的 I 形光标），
// 用显卡画到画面的副本上，再交给编码器。
class CursorRenderer {
public:
	bool init(ID3D11Device *device, ID3D11DeviceContext *context);
	void reset();
	void setShape(const DXGI_OUTDUPL_POINTER_SHAPE_INFO &info, const std::vector<uint8_t> &buffer);
	bool hasShape() const { return mAlphaSrv != nullptr; }
	// 把 src 复制到 dst，再在 pos（指针左上角，相对画面）处画指针
	bool compose(ID3D11Texture2D *src, ID3D11Texture2D *dst, POINT pos);

private:
	bool createTexture(const std::vector<uint8_t> &bgra, int w, int h,
	                   ComPtr<ID3D11ShaderResourceView> &srv);
	void draw(ID3D11ShaderResourceView *srv, ID3D11BlendState *blend);

	ComPtr<ID3D11Device> mDevice;
	ComPtr<ID3D11DeviceContext> mContext;
	ComPtr<ID3D11VertexShader> mVs;
	ComPtr<ID3D11PixelShader> mPs;
	ComPtr<ID3D11Buffer> mRectBuffer;
	ComPtr<ID3D11SamplerState> mSampler;
	ComPtr<ID3D11RasterizerState> mRaster;
	ComPtr<ID3D11BlendState> mAlphaBlend;
	ComPtr<ID3D11BlendState> mInvertBlend;
	ComPtr<ID3D11ShaderResourceView> mAlphaSrv;
	ComPtr<ID3D11ShaderResourceView> mInvertSrv;
	std::map<ID3D11Texture2D *, std::pair<ComPtr<ID3D11Texture2D>, ComPtr<ID3D11RenderTargetView>>>
	    mTargets;
	int mWidth = 0, mHeight = 0;
};

} // namespace pd
