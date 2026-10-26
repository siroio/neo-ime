// Copyright 2026 neo-ime contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef NEO_IME_NATIVE_TSF_TEXT_STORE_H_
#define NEO_IME_NATIVE_TSF_TEXT_STORE_H_

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <msctf.h>
#include <textstor.h>

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace neo_ime {

// GUI-thread-owned COM reference with one explicit owner.
template <typename Interface>
class ComPtr {
 public:
  ComPtr() = default;
  ~ComPtr() { Reset(); }
  ComPtr(const ComPtr&) = delete;
  ComPtr& operator=(const ComPtr&) = delete;
  Interface* get() const { return pointer_; }
  Interface* operator->() const { return pointer_; }
  explicit operator bool() const { return pointer_ != nullptr; }
  Interface** put() {
    Reset();
    return &pointer_;
  }
  void Reset() {
    if (pointer_) pointer_->Release();
    pointer_ = nullptr;
  }

 private:
  Interface* pointer_ = nullptr;
};

struct TextEdit {
  std::uint64_t revision = 0;
  std::uint64_t generation = 0;
  std::wstring text;
  LONG cursor = 0;
  LONG composition_start = 0;
  LONG composition_length = 0;
  bool composing = false;
  std::vector<BYTE> attributes;
};

// All methods run on the HWND's GUI thread, including COM AddRef/Release.
class TextStore final : public ITextStoreACP,
                        public ITfContextOwnerCompositionSink {
 public:
  static constexpr TsViewCookie kView = 1;
  static constexpr std::size_t kMaximumTextUnits = 512 * 1024;
  explicit TextStore(HWND window) : window_(window) {}
  HRESULT Reset(const std::wstring& text, LONG start, LONG end,
                std::uint64_t generation, bool read_only = false);
  HRESULT Publish();
  void SetAttributes(std::vector<BYTE> attributes);
  std::size_t text_length() const { return text_.size(); }
  void Acknowledge(std::uint64_t revision);
  const std::deque<TextEdit>& events() const { return events_; }
  bool composing() const { return composition_ != nullptr; }
  void SetAnchor(POINT anchor, int height);
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** result) override;
  ULONG STDMETHODCALLTYPE AddRef() override;
  ULONG STDMETHODCALLTYPE Release() override;
  HRESULT STDMETHODCALLTYPE AdviseSink(REFIID, IUnknown*, DWORD) override;
  HRESULT STDMETHODCALLTYPE UnadviseSink(IUnknown*) override;
  HRESULT STDMETHODCALLTYPE RequestLock(DWORD, HRESULT*) override;
  HRESULT STDMETHODCALLTYPE GetStatus(TS_STATUS*) override;
  HRESULT STDMETHODCALLTYPE QueryInsert(LONG, LONG, ULONG, LONG*,
                                        LONG*) override;
  HRESULT STDMETHODCALLTYPE GetSelection(ULONG, ULONG, TS_SELECTION_ACP*,
                                         ULONG*) override;
  HRESULT STDMETHODCALLTYPE SetSelection(ULONG,
                                         const TS_SELECTION_ACP*) override;
  HRESULT STDMETHODCALLTYPE GetText(LONG, LONG, WCHAR*, ULONG, ULONG*,
                                    TS_RUNINFO*, ULONG, ULONG*, LONG*) override;
  HRESULT STDMETHODCALLTYPE SetText(DWORD, LONG, LONG, const WCHAR*, ULONG,
                                    TS_TEXTCHANGE*) override;
  HRESULT STDMETHODCALLTYPE GetFormattedText(LONG, LONG,
                                             IDataObject**) override;
  HRESULT STDMETHODCALLTYPE GetEmbedded(LONG, REFGUID, REFIID,
                                        IUnknown**) override;
  HRESULT STDMETHODCALLTYPE QueryInsertEmbedded(const GUID*, const FORMATETC*,
                                                BOOL*) override;
  HRESULT STDMETHODCALLTYPE InsertEmbedded(DWORD, LONG, LONG, IDataObject*,
                                           TS_TEXTCHANGE*) override;
  HRESULT STDMETHODCALLTYPE InsertTextAtSelection(DWORD, const WCHAR*, ULONG,
                                                  LONG*, LONG*,
                                                  TS_TEXTCHANGE*) override;
  HRESULT STDMETHODCALLTYPE InsertEmbeddedAtSelection(DWORD, IDataObject*,
                                                      LONG*, LONG*,
                                                      TS_TEXTCHANGE*) override;
  HRESULT STDMETHODCALLTYPE RequestSupportedAttrs(DWORD, ULONG,
                                                  const TS_ATTRID*) override;
  HRESULT STDMETHODCALLTYPE RequestAttrsAtPosition(LONG, ULONG,
                                                   const TS_ATTRID*,
                                                   DWORD) override;
  HRESULT STDMETHODCALLTYPE RequestAttrsTransitioningAtPosition(
      LONG, ULONG, const TS_ATTRID*, DWORD) override;
  HRESULT STDMETHODCALLTYPE FindNextAttrTransition(LONG, LONG, ULONG,
                                                   const TS_ATTRID*, DWORD,
                                                   LONG*, BOOL*,
                                                   LONG*) override;
  HRESULT STDMETHODCALLTYPE RetrieveRequestedAttrs(ULONG, TS_ATTRVAL*,
                                                   ULONG*) override;
  HRESULT STDMETHODCALLTYPE GetEndACP(LONG*) override;
  HRESULT STDMETHODCALLTYPE GetActiveView(TsViewCookie*) override;
  HRESULT STDMETHODCALLTYPE GetACPFromPoint(TsViewCookie, const POINT*, DWORD,
                                            LONG*) override;
  HRESULT STDMETHODCALLTYPE GetTextExt(TsViewCookie, LONG, LONG, RECT*,
                                       BOOL*) override;
  HRESULT STDMETHODCALLTYPE GetScreenExt(TsViewCookie, RECT*) override;
  HRESULT STDMETHODCALLTYPE GetWnd(TsViewCookie, HWND*) override;
  HRESULT STDMETHODCALLTYPE OnStartComposition(ITfCompositionView*,
                                               BOOL*) override;
  HRESULT STDMETHODCALLTYPE OnUpdateComposition(ITfCompositionView*,
                                                ITfRange*) override;
  HRESULT STDMETHODCALLTYPE OnEndComposition(ITfCompositionView*) override;

 private:
  ~TextStore();
  bool HasLock(DWORD access) const;
  bool ValidPosition(LONG position) const;
  bool ValidRange(LONG start, LONG end) const;
  HRESULT CompositionExtent(LONG* start, LONG* length);
  ULONG references_ = 1;
  HWND window_;
  ComPtr<ITextStoreACPSink> sink_;
  DWORD sink_mask_ = 0;
  DWORD lock_flags_ = 0;
  DWORD pending_lock_ = 0;
  std::wstring text_;
  TS_SELECTION_ACP selection_{0, 0, {TS_AE_END, FALSE}};
  ITfCompositionView* composition_ = nullptr;
  std::uint64_t generation_ = 0;
  std::uint64_t revision_ = 0;
  std::deque<TextEdit> events_;
  bool dirty_ = false;
  bool read_only_ = false;
  std::vector<BYTE> attributes_;
  POINT anchor_{};
  int line_height_ = 1;
};
}  // namespace neo_ime
#endif
