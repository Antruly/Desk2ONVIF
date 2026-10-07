#pragma once

/**
 * @file src/common/com_ptr.h
 * @brief 极简的 COM 智能指针。
 *
 * 项目里只有 DXGI/D3D 这一小块用 COM，为它引一个 wrl/com_ptr.h 或者
 * 依赖 ATL 不划算。这里只实现实际用到的几个操作。
 *
 * 注意 `put()` 会先 reset：它的语义是"把新拿到的接口写进来"，
 * 而不是"在已有指针上再 QueryInterface"。
 */

namespace d2o {

template <typename T>
class ComPtr {
 public:
  ComPtr() = default;
  ~ComPtr() { reset(); }

  ComPtr(ComPtr const&) = delete;
  ComPtr& operator=(ComPtr const&) = delete;

  ComPtr(ComPtr&& other) noexcept : p_(other.p_) { other.p_ = nullptr; }
  ComPtr& operator=(ComPtr&& other) noexcept {
    if (this != &other) {
      reset();
      p_ = other.p_;
      other.p_ = nullptr;
    }
    return *this;
  }

  /// 取出用来接收新接口的地址（会先释放已有的）。
  T** put() {
    reset();
    return &p_;
  }

  /// 取出用来接收"可选的"接口的地址。失败时保留原有的，不置空。
  T** put_opt() { return &p_; }

  T* get() const { return p_; }
  T* operator->() const { return p_; }
  explicit operator bool() const { return p_ != nullptr; }

  void reset() {
    if (p_) {
      p_->Release();
      p_ = nullptr;
    }
  }

  /// 放弃所有权，交给调用方（COM 的 CreateInstance 那套风格）。
  T* detach() {
    T* t = p_;
    p_ = nullptr;
    return t;
  }

 private:
  T* p_ = nullptr;
};

}  // namespace d2o
