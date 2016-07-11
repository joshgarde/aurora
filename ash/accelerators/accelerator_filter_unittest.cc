// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ui/wm/core/accelerator_filter.h"

#include <memory>

#include "ash/accelerators/accelerator_controller.h"
#include "ash/accelerators/accelerator_delegate.h"
#include "ash/common/shell_window_ids.h"
#include "ash/common/wm/window_state.h"
#include "ash/shell.h"
#include "ash/test/ash_test_base.h"
#include "ash/test/ash_test_helper.h"
#include "ash/test/test_screenshot_delegate.h"
#include "ash/test/test_session_state_delegate.h"
#include "ash/wm/window_state_aura.h"
#include "ash/wm/window_util.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "ui/aura/client/aura_constants.h"
#include "ui/aura/test/aura_test_base.h"
#include "ui/aura/test/test_windows.h"
#include "ui/aura/window.h"
#include "ui/base/accelerators/accelerator_history.h"
#include "ui/events/event.h"
#include "ui/events/test/event_generator.h"
#include "ui/gfx/geometry/rect.h"

namespace ash {
namespace test {

typedef AshTestBase AcceleratorFilterTest;

// Tests if AcceleratorFilter works without a focused window.
TEST_F(AcceleratorFilterTest, TestFilterWithoutFocus) {
  const TestScreenshotDelegate* delegate = GetScreenshotDelegate();
  EXPECT_EQ(0, delegate->handle_take_screenshot_count());

  ui::test::EventGenerator generator(Shell::GetPrimaryRootWindow());
  // AcceleratorController calls ScreenshotDelegate::HandleTakeScreenshot() when
  // VKEY_PRINT is pressed. See kAcceleratorData[] in accelerator_controller.cc.
  generator.PressKey(ui::VKEY_PRINT, 0);
  EXPECT_EQ(1, delegate->handle_take_screenshot_count());
  generator.ReleaseKey(ui::VKEY_PRINT, 0);
  EXPECT_EQ(1, delegate->handle_take_screenshot_count());
}

// Tests if AcceleratorFilter works as expected with a focused window.
TEST_F(AcceleratorFilterTest, TestFilterWithFocus) {
  aura::test::TestWindowDelegate test_delegate;
  std::unique_ptr<aura::Window> window(
      CreateTestWindowInShellWithDelegate(&test_delegate, -1, gfx::Rect()));
  wm::ActivateWindow(window.get());

  const TestScreenshotDelegate* delegate = GetScreenshotDelegate();
  EXPECT_EQ(0, delegate->handle_take_screenshot_count());

  // AcceleratorFilter should ignore the key events since the root window is
  // not focused.
  ui::test::EventGenerator generator(Shell::GetPrimaryRootWindow());
  generator.PressKey(ui::VKEY_PRINT, 0);
  EXPECT_EQ(0, delegate->handle_take_screenshot_count());
  generator.ReleaseKey(ui::VKEY_PRINT, 0);
  EXPECT_EQ(0, delegate->handle_take_screenshot_count());

  // Reset window before |test_delegate| gets deleted.
  window.reset();
}

// Tests if AcceleratorFilter ignores the flag for Caps Lock.
TEST_F(AcceleratorFilterTest, TestCapsLockMask) {
  const TestScreenshotDelegate* delegate = GetScreenshotDelegate();
  EXPECT_EQ(0, delegate->handle_take_screenshot_count());

  ui::test::EventGenerator generator(Shell::GetPrimaryRootWindow());
  generator.PressKey(ui::VKEY_PRINT, 0);
  EXPECT_EQ(1, delegate->handle_take_screenshot_count());
  generator.ReleaseKey(ui::VKEY_PRINT, 0);
  EXPECT_EQ(1, delegate->handle_take_screenshot_count());

  // Check if AcceleratorFilter ignores the mask for Caps Lock. Note that there
  // is no ui::EF_ mask for Num Lock.
  generator.PressKey(ui::VKEY_PRINT, ui::EF_CAPS_LOCK_ON);
  EXPECT_EQ(2, delegate->handle_take_screenshot_count());
  generator.ReleaseKey(ui::VKEY_PRINT, ui::EF_CAPS_LOCK_ON);
  EXPECT_EQ(2, delegate->handle_take_screenshot_count());
}

#if defined(OS_CHROMEOS)
// Tests if special hardware keys like brightness and volume are consumed as
// expected by the shell.
TEST_F(AcceleratorFilterTest, CanConsumeSystemKeys) {
  std::unique_ptr<ui::AcceleratorHistory> accelerator_history(
      new ui::AcceleratorHistory());
  ::wm::AcceleratorFilter filter(
      std::unique_ptr<::wm::AcceleratorDelegate>(new AcceleratorDelegate),
      accelerator_history.get());
  aura::Window* root_window = Shell::GetPrimaryRootWindow();

  // Normal keys are not consumed.
  ui::KeyEvent press_a(ui::ET_KEY_PRESSED, ui::VKEY_A, ui::EF_NONE);
  {
    ui::Event::DispatcherApi dispatch_helper(&press_a);
    dispatch_helper.set_target(root_window);
  }
  filter.OnKeyEvent(&press_a);
  EXPECT_FALSE(press_a.stopped_propagation());

  // System keys are directly consumed.
  ui::KeyEvent press_mute(ui::ET_KEY_PRESSED, ui::VKEY_VOLUME_MUTE,
                          ui::EF_NONE);
  {
    ui::Event::DispatcherApi dispatch_helper(&press_mute);
    dispatch_helper.set_target(root_window);
  }
  filter.OnKeyEvent(&press_mute);
  EXPECT_TRUE(press_mute.stopped_propagation());

  // Setting a window property on the target allows system keys to pass through.
  std::unique_ptr<aura::Window> window(CreateTestWindowInShellWithId(1));
  wm::GetWindowState(window.get())->set_can_consume_system_keys(true);
  ui::KeyEvent press_volume_up(ui::ET_KEY_PRESSED, ui::VKEY_VOLUME_UP,
                               ui::EF_NONE);
  ui::Event::DispatcherApi dispatch_helper(&press_volume_up);
  dispatch_helper.set_target(window.get());
  filter.OnKeyEvent(&press_volume_up);
  EXPECT_FALSE(press_volume_up.stopped_propagation());

  // System keys pass through to a child window if the parent (top level)
  // window has the property set.
  std::unique_ptr<aura::Window> child(CreateTestWindowInShellWithId(2));
  window->AddChild(child.get());
  dispatch_helper.set_target(child.get());
  filter.OnKeyEvent(&press_volume_up);
  EXPECT_FALSE(press_volume_up.stopped_propagation());
}

TEST_F(AcceleratorFilterTest, SearchKeyShortcutsAreAlwaysHandled) {
  TestSessionStateDelegate* session_state_delegate =
      AshTestHelper::GetTestSessionStateDelegate();
  EXPECT_FALSE(session_state_delegate->IsScreenLocked());

  ui::test::EventGenerator generator(Shell::GetPrimaryRootWindow());

  // We can lock the screen (Search+L) if a window is not present.
  generator.PressKey(ui::VKEY_L, ui::EF_COMMAND_DOWN);
  generator.ReleaseKey(ui::VKEY_L, ui::EF_COMMAND_DOWN);
  EXPECT_TRUE(session_state_delegate->IsScreenLocked());
  UnblockUserSession();
  EXPECT_FALSE(session_state_delegate->IsScreenLocked());

  // Search+L is processed when the app_list target visibility is false.
  Shell::GetInstance()->DismissAppList();
  EXPECT_FALSE(Shell::GetInstance()->GetAppListTargetVisibility());
  generator.PressKey(ui::VKEY_L, ui::EF_COMMAND_DOWN);
  generator.ReleaseKey(ui::VKEY_L, ui::EF_COMMAND_DOWN);
  EXPECT_TRUE(session_state_delegate->IsScreenLocked());
  UnblockUserSession();
  EXPECT_FALSE(session_state_delegate->IsScreenLocked());

  // Search+L is also processed when there is a full screen window.
  aura::test::TestWindowDelegate window_delegate;
  std::unique_ptr<aura::Window> window(CreateTestWindowInShellWithDelegate(
      &window_delegate, 0, gfx::Rect(200, 200)));
  window->SetProperty(aura::client::kShowStateKey, ui::SHOW_STATE_FULLSCREEN);
  generator.PressKey(ui::VKEY_L, ui::EF_COMMAND_DOWN);
  generator.ReleaseKey(ui::VKEY_L, ui::EF_COMMAND_DOWN);
  EXPECT_TRUE(session_state_delegate->IsScreenLocked());
  UnblockUserSession();
  EXPECT_FALSE(session_state_delegate->IsScreenLocked());
}
#endif  // defined(OS_CHROMEOS)

}  // namespace test
}  // namespace ash
