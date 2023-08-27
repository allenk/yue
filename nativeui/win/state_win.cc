// Copyright 2016 Cheng Zhao. All rights reserved.
// Use of this source code is governed by the license that can be found in the
// LICENSE file.

#include "nativeui/state.h"

#include <shellscalingapi.h>

#include "base/base_paths.h"
#include "base/path_service.h"
#include "base/scoped_native_library.h"
#include "base/win/scoped_com_initializer.h"
#include "base/win/win_util.h"
#include "base/win/windows_version.h"
#include "nativeui/gfx/win/native_theme.h"
#include "nativeui/screen.h"
#include "nativeui/win/util/class_registrar.h"
#include "nativeui/win/util/gdiplus_holder.h"
#include "nativeui/win/util/scoped_ole_initializer.h"
#include "nativeui/win/util/subwin_holder.h"
#include "nativeui/win/util/timer_host.h"
#include "nativeui/win/util/tooltip_host.h"
#include "nativeui/win/util/tray_host.h"
#include "third_party/yoga/yoga/Yoga.h"

namespace nu {

namespace {

FARPROC LoadComBaseFunction(const char* function_name) {
  static HMODULE const handle =
      ::LoadLibraryEx(L"combase.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  return handle ? ::GetProcAddress(handle, function_name) : nullptr;
}

decltype(&::RoActivateInstance) GetRoActivateInstanceFunction() {
  static decltype(&::RoActivateInstance) const function =
      reinterpret_cast<decltype(&::RoActivateInstance)>(
          LoadComBaseFunction("RoActivateInstance"));
  return function;
}

decltype(&::RoGetActivationFactory) GetRoGetActivationFactoryFunction() {
  static decltype(&::RoGetActivationFactory) const function =
      reinterpret_cast<decltype(&::RoGetActivationFactory)>(
          LoadComBaseFunction("RoGetActivationFactory"));
  return function;
}

bool ResolveCoreWinRTDelayload() {
  return GetRoActivateInstanceFunction() && GetRoGetActivationFactoryFunction();
}

decltype(&::WindowsCreateString) GetWindowsCreateString() {
  static decltype(&::WindowsCreateString) const function =
      reinterpret_cast<decltype(&::WindowsCreateString)>(
          LoadComBaseFunction("WindowsCreateString"));
  return function;
}

decltype(&::WindowsDeleteString) GetWindowsDeleteString() {
  static decltype(&::WindowsDeleteString) const function =
      reinterpret_cast<decltype(&::WindowsDeleteString)>(
          LoadComBaseFunction("WindowsDeleteString"));
  return function;
}

decltype(&::WindowsGetStringRawBuffer) GetWindowsGetStringRawBuffer() {
  static decltype(&::WindowsGetStringRawBuffer) const function =
      reinterpret_cast<decltype(&::WindowsGetStringRawBuffer)>(
          LoadComBaseFunction("WindowsGetStringRawBuffer"));
  return function;
}

bool ResolveCoreWinRTStringDelayload() {
  static const bool load_succeeded = []() {
    return GetWindowsCreateString() && GetWindowsDeleteString() &&
                   GetWindowsGetStringRawBuffer();
  }();
  return load_succeeded;
}

}  // namespace

void State::PlatformInit() {
  base::win::EnableHighDPISupport();

  YGConfigSetPointScaleFactor(yoga_config(), Screen::GetDefaultScaleFactor());

  // Initialize Common Controls.
  INITCOMMONCONTROLSEX config;
  config.dwSize = sizeof(config);
  config.dwICC = ICC_WIN95_CLASSES;
  ::InitCommonControlsEx(&config);

  // Make sure TimerHost is initialized for main thread.
  if (GetMain() == this)
    GetTimerHost();

  gdiplus_holder_.reset(new GdiplusHolder);
}

void State::InitializeCOM() {
  if (!com_initializer_) {
    com_initializer_.reset(new base::win::ScopedCOMInitializer);
    ole_initializer_.reset(new ScopedOleInitializer);
  }
}

// TODO(zcbenz): This can be removed after stopping support Windows 7.
bool State::InitializeWinRT() {
  return ResolveCoreWinRTDelayload() &&
         ResolveCoreWinRTStringDelayload();
}

bool State::InitWebView2Loader() {
  if (!webview2_loader_) {
    // First try to do a global search.
    webview2_loader_.reset(
        new base::ScopedNativeLibrary(base::FilePath(L"WebView2Loader.dll")));
    // Then search from dir the contains current module.
    if (!webview2_loader_->is_valid()) {
      base::FilePath module_path;
      if (base::PathService::Get(base::FILE_MODULE, &module_path)) {
        auto dll_path = module_path.DirName().Append(L"WebView2Loader.dll");
        webview2_loader_.reset(new base::ScopedNativeLibrary(dll_path));
      }
    }
  }
  return webview2_loader_->is_valid();
}

base::ScopedNativeLibrary* State::GetWebView2Loader() {
  DCHECK(webview2_loader_ && webview2_loader_->is_valid());
  return webview2_loader_.get();
}

HWND State::GetSubwinHolder() {
  if (!subwin_holder_)
    subwin_holder_.reset(new SubwinHolder);
  return subwin_holder_->hwnd();
}

ClassRegistrar* State::GetClassRegistrar() {
  if (!class_registrar_)
    class_registrar_.reset(new ClassRegistrar);
  return class_registrar_.get();
}

NativeTheme* State::GetNativeTheme() {
  if (!native_theme_)
    native_theme_.reset(new NativeTheme);
  return native_theme_.get();
}

TrayHost* State::GetTrayHost() {
  if (!tray_host_)
    tray_host_.reset(new TrayHost);
  return tray_host_.get();
}

TimerHost* State::GetTimerHost() {
  CHECK_EQ(GetMain(), this);
  if (!timer_host_)
    timer_host_.reset(new TimerHost);
  return timer_host_.get();
}

TooltipHost* State::GetTooltipHost() {
  if (!tooltip_host_)
    tooltip_host_.reset(new TooltipHost);
  return tooltip_host_.get();
}

UINT State::GetNextCommandID() {
  return next_command_id_++;
}

}  // namespace nu
