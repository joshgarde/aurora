// Copyright (c) 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/**
 * Namespace for keyboard utility functions.
 */
var keyboard = {};

/**
 * Swallows keypress and keyup events of arrow keys.
 * @param {Event} event Raised event.
 * @private
 */
keyboard.onKeyIgnore_ = function(event) {
  event = /** @type {KeyboardEvent} */(event);

  if (event.ctrlKey || event.shiftKey || event.altKey || event.metaKey)
    return;

  if (event.key == 'ArrowLeft' ||
      event.key == 'ArrowRight' ||
      event.key == 'ArrowUp' ||
      event.key == 'ArrowDown') {
    event.stopPropagation();
    event.preventDefault();
  }
};

/**
 * Converts arrow keys into tab/shift-tab key events.
 * @param {Event} event Raised event.
 * @private
 */
keyboard.onKeyDown_ = function(event) {
  event = /** @type {KeyboardEvent} */(event);

  if (event.ctrlKey || event.shiftKey || event.altKey || event.metaKey)
    return;

  // This file also gets embedded inside of the CfM/hotrod enrollment webview.
  // Events will bubble down into the webview, which means that the event
  // handler from the webui will steal the events meant for the webview. So we
  // have to disable the webui handler if the active element is the webview.
  //
  // $ is defined differently depending on how this file gets executed; we have
  // to use document.getElementById to get consistent behavior.
  //
  // See crbug.com/543865.
  if (document.activeElement ===
      document.getElementById('oauth-enroll-auth-view'))
    return;

  var needsUpDownKeys = event.target.classList.contains('needs-up-down-keys');

  if (event.key == 'ArrowLeft' ||
      (!needsUpDownKeys && event.key == 'ArrowUp')) {
    keyboard.raiseKeyFocusPrevious(document.activeElement);
    event.stopPropagation();
    event.preventDefault();
  } else if (event.key == 'ArrowRight' ||
             (!needsUpDownKeys && event.key == 'ArrowDown')) {
    keyboard.raiseKeyFocusNext(document.activeElement);
    event.stopPropagation();
    event.preventDefault();
  }
};

/**
 * Raises tab/shift-tab keyboard events.
 * @param {HTMLElement} element Element that should receive the event.
 * @param {string} eventType Keyboard event type.
 * @param {boolean} shift True if shift should be on.
 * @private
 */
keyboard.raiseTabKeyEvent_ = function(element, eventType, shift) {
  var event = document.createEvent('KeyboardEvent');
  event.initKeyboardEvent(
      eventType,
      true,  // canBubble
      true,  // cancelable
      window,
      'U+0009',
      0,  // keyLocation
      false,  // ctrl
      false,  // alt
      shift,  // shift
      false);  // meta
  element.dispatchEvent(event);
};

/**
 * Raises shift+tab keyboard events to focus previous element.
 * @param {HTMLElement} element Element that should receive the event.
 */
keyboard.raiseKeyFocusPrevious = function(element) {
  keyboard.raiseTabKeyEvent_(element, 'keydown', true);
  keyboard.raiseTabKeyEvent_(element, 'keypress', true);
  keyboard.raiseTabKeyEvent_(element, 'keyup', true);
};

/**
 * Raises tab keyboard events to focus next element.
 * @param {HTMLElement} element Element that should receive the event.
 */
keyboard.raiseKeyFocusNext = function(element) {
  keyboard.raiseTabKeyEvent_(element, 'keydown', false);
  keyboard.raiseTabKeyEvent_(element, 'keypress', false);
  keyboard.raiseTabKeyEvent_(element, 'keyup', false);
};

/**
 * Initializes event handling for arrow keys driven focus flow.
 */
keyboard.initializeKeyboardFlow = function() {
  document.addEventListener('keydown',
      keyboard.onKeyDown_, true);
  document.addEventListener('keypress',
      keyboard.onKeyIgnore_, true);
  document.addEventListener('keyup',
      keyboard.onKeyIgnore_, true);
};
