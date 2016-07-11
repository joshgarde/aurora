// Copyright (c) 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef UI_AURA_CLIENT_CURSOR_CLIENT_OBSERVER_H_
#define UI_AURA_CLIENT_CURSOR_CLIENT_OBSERVER_H_

#include "ui/aura/aura_export.h"
#include "ui/base/cursor/cursor.h"

namespace aura {
namespace client {

class AURA_EXPORT CursorClientObserver {
 public:
  virtual void OnCursorVisibilityChanged(bool is_visible) {}
  virtual void OnCursorSetChanged(ui::CursorSetType cursor_set) {}

 protected:
  virtual ~CursorClientObserver() {}
};

}  // namespace client
}  // namespace aura

#endif  // UI_AURA_CLIENT_CURSOR_CLIENT_OBSERVER_H_
