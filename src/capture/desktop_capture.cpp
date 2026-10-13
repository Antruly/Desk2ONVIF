#include "capture/desktop_capture.h"

#include "common/log.h"
#include "common/text.h"
#include "capture/display_info.h"

#include <cstring>

namespace d2o {

namespace {

/// AcquireNextFrame 里那个 IDXGIResource 其实是 ID3D11Texture2D。
/// 拿不到就说明不是我们认识的桌面复制实现，直接判失败比硬转安全。
bool as_texture(IDXGIResource* res, ID3D11Texture2D** out) {
  if (!res) return false;
  return SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D),
                                       reinterpret_cast<void**>(out)));
}

/// 中转纹理和桌面帧的尺寸/格式必须严格一致，否则 CopyResource 直接失败。
constexpr DXGI_FORMAT kDesktopFormat = DXGI_FORMAT_B8G8R8A8_UNORM;

/// DXGI 的形状信息 → 我们自己的形状结构。**类型不认识就返回 false**，
/// 而不是猜一个：猜错的表现是画出一块花花绿绿的东西，比不画指针难查得多。
bool to_cursor_shape(DXGI_OUTDUPL_POINTER_SHAPE_INFO const& si,
                     std::vector<std::uint8_t> const& bits, CursorShape& out) {
  switch (si.Type) {
    case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR:
      out.type = CursorShapeType::Color;
      break;
    case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME:
      out.type = CursorShapeType::Monochrome;
      break;
    case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR:
      out.type = CursorShapeType::MaskedColor;
      break;
    default:
      return false;
  }
  out.width = static_cast<int>(si.Width);
  out.height = static_cast<int>(si.Height);
  out.pitch = static_cast<int>(si.Pitch);
  out.hotspot_x = si.HotSpot.x;
  out.hotspot_y = si.HotSpot.y;
  out.bits = bits;

  // 少数驱动不给 Pitch（填 0）。按类型补一个最小值，总比整块形状作废好 ——
  // 形状作废的症状是"录屏里没有鼠标"，和没做这个功能长得一模一样。
  if (out.pitch <= 0 && out.width > 0) {
    out.pitch = out.type == CursorShapeType::Monochrome
                    ? ((out.width + 7) / 8 + 3) / 4 * 4   // 1bpp，DWORD 对齐
                    : out.width * 4;
  }
  // 长度检查在 CursorShape::valid() 里（它会按 height × pitch 去读 bits，
  // 短一个字节就是越界读）。
  return out.valid();
}

}  // namespace

DesktopCapture::~DesktopCapture() { close(); }

void DesktopCapture::close() {
  staging_.reset();
  dup_.reset();
  context_.reset();
  device_.reset();
  first_frame_ = true;
  warned_static_ = false;
  cursor_.clear_shape();
  desktop_.clear();
  shape_buf_.clear();
  have_desktop_ = false;
  warned_shape_ = false;
}

bool DesktopCapture::open(OutputInfo const& out, std::string& err) {
  close();
  adapter_index_ = out.adapter_index;
  output_index_ = out.output_index;
  device_name_ = out.device_name;
  width_ = out.width();
  height_ = out.height();
  return create_session(err);
}

bool DesktopCapture::pick_output(IDXGIFactory1* factory, IDXGIAdapter1** adapter_out,
                                 IDXGIOutput** output_out, std::string& err) {
  // --- 1. 按设备名找 -------------------------------------------------------
  //
  // **先按名字，不先按下标。** 下标只在"枚举顺序没变过"的前提下才等价于
  // "同一台显示器"，而显示器一插一拔、切换主副屏、驱动重载都会让它错位。
  // 错位之后 EnumOutputs 照样成功 —— 它指向的是另一台显示器，于是录屏安静地
  // 换成了别人的桌面，没有任何一条错误信息。名字是唯一稳定的标识。
  if (!device_name_.empty()) {
    for (UINT ai = 0;; ++ai) {
      ComPtr<IDXGIAdapter1> ad;
      if (FAILED(factory->EnumAdapters1(ai, ad.put())) || !ad) break;
      for (UINT oi = 0;; ++oi) {
        ComPtr<IDXGIOutput> op;
        if (FAILED(ad->EnumOutputs(oi, op.put())) || !op) break;
        DXGI_OUTPUT_DESC d{};
        if (FAILED(op->GetDesc(&d))) continue;
        if (device_name_ != to_utf8(d.DeviceName)) continue;

        if (ai != adapter_index_ || oi != output_index_) {
          // 换了适配器的话，D3D 设备和中转纹理都是**旧适配器**上的东西，
          // 必须一起丢掉重建 —— 拿旧设备去 DuplicateOutput 新适配器的输出
          // 会返回 E_INVALIDARG，而那条路看起来像"这台显示器不支持复制"。
          D2O_INFO("目标输出 %s 的枚举下标变了：#%u/%u → #%u/%u，已重新定位",
                   device_name_.c_str(), adapter_index_, output_index_, ai, oi);
          if (ai != adapter_index_) {
            device_.reset();
            context_.reset();
          }
          staging_.reset();
          adapter_index_ = ai;
          output_index_ = oi;
        }
        *adapter_out = ad.detach();
        *output_out = op.detach();
        return true;
      }
    }
  }

  // --- 2. 退回按下标找 -----------------------------------------------------
  //
  // 走到这里有两种情况：设备名是空的（老调用方），或者那块屏此刻真的不在
  // 枚举里。后者会在下面以"枚举输出失败"收场，而调用方会带着退让重试 ——
  // 显示器插回来之后就又能按名字找到了。
  ComPtr<IDXGIAdapter1> adapter;
  HRESULT hr = factory->EnumAdapters1(adapter_index_, adapter.put());
  if (FAILED(hr) || !adapter) {
    err = fmt("枚举适配器 %u 失败：%s", adapter_index_,
              hresult_text(static_cast<long>(hr)).c_str());
    return false;
  }
  ComPtr<IDXGIOutput> output;
  hr = adapter->EnumOutputs(output_index_, output.put());
  if (FAILED(hr) || !output) {
    std::string const who = device_name_.empty() ? std::string("该输出")
                                                 : (device_name_ + " 这台显示器");
    err = fmt("枚举输出 %u 失败：%s —— %s此刻不在输出列表里（断开了？）",
              output_index_, hresult_text(static_cast<long>(hr)).c_str(),
              who.c_str());
    return false;
  }
  *adapter_out = adapter.detach();
  *output_out = output.detach();
  return true;
}

bool DesktopCapture::create_session(std::string& err) {
  // --- 1. 工厂、适配器、输出 ----------------------------------------------
  ComPtr<IDXGIFactory1> factory;
  HRESULT hr = ::CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                    reinterpret_cast<void**>(factory.put()));
  if (FAILED(hr)) {
    err = "CreateDXGIFactory1 失败：" + hresult_text(static_cast<long>(hr));
    return false;
  }

  ComPtr<IDXGIAdapter1> adapter;
  ComPtr<IDXGIOutput> output;
  if (!pick_output(factory.get(), adapter.put(), output.put(), err)) return false;

  DXGI_OUTPUT_DESC od{};
  if (SUCCEEDED(output->GetDesc(&od))) {
    width_ = static_cast<int>(od.DesktopCoordinates.right - od.DesktopCoordinates.left);
    height_ = static_cast<int>(od.DesktopCoordinates.bottom - od.DesktopCoordinates.top);
  }

  // --- 2. D3D 设备 ---------------------------------------------------------
  // 必须用这块输出所属的适配器来建设备。用别的适配器建出来的设备去
  // DuplicateOutput 会返回 E_INVALIDARG。
  if (!device_) {
    D3D_FEATURE_LEVEL const levels[] = {
        D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
        D3D_FEATURE_LEVEL_9_3,  D3D_FEATURE_LEVEL_9_2,  D3D_FEATURE_LEVEL_9_1};
    hr = ::D3D11CreateDevice(adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
                             levels, static_cast<UINT>(sizeof levels / sizeof levels[0]),
                             D3D11_SDK_VERSION, device_.put(), nullptr, context_.put());
    if (FAILED(hr)) {
      err = "D3D11CreateDevice 失败：" + hresult_text(static_cast<long>(hr));
      return false;
    }
  }

  // --- 3. 复制会话 ---------------------------------------------------------
  ComPtr<IDXGIOutput1> output1;
  hr = output->QueryInterface(__uuidof(IDXGIOutput1),
                              reinterpret_cast<void**>(output1.put()));
  if (FAILED(hr) || !output1) {
    err = "该输出没有 IDXGIOutput1 接口";
    return false;
  }

  hr = output1->DuplicateOutput(device_.get(), dup_.put());
  if (FAILED(hr)) {
    err = fmt("DuplicateOutput 失败：%s（%s）",
              hresult_text(static_cast<long>(hr)).c_str(),
              hresult_hex(static_cast<long>(hr)).c_str());
    return false;
  }

  // --- 4. 中转纹理 ---------------------------------------------------------
  if (!ensure_staging(err)) return false;

  first_frame_ = true;
  warned_static_ = false;
  changed_frames_ = 0;
  pointer_only_frames_ = 0;
  composited_frames_ = 0;
  // 重建之后那份"没有指针的桌面图"不一定还对得上（分辨率可能变了），
  // 丢掉重来最省心：重建后的第一帧本来就是整帧。
  have_desktop_ = false;
  D2O_INFO("捕获会话已建立：%dx%d，适配器 %u 输出 %u", width_, height_,
           adapter_index_, output_index_);
  return true;
}

bool DesktopCapture::reopen(std::string& err) {
  // 分辨率可能变了：先放掉旧的中转纹理，create_session 会按新尺寸重建。
  staging_.reset();
  dup_.reset();
  return create_session(err);
}

bool DesktopCapture::ensure_staging(std::string& err) {
  if (staging_) {
    D3D11_TEXTURE2D_DESC d{};
    staging_->GetDesc(&d);
    if (static_cast<int>(d.Width) == width_ &&
        static_cast<int>(d.Height) == height_) {
      return true;  // 尺寸没变，复用
    }
    staging_.reset();
  }
  if (width_ <= 0 || height_ <= 0) {
    err = fmt("输出尺寸不合法：%dx%d", width_, height_);
    return false;
  }

  D3D11_TEXTURE2D_DESC td{};
  td.Width = static_cast<UINT>(width_);
  td.Height = static_cast<UINT>(height_);
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = kDesktopFormat;
  td.SampleDesc.Count = 1;
  td.SampleDesc.Quality = 0;
  td.Usage = D3D11_USAGE_STAGING;
  // CPU 只能读。桌面复制的画面归 DXGI 所有，我们只是把它拷出来看一眼。
  td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  td.BindFlags = 0;
  td.MiscFlags = 0;

  HRESULT const hr = device_->CreateTexture2D(&td, nullptr, staging_.put());
  if (FAILED(hr)) {
    err = fmt("创建中转纹理（%dx%d）失败：%s", width_, height_,
              hresult_text(static_cast<long>(hr)).c_str());
    return false;
  }
  return true;
}

DesktopCapture::Status DesktopCapture::acquire(std::uint32_t timeout_ms,
                                               BgraFrame& dst, bool& changed,
                                               std::string& err) {
  changed = false;
  if (!dup_) {
    err = "捕获会话未建立";
    return Status::Fatal;
  }

  DXGI_OUTDUPL_FRAME_INFO info{};
  ComPtr<IDXGIResource> res;
  HRESULT const hr = dup_->AcquireNextFrame(timeout_ms, &info, res.put());
  if (hr == DXGI_ERROR_WAIT_TIMEOUT) return Status::Timeout;
  if (hr == DXGI_ERROR_ACCESS_LOST) return Status::AccessLost;
  if (FAILED(hr)) {
    err = fmt("AcquireNextFrame 失败：%s（%s）",
              hresult_text(static_cast<long>(hr)).c_str(),
              hresult_hex(static_cast<long>(hr)).c_str());
    return Status::Fatal;
  }

  // --- 指针：必须在 ReleaseFrame 之前取 ------------------------------------
  // 形状是"跟着当前这一帧"的，帧一释放就没了。
  bool pointer_changed = false;
  if (info.LastMouseUpdateTime.QuadPart != 0) {
    // Position 只有在 LastMouseUpdateTime 非零时才有效。
    // set_state 会自己比较新旧值 —— LastMouseUpdateTime 在"按了键但指针没动"
    // 时也会刷新，直接拿它当"指针动了"会凭空多编一堆一模一样的帧。
    if (cursor_.set_state(info.PointerPosition.Position.x,
                          info.PointerPosition.Position.y,
                          info.PointerPosition.Visible != FALSE)) {
      pointer_changed = true;
    }
  }
  if (info.PointerShapeBufferSize > 0 || !cursor_.has_shape()) {
    // 后半个条件管的是"第一帧没报形状"：那时不主动取一次，指针就要等到
    // 用户换个形状（悬停、改指针大小）才出现。
    fetch_pointer_shape(info);
  }

  // LastPresentTime 非零 = 桌面图像真的被更新过。
  //
  // 为零的两种情形要分开看：
  //   * 会话已经开始过、我们手上已经有画面 —— 这时中转纹理里的内容是**未定义**的
  //     （DXGI 的约定是"桌面没变就不更新这张纹理"），必须原样跳过，不能读。
  //   * 这是 DuplicateOutput 之后的第一帧 —— 此时我们手里什么都没有，而不读就
  //     等于永远录不到画面。这里选择读一次：即便拿到的是未定义内容，下一帧真的
  //     有变化时就会被覆盖；反过来不读就是黑屏到底，那是更糟的失败方式。
  bool const has_present = info.LastPresentTime.QuadPart != 0;
  bool const bootstrap = first_frame_;
  first_frame_ = false;

  if (!has_present && !bootstrap) {
    dup_->ReleaseFrame();
    if (!warned_static_) {
      warned_static_ = true;
      D2O_DEBUG("桌面静止（LastPresentTime=0），不读纹理，继续用上一帧");
    }
    // 桌面没变、**但指针动了**。这是一种必须产出的帧：不产出的话，画面上的
    // 指针就冻在上一格，而鼠标明明在动 —— 比完全没有指针更让人困惑。
    // 桌面图（不含指针的那份）还在，从它重新合成一张就行，不需要读纹理
    // —— 此刻纹理里是未定义内容，读了就是把垃圾编进视频。
    if (!pointer_changed) return Status::Timeout;
    Status const st = composite_from_desktop(dst);
    if (st != Status::Ok) return st;
    ++pointer_only_frames_;
    changed = true;
    return Status::Ok;
  }

  // CopyResource → ReleaseFrame → Map → memcpy 的顺序见头文件里的说明。
  ComPtr<ID3D11Texture2D> tex;
  if (!as_texture(res.get(), tex.put())) {
    dup_->ReleaseFrame();
    err = "桌面复制的帧不是 ID3D11Texture2D";
    return Status::Fatal;
  }
  context_->CopyResource(staging_.get(), tex.get());
  tex.reset();
  res.reset();
  dup_->ReleaseFrame();

  D3D11_MAPPED_SUBRESOURCE mapped{};
  HRESULT const mhr = context_->Map(staging_.get(), 0, D3D11_MAP_READ, 0, &mapped);
  if (FAILED(mhr)) {
    err = fmt("Map 中转纹理失败：%s", hresult_text(static_cast<long>(mhr)).c_str());
    return Status::Fatal;
  }

  ensure_dst(dst);

  // 逐行拷：源的行距由 D3D 决定（GPU 纹理有对齐要求），通常大于 width*4。
  // 目标用紧凑行距，这样编码侧不用再关心对齐。
  std::uint8_t const* src = static_cast<std::uint8_t const*>(mapped.pData);
  std::size_t const row = static_cast<std::size_t>(width_) * 4u;
  for (int y = 0; y < height_; ++y) {
    std::memcpy(dst.pixels.data() + static_cast<std::size_t>(y) * row,
                src + static_cast<std::size_t>(y) * mapped.RowPitch, row);
  }
  context_->Unmap(staging_.get(), 0);

  // 再把**没有指针**的这一份留一张。指针单独动的时候只有它可用。
  //
  // 顺序上先拷进 dst 再拷进 desktop_：两次拷贝无论谁先谁后总量一样，
  // 但先填 dst 意味着即便下面这次 memcpy 出问题（比如 bad_alloc），
  // 也已经有一张能用的画面了。
  desktop_.assign(dst.pixels.begin(), dst.pixels.end());
  have_desktop_ = true;

  if (cursor_.drawable()) {
    cursor_.compose(dst.pixels.data(), width_, height_,
                    static_cast<std::size_t>(dst.stride));
    ++composited_frames_;
  }

  changed = true;
  ++changed_frames_;
  return Status::Ok;
}

DesktopCapture::Status DesktopCapture::composite_from_desktop(BgraFrame& dst) {
  if (!have_desktop_) return Status::Timeout;
  ensure_dst(dst);
  if (desktop_.size() != dst.pixels.size()) {
    // 尺寸对不上（多半是重建过会话但 have_desktop_ 还没被清）。宁可丢掉
    // 这一帧，也不能整块拷进去 —— 那样画面的下半部分是上一次分辨率的内容。
    have_desktop_ = false;
    return Status::Timeout;
  }
  std::memcpy(dst.pixels.data(), desktop_.data(), desktop_.size());
  if (cursor_.drawable()) {
    cursor_.compose(dst.pixels.data(), width_, height_,
                    static_cast<std::size_t>(dst.stride));
    ++composited_frames_;
  }
  return Status::Ok;
}

DesktopCapture::Status DesktopCapture::reemit(BgraFrame& dst) {
  return composite_from_desktop(dst);
}

void DesktopCapture::ensure_dst(BgraFrame& dst) {
  // 按需调整目标缓冲。改了尺寸说明中转纹理刚被重建过（分辨率切换）。
  std::size_t const need =
      static_cast<std::size_t>(width_) * 4u * static_cast<std::size_t>(height_);
  if (dst.width != width_ || dst.height != height_ ||
      static_cast<int>(dst.stride) != width_ * 4 || dst.pixels.size() != need) {
    dst.width = width_;
    dst.height = height_;
    dst.stride = width_ * 4;
    dst.pixels.resize(need);
  }
}

void DesktopCapture::fetch_pointer_shape(DXGI_OUTDUPL_FRAME_INFO const& info) {
  // 先按帧信息里报的需求量取。`PointerShapeBufferSize` 本来就是"这一次需要
  // 多大的缓冲"，所以正常路径上一次调用就够。它可能是 0（形状没变），
  // 这时按一个够大的经验值试一次 —— 失败也无所谓，指针不画就是。
  UINT want = info.PointerShapeBufferSize;
  if (want == 0) want = 64u * 1024u;
  shape_buf_.resize(want);

  DXGI_OUTDUPL_POINTER_SHAPE_INFO si{};
  UINT required = 0;
  HRESULT const hr = dup_->GetFramePointerShape(
      static_cast<UINT>(shape_buf_.size()), shape_buf_.data(), &required, &si);

  if (hr == DXGI_ERROR_MORE_DATA) {
    // 缓冲不够，required 里是真正需要的大小。再取一次。
    shape_buf_.resize(required);
    HRESULT const hr2 = dup_->GetFramePointerShape(
        static_cast<UINT>(shape_buf_.size()), shape_buf_.data(), &required, &si);
    if (FAILED(hr2)) {
      if (!warned_shape_) {
        warned_shape_ = true;
        D2O_WARN("取指针形状失败（%s），鼠标不会出现在画面里",
                 hresult_text(static_cast<long>(hr2)).c_str());
      }
      return;
    }
  } else if (FAILED(hr)) {
    if (!warned_shape_) {
      warned_shape_ = true;
      D2O_WARN("取指针形状失败（%s），鼠标不会出现在画面里",
               hresult_text(static_cast<long>(hr)).c_str());
    }
    return;
  }

  CursorShape shape;
  if (!to_cursor_shape(si, shape_buf_, shape)) {
    // 形状的类型不认识，或者长度对不上。**不猜**：宁可没有指针，也不要一块
    // 花花绿绿的东西盖在画面上 —— 后者会被当成"编码坏了"，查的方向全错。
    if (!warned_shape_) {
      warned_shape_ = true;
      D2O_WARN("指针形状不认识（类型 %u，%ux%u，pitch %u，%u 字节），跳过合成",
               static_cast<unsigned>(si.Type), static_cast<unsigned>(si.Width),
               static_cast<unsigned>(si.Height), static_cast<unsigned>(si.Pitch),
               static_cast<unsigned>(shape_buf_.size()));
    }
    cursor_.clear_shape();
    return;
  }

  D2O_DEBUG("指针形状：%s", shape.type == CursorShapeType::Monochrome ? "单色"
                              : shape.type == CursorShapeType::MaskedColor
                                  ? "掩码色"
                                  : "彩色");
  cursor_.set_shape(std::move(shape));
}

}  // namespace d2o
