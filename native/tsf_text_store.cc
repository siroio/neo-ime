// Copyright 2026 neo-ime contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "tsf_text_store.h"

#include <olectl.h>

#include <algorithm>
#include <cstring>
#include <new>

namespace neo_ime {
namespace {
constexpr WCHAR kHighSurrogateFirst = 0xd800;
constexpr WCHAR kHighSurrogateLast = 0xdbff;
constexpr WCHAR kLowSurrogateFirst = 0xdc00;
constexpr WCHAR kLowSurrogateLast = 0xdfff;
constexpr LONG kDocumentEnd = -1;
constexpr int kRectanglePoints = 2;
}  // namespace

TextStore::~TextStore() {
  if (composition_) composition_->Release();
}
HRESULT TextStore::QueryInterface(REFIID id, void** result) {
  if (!result) return E_POINTER;
  *result = nullptr;
  if (id == IID_IUnknown || id == IID_ITextStoreACP)
    *result = static_cast<ITextStoreACP*>(this);
  else if (id == IID_ITfContextOwnerCompositionSink)
    *result = static_cast<ITfContextOwnerCompositionSink*>(this);
  else
    return E_NOINTERFACE;
  AddRef();
  return S_OK;
}
ULONG TextStore::AddRef() { return ++references_; }
ULONG TextStore::Release() {
  const ULONG remaining = --references_;
  if (!remaining) delete this;
  return remaining;
}
bool TextStore::HasLock(DWORD access) const {
  return (lock_flags_ & access) == access;
}
bool TextStore::ValidPosition(LONG position) const {
  if (position < 0 || static_cast<std::size_t>(position) > text_.size())
    return false;
  if (position == 0 || static_cast<std::size_t>(position) == text_.size())
    return true;
  const auto index = static_cast<std::size_t>(position);
  return !(text_[index - 1] >= kHighSurrogateFirst &&
           text_[index - 1] <= kHighSurrogateLast &&
           text_[index] >= kLowSurrogateFirst &&
           text_[index] <= kLowSurrogateLast);
}
bool TextStore::ValidRange(LONG start, LONG end) const {
  return start <= end && ValidPosition(start) && ValidPosition(end);
}
HRESULT TextStore::Reset(const std::wstring& text, LONG start, LONG end,
                         std::uint64_t generation, bool read_only) {
  if (lock_flags_ || composition_ || !events_.empty() || dirty_)
    return E_PENDING;
  if (text.size() > kMaximumTextUnits || start < 0 || start > end ||
      static_cast<std::size_t>(end) > text.size())
    return E_INVALIDARG;
  try {
    std::wstring previous = text_;
    text_ = text;
    if (!ValidRange(start, end)) {
      text_.swap(previous);
      return E_INVALIDARG;
    }
    const LONG old_end = static_cast<LONG>(previous.size());
    selection_ = {start, end, {TS_AE_END, FALSE}};
    generation_ = generation;
    attributes_.clear();
    const bool status_changed = read_only_ != read_only;
    read_only_ = read_only;
    if (sink_) {
      const TS_TEXTCHANGE change{0, old_end, static_cast<LONG>(text_.size())};
      if (sink_mask_ & TS_AS_TEXT_CHANGE) sink_->OnTextChange(0, &change);
      if (sink_mask_ & TS_AS_SEL_CHANGE) sink_->OnSelectionChange();
      if (status_changed && (sink_mask_ & TS_AS_STATUS_CHANGE))
        sink_->OnStatusChange(read_only_ ? TS_SD_READONLY : 0);
    }
    return S_OK;
  } catch (const std::bad_alloc&) {
    return E_OUTOFMEMORY;
  }
}
HRESULT TextStore::AdviseSink(REFIID id, IUnknown* unknown, DWORD mask) {
  if (!unknown) return E_INVALIDARG;
  if (id != IID_ITextStoreACPSink) return E_NOINTERFACE;
  ComPtr<IUnknown> incoming_identity;
  HRESULT result = unknown->QueryInterface(
      IID_IUnknown, reinterpret_cast<void**>(incoming_identity.put()));
  if (FAILED(result)) return result;
  if (sink_) {
    ComPtr<IUnknown> existing_identity;
    result = sink_->QueryInterface(
        IID_IUnknown, reinterpret_cast<void**>(existing_identity.put()));
    if (FAILED(result)) return result;
    if (existing_identity.get() != incoming_identity.get())
      return CONNECT_E_ADVISELIMIT;
  } else {
    result = unknown->QueryInterface(id, reinterpret_cast<void**>(sink_.put()));
    if (FAILED(result)) return result;
  }
  sink_mask_ = mask;
  return S_OK;
}
HRESULT TextStore::UnadviseSink(IUnknown* unknown) {
  if (!unknown || !sink_) return CONNECT_E_NOCONNECTION;
  ComPtr<IUnknown> current;
  ComPtr<IUnknown> requested;
  sink_->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(current.put()));
  unknown->QueryInterface(IID_IUnknown,
                          reinterpret_cast<void**>(requested.put()));
  if (!current || current.get() != requested.get())
    return CONNECT_E_NOCONNECTION;
  sink_.Reset();
  sink_mask_ = 0;
  return S_OK;
}
HRESULT TextStore::RequestLock(DWORD flags, HRESULT* session) {
  if (!session) return E_POINTER;
  *session = E_FAIL;
  if (!sink_) return E_UNEXPECTED;
  const DWORD access = flags & TS_LF_READWRITE;
  if (access != TS_LF_READ && access != TS_LF_READWRITE) return E_INVALIDARG;
  if (lock_flags_) {
    if (flags & TS_LF_SYNC)
      *session = TS_E_SYNCHRONOUS;
    else {
      pending_lock_ |= access;
      *session = TS_S_ASYNC;
    }
    return S_OK;
  }
  // Keep the sink alive even if a callback unadvises itself.
  ITextStoreACPSink* sink = sink_.get();
  sink->AddRef();
  lock_flags_ = access;
  *session = sink->OnLockGranted(flags);
  lock_flags_ = 0;
  while (pending_lock_ && sink_) {
    lock_flags_ = pending_lock_;
    pending_lock_ = 0;
    sink->OnLockGranted(lock_flags_);
    lock_flags_ = 0;
  }
  sink->Release();
  const HRESULT published = Publish();
  return FAILED(published) ? published : S_OK;
}
HRESULT TextStore::GetStatus(TS_STATUS* status) {
  if (!status) return E_POINTER;
  *status = {read_only_ ? DWORD{TS_SD_READONLY} : DWORD{0}, TS_SS_NOHIDDENTEXT};
  return S_OK;
}
HRESULT TextStore::QueryInsert(LONG start, LONG end, ULONG count,
                               LONG* out_start, LONG* out_end) {
  if (!out_start || !out_end) return E_POINTER;
  if (read_only_) return TS_E_READONLY;
  if (!ValidRange(start, end)) return TS_E_INVALIDPOS;
  if (count > kMaximumTextUnits -
                  (text_.size() - static_cast<std::size_t>(end - start)))
    return E_INVALIDARG;
  *out_start = start;
  *out_end = end;
  return S_OK;
}
HRESULT TextStore::GetSelection(ULONG index, ULONG count,
                                TS_SELECTION_ACP* selection, ULONG* fetched) {
  if (!fetched || (count && !selection)) return E_POINTER;
  *fetched = 0;
  if (!HasLock(TS_LF_READ)) return TS_E_NOLOCK;
  if (index != 0 && index != TS_DEFAULT_SELECTION) return TS_E_NOSELECTION;
  if (count) {
    *selection = selection_;
    *fetched = 1;
  }
  return S_OK;
}
HRESULT TextStore::SetSelection(ULONG count,
                                const TS_SELECTION_ACP* selection) {
  if (!HasLock(TS_LF_READWRITE)) return TS_E_NOLOCK;
  if (count != 1 || !selection) return E_INVALIDARG;
  if (!ValidRange(selection->acpStart, selection->acpEnd))
    return TS_E_INVALIDPOS;
  selection_ = *selection;
  dirty_ = true;
  return S_OK;
}
HRESULT TextStore::GetText(LONG start, LONG end, WCHAR* plain, ULONG capacity,
                           ULONG* copied, TS_RUNINFO* runs, ULONG run_capacity,
                           ULONG* run_count, LONG* next) {
  if (!copied || !run_count || !next || (capacity && !plain) ||
      (run_capacity && !runs))
    return E_POINTER;
  *copied = 0;
  *run_count = 0;
  *next = start;
  if (!HasLock(TS_LF_READ)) return TS_E_NOLOCK;
  if (end == kDocumentEnd) end = static_cast<LONG>(text_.size());
  if (start < 0 || end < start || static_cast<std::size_t>(end) > text_.size())
    return TS_E_INVALIDPOS;
  const ULONG available = static_cast<ULONG>(end - start);
  const ULONG count =
      capacity ? std::min(capacity, available) : (run_capacity ? available : 0);
  if (capacity && count)
    std::memcpy(plain, text_.data() + start, count * sizeof(WCHAR));
  *copied = capacity ? count : 0;
  if (run_capacity && count) {
    *runs = {count, TS_RT_PLAIN};
    *run_count = 1;
  }
  *next = start + static_cast<LONG>(count);
  return S_OK;
}
HRESULT TextStore::SetText(DWORD, LONG start, LONG end, const WCHAR* text,
                           ULONG count, TS_TEXTCHANGE* change) {
  if (!HasLock(TS_LF_READWRITE)) return TS_E_NOLOCK;
  if (!change || (count && !text)) return E_POINTER;
  LONG checked_start, checked_end;
  const HRESULT valid =
      QueryInsert(start, end, count, &checked_start, &checked_end);
  if (FAILED(valid)) return valid;
  try {
    text_.replace(static_cast<std::size_t>(start),
                  static_cast<std::size_t>(end - start), text ? text : L"",
                  count);
    const LONG new_end = start + static_cast<LONG>(count);
    *change = {start, end, new_end};
    selection_ = {new_end, new_end, {TS_AE_END, FALSE}};
    dirty_ = true;
    return S_OK;
  } catch (const std::bad_alloc&) {
    return E_OUTOFMEMORY;
  }
}
HRESULT TextStore::InsertTextAtSelection(DWORD flags, const WCHAR* text,
                                         ULONG count, LONG* start, LONG* end,
                                         TS_TEXTCHANGE* change) {
  if (!HasLock((flags & TS_IAS_QUERYONLY) ? TS_LF_READ : TS_LF_READWRITE))
    return TS_E_NOLOCK;
  if (flags & TS_IAS_QUERYONLY)
    return QueryInsert(selection_.acpStart, selection_.acpEnd, count, start,
                       end);
  const LONG insertion = selection_.acpStart;
  const HRESULT result =
      SetText(0, insertion, selection_.acpEnd, text, count, change);
  if (SUCCEEDED(result) && !(flags & TS_IAS_NOQUERY)) {
    if (start) *start = insertion;
    if (end) *end = change->acpNewEnd;
  }
  return result;
}
HRESULT TextStore::GetFormattedText(LONG, LONG, IDataObject** result) {
  if (result) *result = nullptr;
  return E_NOTIMPL;
}
HRESULT TextStore::GetEmbedded(LONG, REFGUID, REFIID, IUnknown** result) {
  if (result) *result = nullptr;
  return E_NOTIMPL;
}
HRESULT TextStore::QueryInsertEmbedded(const GUID*, const FORMATETC*,
                                       BOOL* result) {
  if (!result) return E_POINTER;
  *result = FALSE;
  return S_OK;
}
HRESULT TextStore::InsertEmbedded(DWORD, LONG, LONG, IDataObject*,
                                  TS_TEXTCHANGE*) {
  return E_NOTIMPL;
}
HRESULT TextStore::InsertEmbeddedAtSelection(DWORD, IDataObject*, LONG*, LONG*,
                                             TS_TEXTCHANGE*) {
  return E_NOTIMPL;
}
HRESULT TextStore::RequestSupportedAttrs(DWORD, ULONG, const TS_ATTRID*) {
  return S_OK;
}
HRESULT TextStore::RequestAttrsAtPosition(LONG, ULONG, const TS_ATTRID*,
                                          DWORD) {
  return S_OK;
}
HRESULT TextStore::RequestAttrsTransitioningAtPosition(LONG, ULONG,
                                                       const TS_ATTRID*,
                                                       DWORD) {
  return S_OK;
}
HRESULT TextStore::FindNextAttrTransition(LONG start, LONG halt, ULONG,
                                          const TS_ATTRID*, DWORD, LONG* next,
                                          BOOL* found, LONG* offset) {
  if (!next || !found || !offset) return E_POINTER;
  if (!ValidRange(start, halt)) return TS_E_INVALIDPOS;
  *next = halt;
  *found = FALSE;
  *offset = 0;
  return S_OK;
}
HRESULT TextStore::RetrieveRequestedAttrs(ULONG, TS_ATTRVAL*, ULONG* fetched) {
  if (!fetched) return E_POINTER;
  *fetched = 0;
  return S_OK;
}
HRESULT TextStore::GetEndACP(LONG* end) {
  if (!end) return E_POINTER;
  if (!HasLock(TS_LF_READ)) return TS_E_NOLOCK;
  *end = static_cast<LONG>(text_.size());
  return S_OK;
}
HRESULT TextStore::GetActiveView(TsViewCookie* view) {
  if (!view) return E_POINTER;
  *view = kView;
  return S_OK;
}
HRESULT TextStore::GetACPFromPoint(TsViewCookie, const POINT*, DWORD, LONG*) {
  return TS_E_NOLAYOUT;
}
HRESULT TextStore::GetTextExt(TsViewCookie view, LONG start, LONG end,
                              RECT* rectangle, BOOL* clipped) {
  if (!rectangle || !clipped) return E_POINTER;
  if (!HasLock(TS_LF_READ)) return TS_E_NOLOCK;
  if (view != kView || !ValidRange(start, end)) return E_INVALIDARG;
  POINT anchor = anchor_;
  if (!ClientToScreen(window_, &anchor)) return TS_E_NOLAYOUT;
  // shortcut: one caret rectangle for this session, expand for per-glyph hit
  // testing.
  *rectangle = {anchor.x, anchor.y, anchor.x + 1, anchor.y + line_height_};
  *clipped = FALSE;
  return S_OK;
}
HRESULT TextStore::GetScreenExt(TsViewCookie view, RECT* rectangle) {
  if (!rectangle) return E_POINTER;
  if (view != kView) return E_INVALIDARG;
  if (!GetClientRect(window_, rectangle)) return TS_E_NOLAYOUT;
  MapWindowPoints(window_, nullptr, reinterpret_cast<POINT*>(rectangle),
                  kRectanglePoints);
  return S_OK;
}
HRESULT TextStore::GetWnd(TsViewCookie view, HWND* window) {
  if (!window) return E_POINTER;
  if (view != kView) return E_INVALIDARG;
  *window = window_;
  return S_OK;
}
void TextStore::SetAnchor(POINT anchor, int height) {
  anchor_ = anchor;
  line_height_ = height;
  if (sink_ && (sink_mask_ & TS_AS_LAYOUT_CHANGE))
    sink_->OnLayoutChange(TS_LC_CHANGE, kView);
}
HRESULT TextStore::OnStartComposition(ITfCompositionView* composition,
                                      BOOL* accepted) {
  if (!accepted || !composition) return E_POINTER;
  *accepted = !composition_;
  if (*accepted) {
    composition_ = composition;
    composition_->AddRef();
    dirty_ = true;
  }
  return S_OK;
}
HRESULT TextStore::OnUpdateComposition(ITfCompositionView*, ITfRange*) {
  dirty_ = true;
  return S_OK;
}
HRESULT TextStore::OnEndComposition(ITfCompositionView* composition) {
  if (composition_ == composition) {
    composition_->Release();
    composition_ = nullptr;
    dirty_ = true;
  }
  return S_OK;
}
HRESULT TextStore::CompositionExtent(LONG* start, LONG* length) {
  *start = 0;
  *length = 0;
  if (!composition_) return S_OK;
  ComPtr<ITfRange> range;
  ComPtr<ITfRangeACP> acp;
  HRESULT result = composition_->GetRange(range.put());
  if (SUCCEEDED(result))
    result = range->QueryInterface(IID_ITfRangeACP,
                                   reinterpret_cast<void**>(acp.put()));
  if (SUCCEEDED(result)) result = acp->GetExtent(start, length);
  if (SUCCEEDED(result) &&
      (*length < 0 || *start < 0 ||
       static_cast<std::size_t>(*start) > text_.size() ||
       static_cast<std::size_t>(*length) >
           text_.size() - static_cast<std::size_t>(*start)))
    return E_UNEXPECTED;
  return result;
}
HRESULT TextStore::Publish() {
  if (lock_flags_ || !dirty_) return S_OK;
  try {
    TextEdit edit;
    edit.generation = generation_;
    edit.revision = revision_ + 1;
    edit.text = text_;
    edit.cursor = selection_.style.ase == TS_AE_START ? selection_.acpStart
                                                      : selection_.acpEnd;
    edit.composing = composing();
    edit.attributes = attributes_;
    edit.attributes.resize(text_.size(), TF_ATTR_INPUT);
    const HRESULT result =
        CompositionExtent(&edit.composition_start, &edit.composition_length);
    if (FAILED(result)) return result;
    events_.push_back(std::move(edit));
    ++revision_;
    dirty_ = false;
    return S_OK;
  } catch (const std::bad_alloc&) {
    return E_OUTOFMEMORY;
  }
}
void TextStore::Acknowledge(std::uint64_t revision) {
  while (!events_.empty() && events_.front().revision <= revision)
    events_.pop_front();
}
void TextStore::SetAttributes(std::vector<BYTE> attributes) {
  if (attributes_ != attributes) {
    attributes_ = std::move(attributes);
    dirty_ = true;
  }
}
}  // namespace neo_ime
