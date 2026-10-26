// Copyright 2026 neo-ime contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef NEO_IME_NATIVE_TSF_BACKEND_H_
#define NEO_IME_NATIVE_TSF_BACKEND_H_
#include "tsf_text_store.h"

#include <vector>

namespace neo_ime {
enum class TsfOperation {
  kStart,
  kRead,
  kReset,
  kAcknowledge,
  kCancel,
  kReconvert,
  kGetOpen,
  kSetOpen,
  kGetConversion,
  kSetConversion
};
struct TsfRequest {
  TsfOperation operation = TsfOperation::kRead;
  HRESULT result = E_FAIL;
  std::wstring text;
  LONG selection_start = 0;
  LONG selection_end = 0;
  bool read_only = false;
  std::uint64_t generation = 0;
  std::uint64_t acknowledgement = 0;
  LONG value = 0;
  std::vector<TextEdit> edits;
  std::vector<std::wstring> candidates;
  LONG selection = -1;
  UINT page_start = 0;
  UINT total = 0;
};

class TsfBackend final : public ITfUIElementSink {
 public:
  explicit TsfBackend(HWND window) : window_(window) {}
  HRESULT Initialize();
  void Shutdown();
  void Focus(bool focused);
  void SetAnchor(POINT anchor, int height);
  void Execute(TsfRequest& request) noexcept;
  bool composing() const { return store_ && store_->composing(); }
  bool active() const { return active_; }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override;
  ULONG STDMETHODCALLTYPE AddRef() override;
  ULONG STDMETHODCALLTYPE Release() override;
  HRESULT STDMETHODCALLTYPE BeginUIElement(DWORD, BOOL*) override;
  HRESULT STDMETHODCALLTYPE UpdateUIElement(DWORD) override;
  HRESULT STDMETHODCALLTYPE EndUIElement(DWORD) override;

 private:
  ~TsfBackend() = default;
  static LRESULT CALLBACK KeyHook(int, WPARAM, LPARAM);
  HRESULT Cancel();
  HRESULT Reconvert();
  HRESULT ReadCompartment(REFGUID, LONG*);
  HRESULT WriteCompartment(REFGUID, LONG);
  HRESULT ReadCandidates(DWORD);
  void RefreshAttributes();
  ULONG references_ = 1;
  HWND window_;
  bool initialized_com_ = false;
  bool active_ = false;
  bool associated_ = false;
  TfClientId client_id_ = 0;
  DWORD ui_cookie_ = TF_INVALID_COOKIE;
  DWORD candidate_id_ = TF_INVALID_COOKIE;
  ComPtr<ITfThreadMgr> manager_;
  ComPtr<ITfDocumentMgr> document_;
  ComPtr<ITfDocumentMgr> previous_document_;
  ComPtr<ITfContext> context_;
  ComPtr<ITfKeystrokeMgr> keys_;
  ComPtr<ITfUIElementMgr> ui_;
  ComPtr<ITfCategoryMgr> categories_;
  ComPtr<ITfDisplayAttributeMgr> display_attributes_;
  TextStore* store_ = nullptr;
  TsfBackend* next_ = nullptr;
  std::vector<std::wstring> candidates_;
  LONG selection_ = -1;
  UINT page_start_ = 0;
  UINT total_ = 0;
  static thread_local TsfBackend* thread_backends_;
  static thread_local HHOOK thread_key_hook_;
};
}  // namespace neo_ime
#endif
