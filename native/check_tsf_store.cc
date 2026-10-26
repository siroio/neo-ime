// Copyright 2026 neo-ime contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "tsf_text_store.h"

#include <cassert>
#include <cstdio>
#include <functional>

namespace {
class Sink final : public ITextStoreACPSink {
 public:
  std::function<HRESULT(DWORD)> on_lock;
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** result) override {
    if (!result) return E_POINTER;
    *result = nullptr;
    if (id != IID_IUnknown && id != IID_ITextStoreACPSink) return E_NOINTERFACE;
    *result = static_cast<ITextStoreACPSink*>(this);
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
  ULONG STDMETHODCALLTYPE Release() override { return --references_; }
  HRESULT STDMETHODCALLTYPE OnTextChange(DWORD, const TS_TEXTCHANGE*) override {
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE OnSelectionChange() override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnLayoutChange(TsLayoutCode,
                                           TsViewCookie) override {
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE OnStatusChange(DWORD) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnAttrsChange(LONG, LONG, ULONG,
                                          const TS_ATTRID*) override {
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE OnLockGranted(DWORD flags) override {
    return on_lock(flags);
  }
  HRESULT STDMETHODCALLTYPE OnStartEditTransaction() override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnEndEditTransaction() override { return S_OK; }

 private:
  ULONG references_ = 1;
};
}  // namespace

int main() {
  using neo_ime::TextStore;
  auto* store = new TextStore(nullptr);
  Sink sink;
  assert(store->Reset(L"before", 0, 6, 1) == S_OK);
  LONG length = 0;
  assert(store->GetEndACP(&length) == TS_E_NOLOCK);
  assert(store->AdviseSink(IID_ITextStoreACPSink, &sink, TS_AS_ALL_SINKS) ==
         S_OK);
  int calls = 0;
  sink.on_lock = [&](DWORD flags) {
    ++calls;
    assert(store->GetEndACP(&length) == S_OK && length == 6);
    if (flags == TS_LF_READ) {
      TS_TEXTCHANGE change{};
      assert(store->SetText(0, 0, 6, L"after", 5, &change) == TS_E_NOLOCK);
      HRESULT nested = E_FAIL;
      assert(store->RequestLock(TS_LF_SYNC | TS_LF_READWRITE, &nested) == S_OK);
      assert(nested == TS_E_SYNCHRONOUS);
      assert(store->RequestLock(TS_LF_READWRITE, &nested) == S_OK);
      assert(nested == TS_S_ASYNC);
    } else {
      TS_TEXTCHANGE change{};
      assert(store->SetText(0, -1, 6, L"x", 1, &change) == TS_E_INVALIDPOS);
      assert(store->SetText(0, 0, 6, L"after", 5, &change) == S_OK);
      assert(change.acpStart == 0 && change.acpOldEnd == 6 &&
             change.acpNewEnd == 5);
    }
    return S_OK;
  };
  HRESULT session = E_FAIL;
  assert(store->RequestLock(TS_LF_READ, &session) == S_OK && session == S_OK);
  assert(calls == 2);
  assert(store->events().size() == 1 &&
         store->events().front().text == L"after");
  assert(store->Reset(L"", 0, 0, 2) == E_PENDING);
  store->Acknowledge(store->events().back().revision);
  assert(store->Reset(L"\U0001F600", 0, 2, 2) == S_OK);
  sink.on_lock = [&](DWORD) {
    TS_TEXTCHANGE change{};
    assert(store->SetText(0, 1, 2, L"x", 1, &change) == TS_E_INVALIDPOS);
    assert(store->SetText(0, 0, 2, L"a", 1, &change) == S_OK);
    return S_OK;
  };
  assert(store->RequestLock(TS_LF_READWRITE, &session) == S_OK);
  sink.on_lock = [&](DWORD) {
    TS_TEXTCHANGE change{};
    return store->SetText(0, 1, 1, L"b", 1, &change);
  };
  assert(store->RequestLock(TS_LF_READWRITE, &session) == S_OK);
  assert(store->events().size() == 2);
  assert(store->events().front().text == L"a" &&
         store->events().back().text == L"ab");
  store->Acknowledge(store->events().back().revision);
  assert(store->Reset(L"locked", 0, 0, 3, true) == S_OK);
  sink.on_lock = [&](DWORD) {
    TS_STATUS status{};
    assert(store->GetStatus(&status) == S_OK &&
           (status.dwDynamicFlags & TS_SD_READONLY));
    TS_TEXTCHANGE change{};
    assert(store->SetText(0, 0, 0, L"x", 1, &change) == TS_E_READONLY);
    return S_OK;
  };
  assert(store->RequestLock(TS_LF_READWRITE, &session) == S_OK);
  assert(store->UnadviseSink(&sink) == S_OK);
  store->Release();
  std::puts(
      "NEO_IME_TSF_STORE=PASS lock-upgrade invalid-range utf16 ordered-edits "
      "reset-guard");
}
