// Copyright 2026 neo-ime contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "tsf_backend.h"
#include "tsf_compat.h"

#include <imm.h>

#include <algorithm>
#include <new>

namespace neo_ime {
namespace {
constexpr UINT kMaximumCandidateCount = 65536;
constexpr UINT kDefaultPageSize = 9;
constexpr LONG kNoSelection = -1;
constexpr LONG kConversionShapeMask =
    IME_CMODE_NATIVE | IME_CMODE_KATAKANA | IME_CMODE_FULLSHAPE;

class AttributeSession final : public ITfEditSession {
 public:
  AttributeSession(ITfContext* context, TextStore* store,
                   ITfCategoryMgr* categories,
                   ITfDisplayAttributeMgr* attributes)
      : context_(context),
        store_(store),
        categories_(categories),
        attributes_(attributes) {
    context_->AddRef();
    store_->AddRef();
    categories_->AddRef();
    attributes_->AddRef();
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** output) override {
    if (!output) return E_POINTER;
    *output = nullptr;
    if (id != IID_IUnknown && id != IID_ITfEditSession) return E_NOINTERFACE;
    *output = static_cast<ITfEditSession*>(this);
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG left = --references_;
    if (!left) delete this;
    return left;
  }
  HRESULT STDMETHODCALLTYPE DoEditSession(TfEditCookie cookie) override {
    try {
      ComPtr<ITfProperty> property;
      ComPtr<IEnumTfRanges> ranges;
      HRESULT result =
          context_->GetProperty(GUID_PROP_ATTRIBUTE, property.put());
      if (SUCCEEDED(result))
        result = property->EnumRanges(cookie, ranges.put(), nullptr);
      if (FAILED(result)) return result;
      std::vector<BYTE> values(store_->text_length(), TF_ATTR_INPUT);
      ComPtr<ITfRange> range;
      ULONG fetched = 0;
      while (ranges->Next(1, range.put(), &fetched) == S_OK && fetched == 1) {
        VARIANT value;
        VariantInit(&value);
        result = property->GetValue(cookie, range.get(), &value);
        GUID id{};
        if (SUCCEEDED(result) && value.vt == VT_I4)
          result =
              categories_->GetGUID(static_cast<TfGuidAtom>(value.lVal), &id);
        else
          result = E_FAIL;
        VariantClear(&value);
        if (FAILED(result)) continue;
        ComPtr<ITfDisplayAttributeInfo> info;
        ComPtr<ITfRangeACP> acp;
        TF_DISPLAYATTRIBUTE attribute{};
        LONG start = 0, length = 0;
        if (FAILED(attributes_->GetDisplayAttributeInfo(id, info.put(),
                                                        nullptr)) ||
            FAILED(info->GetAttributeInfo(&attribute)) ||
            FAILED(range->QueryInterface(
                IID_ITfRangeACP, reinterpret_cast<void**>(acp.put()))) ||
            FAILED(acp->GetExtent(&start, &length)))
          continue;
        if (start < 0 || length < 0 ||
            static_cast<std::size_t>(start) > values.size() ||
            static_cast<std::size_t>(length) >
                values.size() - static_cast<std::size_t>(start))
          return E_UNEXPECTED;
        std::fill(values.begin() + start, values.begin() + start + length,
                  static_cast<BYTE>(attribute.bAttr));
      }
      store_->SetAttributes(std::move(values));
      return S_OK;
    } catch (const std::bad_alloc&) {
      return E_OUTOFMEMORY;
    }
  }

 private:
  ~AttributeSession() {
    attributes_->Release();
    categories_->Release();
    store_->Release();
    context_->Release();
  }
  ULONG references_ = 1;
  ITfContext* context_;
  TextStore* store_;
  ITfCategoryMgr* categories_;
  ITfDisplayAttributeMgr* attributes_;
};

class ReconvertSession final : public ITfEditSession {
 public:
  ReconvertSession(ITfContext* context, ReconversionFunction* function)
      : context_(context), function_(function) {
    context_->AddRef();
    function_->AddRef();
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** output) override {
    if (!output) return E_POINTER;
    *output = nullptr;
    if (id != IID_IUnknown && id != IID_ITfEditSession) return E_NOINTERFACE;
    *output = static_cast<ITfEditSession*>(this);
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG left = --references_;
    if (!left) delete this;
    return left;
  }
  HRESULT STDMETHODCALLTYPE DoEditSession(TfEditCookie cookie) override {
    TF_SELECTION selection{};
    ULONG fetched = 0;
    HRESULT result = context_->GetSelection(cookie, TF_DEFAULT_SELECTION, 1,
                                            &selection, &fetched);
    if (FAILED(result) || fetched != 1 || !selection.range)
      return FAILED(result) ? result : E_FAIL;
    ComPtr<ITfRange> range;
    BOOL convertible = FALSE;
    result = function_->QueryRange(selection.range, range.put(), &convertible);
    selection.range->Release();
    if (FAILED(result)) return result;
    if (!convertible || !range) return E_NOTIMPL;
    return function_->Reconvert(range.get());
  }

 private:
  ~ReconvertSession() {
    function_->Release();
    context_->Release();
  }
  ULONG references_ = 1;
  ITfContext* context_;
  ReconversionFunction* function_;
};
}  // namespace
thread_local TsfBackend* TsfBackend::thread_backends_ = nullptr;
thread_local HHOOK TsfBackend::thread_key_hook_ = nullptr;

HRESULT TsfBackend::Initialize() {
  const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(com)) return com;
  initialized_com_ = true;
  HRESULT result = CoCreateInstance(CLSID_TF_ThreadMgr, nullptr,
                                    CLSCTX_INPROC_SERVER, IID_ITfThreadMgr,
                                    reinterpret_cast<void**>(manager_.put()));
  ComPtr<ITfThreadMgrEx> extended_manager;
  if (SUCCEEDED(result))
    result = manager_->QueryInterface(
        IID_ITfThreadMgrEx, reinterpret_cast<void**>(extended_manager.put()));
  if (SUCCEEDED(result))
    result =
        extended_manager->ActivateEx(&client_id_, TF_TMAE_UIELEMENTENABLEDONLY);
  active_ = SUCCEEDED(result);
  DWORD active_flags = 0;
  if (SUCCEEDED(result))
    result = extended_manager->GetActiveFlags(&active_flags);
  if (SUCCEEDED(result) && !(active_flags & TF_TMF_UIELEMENTENABLEDONLY))
    result = E_NOTIMPL;
  extended_manager.Reset();
  if (SUCCEEDED(result)) result = manager_->CreateDocumentMgr(document_.put());
  if (SUCCEEDED(result)) {
    store_ = new (std::nothrow) TextStore(window_);
    if (!store_) result = E_OUTOFMEMORY;
  }
  TfEditCookie edit_cookie = 0;
  if (SUCCEEDED(result))
    result = document_->CreateContext(client_id_, 0,
                                      static_cast<ITextStoreACP*>(store_),
                                      context_.put(), &edit_cookie);
  if (SUCCEEDED(result)) result = document_->Push(context_.get());
  if (SUCCEEDED(result))
    result = manager_->QueryInterface(IID_ITfKeystrokeMgr,
                                      reinterpret_cast<void**>(keys_.put()));
  if (SUCCEEDED(result))
    result = manager_->AssociateFocus(window_, document_.get(),
                                      previous_document_.put());
  associated_ = SUCCEEDED(result);
  if (SUCCEEDED(result) && !thread_key_hook_) {
    thread_key_hook_ = SetWindowsHookExW(WH_GETMESSAGE, KeyHook, nullptr,
                                         GetCurrentThreadId());
    if (!thread_key_hook_) result = HRESULT_FROM_WIN32(GetLastError());
  }
  if (FAILED(result)) {
    Shutdown();
    return result;
  }
  next_ = thread_backends_;
  thread_backends_ = this;
  CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER,
                   IID_ITfCategoryMgr,
                   reinterpret_cast<void**>(categories_.put()));
  CoCreateInstance(CLSID_TF_DisplayAttributeMgr, nullptr, CLSCTX_INPROC_SERVER,
                   IID_ITfDisplayAttributeMgr,
                   reinterpret_cast<void**>(display_attributes_.put()));
  result = manager_->QueryInterface(IID_ITfUIElementMgr,
                                    reinterpret_cast<void**>(ui_.put()));
  ComPtr<ITfSource> source;
  if (SUCCEEDED(result))
    result = manager_->QueryInterface(IID_ITfSource,
                                      reinterpret_cast<void**>(source.put()));
  if (SUCCEEDED(result))
    result =
        source->AdviseSink(IID_ITfUIElementSink,
                           static_cast<ITfUIElementSink*>(this), &ui_cookie_);
  source.Reset();
  if (FAILED(result)) {
    Shutdown();
    return result;
  }
  Focus(GetFocus() == window_);
  return S_OK;
}
void TsfBackend::Shutdown() {
  for (TsfBackend** entry = &thread_backends_; *entry; entry = &(*entry)->next_)
    if (*entry == this) {
      *entry = next_;
      break;
    }
  if (!thread_backends_ && thread_key_hook_) {
    UnhookWindowsHookEx(thread_key_hook_);
    thread_key_hook_ = nullptr;
  }
  if (manager_ && ui_cookie_ != TF_INVALID_COOKIE) {
    ComPtr<ITfSource> source;
    if (SUCCEEDED(manager_->QueryInterface(
            IID_ITfSource, reinterpret_cast<void**>(source.put()))))
      source->UnadviseSink(ui_cookie_);
    ui_cookie_ = TF_INVALID_COOKIE;
  }
  if (associated_) {
    ComPtr<ITfDocumentMgr> replaced;
    manager_->AssociateFocus(window_, previous_document_.get(), replaced.put());
    associated_ = false;
  }
  if (store_ && store_->composing()) Cancel();
  if (document_) document_->Pop(TF_POPF_ALL);
  context_.Reset();
  document_.Reset();
  previous_document_.Reset();
  keys_.Reset();
  ui_.Reset();
  categories_.Reset();
  display_attributes_.Reset();
  if (store_) {
    store_->Release();
    store_ = nullptr;
  }
  if (active_) {
    manager_->Deactivate();
    active_ = false;
  }
  manager_.Reset();
  if (initialized_com_) {
    CoUninitialize();
    initialized_com_ = false;
  }
}
void TsfBackend::Focus(bool focused) {
  if (active_ && focused) manager_->SetFocus(document_.get());
}
void TsfBackend::SetAnchor(POINT anchor, int height) {
  if (store_) store_->SetAnchor(anchor, height);
}
LRESULT CALLBACK TsfBackend::KeyHook(int code, WPARAM parameter,
                                     LPARAM payload) {
  if (code >= 0 && parameter == PM_REMOVE) {
    auto& message = *reinterpret_cast<MSG*>(payload);
    for (TsfBackend* backend = thread_backends_; backend;
         backend = backend->next_) {
      if (message.hwnd != backend->window_ || !backend->active_) continue;
      const bool down =
          message.message == WM_KEYDOWN || message.message == WM_SYSKEYDOWN;
      const bool up =
          message.message == WM_KEYUP || message.message == WM_SYSKEYUP;
      if (!down && !up) break;
      BOOL eaten = FALSE;
      HRESULT result = down ? backend->keys_->TestKeyDown(
                                  message.wParam, message.lParam, &eaten)
                            : backend->keys_->TestKeyUp(message.wParam,
                                                        message.lParam, &eaten);
      if (SUCCEEDED(result) && eaten) {
        result = down ? backend->keys_->KeyDown(message.wParam, message.lParam,
                                                &eaten)
                      : backend->keys_->KeyUp(message.wParam, message.lParam,
                                              &eaten);
        if (SUCCEEDED(result) && eaten) {
          message.message = WM_NULL;
          message.wParam = 0;
          message.lParam = 0;
        }
      }
      backend->store_->Publish();
      break;
    }
  }
  return CallNextHookEx(nullptr, code, parameter, payload);
}
HRESULT TsfBackend::Cancel() {
  if (!store_->composing()) return S_OK;
  ComPtr<ITfContextOwnerCompositionServices> services;
  HRESULT result =
      context_->QueryInterface(IID_ITfContextOwnerCompositionServices,
                               reinterpret_cast<void**>(services.put()));
  if (FAILED(result)) return result;
  result = services->TerminateComposition(nullptr);
  const HRESULT published = store_->Publish();
  return FAILED(result) ? result : published;
}
HRESULT TsfBackend::Reconvert() {
  ComPtr<ITfFunctionProvider> provider;
  HRESULT result =
      manager_->GetFunctionProvider(kSystemFunctionProvider, provider.put());
  ComPtr<ReconversionFunction> function;
  if (SUCCEEDED(result))
    result =
        provider->GetFunction(GUID_NULL, kReconversionInterface,
                              reinterpret_cast<IUnknown**>(function.put()));
  if (FAILED(result)) return result;
  auto* session =
      new (std::nothrow) ReconvertSession(context_.get(), function.get());
  if (!session) return E_OUTOFMEMORY;
  HRESULT edit_result = E_FAIL;
  result = context_->RequestEditSession(
      client_id_, session, TF_ES_SYNC | TF_ES_READWRITE, &edit_result);
  session->Release();
  return FAILED(result) ? result : edit_result;
}
HRESULT TsfBackend::ReadCompartment(REFGUID id, LONG* value) {
  ComPtr<ITfCompartmentMgr> compartments;
  ComPtr<ITfCompartment> compartment;
  HRESULT result = manager_->QueryInterface(
      IID_ITfCompartmentMgr, reinterpret_cast<void**>(compartments.put()));
  if (SUCCEEDED(result))
    result = compartments->GetCompartment(id, compartment.put());
  VARIANT data;
  VariantInit(&data);
  if (SUCCEEDED(result)) result = compartment->GetValue(&data);
  if (SUCCEEDED(result) && data.vt != VT_I4) result = E_NOTIMPL;
  if (SUCCEEDED(result)) *value = data.lVal;
  VariantClear(&data);
  return result;
}
HRESULT TsfBackend::WriteCompartment(REFGUID id, LONG value) {
  ComPtr<ITfCompartmentMgr> compartments;
  ComPtr<ITfCompartment> compartment;
  HRESULT result = manager_->QueryInterface(
      IID_ITfCompartmentMgr, reinterpret_cast<void**>(compartments.put()));
  if (SUCCEEDED(result))
    result = compartments->GetCompartment(id, compartment.put());
  if (FAILED(result)) return result;
  VARIANT data;
  VariantInit(&data);
  data.vt = VT_I4;
  data.lVal = value;
  return compartment->SetValue(client_id_, &data);
}
void TsfBackend::Execute(TsfRequest& request) noexcept {
  try {
    if (!active_) {
      request.result = E_UNEXPECTED;
      return;
    }
    switch (request.operation) {
      case TsfOperation::kStart:
        request.result = S_OK;
        break;
      case TsfOperation::kRead:
        request.result = store_->Publish();
        if (SUCCEEDED(request.result) && !store_->events().empty() &&
            store_->composing()) {
          RefreshAttributes();
          request.result = store_->Publish();
        }
        if (SUCCEEDED(request.result)) {
          request.edits.assign(store_->events().begin(),
                               store_->events().end());
          request.candidates = candidates_;
          request.selection = selection_;
          request.page_start = page_start_;
          request.total = total_;
        }
        break;
      case TsfOperation::kReset:
        request.result = store_->Reset(request.text, request.selection_start,
                                       request.selection_end,
                                       request.generation, request.read_only);
        break;
      case TsfOperation::kAcknowledge:
        store_->Acknowledge(request.acknowledgement);
        request.result = S_OK;
        break;
      case TsfOperation::kCancel:
        request.result = Cancel();
        break;
      case TsfOperation::kReconvert:
        request.result = Reconvert();
        break;
      case TsfOperation::kGetOpen:
        request.result = ReadCompartment(GUID_COMPARTMENT_KEYBOARD_OPENCLOSE,
                                         &request.value);
        break;
      case TsfOperation::kSetOpen:
        request.result = WriteCompartment(GUID_COMPARTMENT_KEYBOARD_OPENCLOSE,
                                          request.value != 0);
        break;
      case TsfOperation::kGetConversion:
        request.result =
            ReadCompartment(kConversionCompartment, &request.value);
        break;
      case TsfOperation::kSetConversion: {
        LONG current = 0;
        request.result = ReadCompartment(kConversionCompartment, &current);
        if (SUCCEEDED(request.result))
          request.result =
              WriteCompartment(kConversionCompartment,
                               (current & ~kConversionShapeMask) |
                                   (request.value & kConversionShapeMask));
        break;
      }
    }
  } catch (const std::bad_alloc&) {
    request.result = E_OUTOFMEMORY;
  }
}
void TsfBackend::RefreshAttributes() {
  if (!categories_ || !display_attributes_) return;
  auto* session = new (std::nothrow) AttributeSession(
      context_.get(), store_, categories_.get(), display_attributes_.get());
  if (!session) return;
  HRESULT result = E_FAIL;
  context_->RequestEditSession(client_id_, session, TF_ES_SYNC | TF_ES_READ,
                               &result);
  session->Release();
}
HRESULT TsfBackend::QueryInterface(REFIID id, void** output) {
  if (!output) return E_POINTER;
  *output = nullptr;
  if (id != IID_IUnknown && id != IID_ITfUIElementSink) return E_NOINTERFACE;
  *output = static_cast<ITfUIElementSink*>(this);
  AddRef();
  return S_OK;
}
ULONG TsfBackend::AddRef() { return ++references_; }
ULONG TsfBackend::Release() {
  const ULONG left = --references_;
  if (!left) delete this;
  return left;
}
HRESULT TsfBackend::BeginUIElement(DWORD id, BOOL* show) {
  if (!show) return E_POINTER;
  if (!ui_ || GetFocus() != window_) return S_OK;
  ComPtr<ITfUIElement> element;
  ComPtr<CandidateUi> candidates;
  ComPtr<ITfDocumentMgr> document;
  if (FAILED(ui_->GetUIElement(id, element.put())) ||
      FAILED(element->QueryInterface(
          kCandidateInterface, reinterpret_cast<void**>(candidates.put()))) ||
      FAILED(candidates->GetDocumentMgr(document.put())) ||
      document.get() != document_.get())
    return S_OK;
  candidate_id_ = id;
  *show = FALSE;
  return S_OK;
}
HRESULT TsfBackend::ReadCandidates(DWORD id) {
  ComPtr<ITfUIElement> element;
  ComPtr<CandidateUi> candidates;
  HRESULT result = ui_->GetUIElement(id, element.put());
  if (SUCCEEDED(result))
    result = element->QueryInterface(
        kCandidateInterface, reinterpret_cast<void**>(candidates.put()));
  if (FAILED(result)) return result;
  UINT count = 0, selected = 0, page_count = 0, page = 0;
  result = candidates->GetCount(&count);
  if (FAILED(result) || count > kMaximumCandidateCount)
    return FAILED(result) ? result : E_INVALIDARG;
  const HRESULT selection_result = candidates->GetSelection(&selected);
  if (FAILED(selection_result)) return selection_result;
  if (selection_result == S_OK && selected >= count) return E_INVALIDARG;
  result = candidates->GetPageIndex(nullptr, 0, &page_count);
  if (FAILED(result) || page_count > count)
    return FAILED(result) ? result : E_INVALIDARG;
  std::vector<UINT> pages(page_count);
  if (page_count) {
    result = candidates->GetPageIndex(pages.data(), page_count, &page_count);
    if (SUCCEEDED(result)) result = candidates->GetCurrentPage(&page);
    if (FAILED(result) || page >= pages.size())
      return FAILED(result) ? result : E_INVALIDARG;
  }
  const UINT start = page_count ? pages[page] : 0;
  const UINT end = page_count
                       ? (page + 1 < pages.size() ? pages[page + 1] : count)
                       : std::min(count, kDefaultPageSize);
  if (start > end || end > count) return E_INVALIDARG;
  std::vector<std::wstring> strings;
  strings.reserve(end - start);
  for (UINT index = start; index < end; ++index) {
    BSTR text = nullptr;
    result = candidates->GetString(index, &text);
    if (FAILED(result)) {
      SysFreeString(text);
      return result;
    }
    if (SysStringLen(text) > TextStore::kMaximumTextUnits) {
      SysFreeString(text);
      return E_INVALIDARG;
    }
    try {
      strings.emplace_back(text ? text : L"", SysStringLen(text));
    } catch (...) {
      SysFreeString(text);
      throw;
    }
    SysFreeString(text);
  }
  candidates_.swap(strings);
  total_ = count;
  page_start_ = start;
  selection_ =
      selection_result == S_OK ? static_cast<LONG>(selected) : kNoSelection;
  return S_OK;
}
HRESULT TsfBackend::UpdateUIElement(DWORD id) {
  if (id != candidate_id_) return S_OK;
  HRESULT result;
  try {
    result = ReadCandidates(id);
  } catch (const std::bad_alloc&) {
    result = E_OUTOFMEMORY;
  }
  if (FAILED(result)) {
    ComPtr<ITfUIElement> element;
    if (SUCCEEDED(ui_->GetUIElement(id, element.put()))) element->Show(TRUE);
    candidates_.clear();
    candidate_id_ = TF_INVALID_COOKIE;
  }
  return S_OK;
}
HRESULT TsfBackend::EndUIElement(DWORD id) {
  if (id == candidate_id_) {
    candidates_.clear();
    candidate_id_ = TF_INVALID_COOKIE;
    selection_ = kNoSelection;
    total_ = 0;
  }
  return S_OK;
}
}  // namespace neo_ime
