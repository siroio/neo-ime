// Copyright 2026 neo-ime contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef NEO_IME_NATIVE_TSF_COMPAT_H_
#define NEO_IME_NATIVE_TSF_COMPAT_H_
#include "tsf_text_store.h"

namespace neo_ime {
// ABI declarations missing from MinGW. Verified against Windows
// SDK 10.0.26100.0 msctf.h/ctffunc.h and uuid.lib; these are Windows
// identifiers, not app settings.
inline constexpr GUID kConversionCompartment{
    0xccf05dd8,
    0x4a87,
    0x11d7,
    {0xa6, 0xe2, 0x00, 0x06, 0x5b, 0x84, 0x43, 0x5c}};
inline constexpr GUID kSystemFunctionProvider{
    0x9a698bb0,
    0x0f21,
    0x11d3,
    {0x8d, 0xf1, 0x00, 0x10, 0x5a, 0x27, 0x99, 0xb5}};
inline constexpr GUID kReconversionInterface{
    0x4cea93c0,
    0x0a58,
    0x11d3,
    {0x8d, 0xf0, 0x00, 0x10, 0x5a, 0x27, 0x99, 0xb5}};
inline constexpr GUID kCandidateInterface{
    0xea1ea138,
    0x19df,
    0x11d7,
    {0xa6, 0xd2, 0x00, 0x06, 0x5b, 0x84, 0x43, 0x5c}};

struct ReconversionFunction : IUnknown {
  virtual HRESULT STDMETHODCALLTYPE GetDisplayName(BSTR*) = 0;
  virtual HRESULT STDMETHODCALLTYPE QueryRange(ITfRange*, ITfRange**,
                                               BOOL*) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetReconversion(ITfRange*, IUnknown**) = 0;
  virtual HRESULT STDMETHODCALLTYPE Reconvert(ITfRange*) = 0;
};
struct CandidateUi : ITfUIElement {
  virtual HRESULT STDMETHODCALLTYPE GetUpdatedFlags(DWORD*) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetDocumentMgr(ITfDocumentMgr**) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetCount(UINT*) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetSelection(UINT*) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetString(UINT, BSTR*) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetPageIndex(UINT*, UINT, UINT*) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetPageIndex(UINT*, UINT) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetCurrentPage(UINT*) = 0;
};
}  // namespace neo_ime
#endif
