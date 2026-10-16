//
//  Created by ktiays on 2026/10/16.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#pragma once

#include "App.xaml.g.h"

namespace winrt::AsyncMusic::implementation {

struct App : AppT<App> {
    App();

    void OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const& args);

private:
    Microsoft::UI::Xaml::Window window_{nullptr};
};

}  // namespace winrt::AsyncMusic::implementation
