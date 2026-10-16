//
//  Created by ktiays on 2026/10/16.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#pragma once

#include "MainWindow.g.h"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "Engine.h"

namespace winrt::AsyncMusic::implementation {

/// The app's only window: the welcome screen, the speaker screen (finding a host, then what
/// this device plays for it) and diagnostics. The engine runs on background threads; the
/// window redraws from its snapshot on a short timer whenever something changed.
struct MainWindow : MainWindowT<MainWindow> {
    MainWindow();

    void OnBack(Windows::Foundation::IInspectable const& sender, Microsoft::UI::Xaml::RoutedEventArgs const& args);
    void OnShowDiagnostics(Windows::Foundation::IInspectable const& sender, Microsoft::UI::Xaml::RoutedEventArgs const& args);
    void OnJoin(Windows::Foundation::IInspectable const& sender, Microsoft::UI::Xaml::RoutedEventArgs const& args);
    void OnLeave(Windows::Foundation::IInspectable const& sender, Microsoft::UI::Xaml::RoutedEventArgs const& args);
    void OnOffsetChanged(Windows::Foundation::IInspectable const& sender, Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const& args);
    void OnRename(Windows::Foundation::IInspectable const& sender, Microsoft::UI::Xaml::RoutedEventArgs const& args);
    void OnCopyReport(Windows::Foundation::IInspectable const& sender, Microsoft::UI::Xaml::RoutedEventArgs const& args);
    void OnClearLog(Windows::Foundation::IInspectable const& sender, Microsoft::UI::Xaml::RoutedEventArgs const& args);

private:
    enum class Page { welcome, join, diagnostics };

    /// Value cells of the diagnostics rows.
    struct Rows {
        Microsoft::UI::Xaml::Controls::TextBlock role{nullptr}, device{nullptr}, deviceID{nullptr}, status{nullptr};
        Microsoft::UI::Xaml::Controls::TextBlock connectedTo{nullptr}, discovered{nullptr}, offset{nullptr}, skew{nullptr}, leaderClock{nullptr},
            fitJitter{nullptr}, minimumRoundTrip{nullptr}, asymmetry{nullptr}, rounds{nullptr}, lastRound{nullptr};
        Microsoft::UI::Xaml::Controls::TextBlock layout{nullptr}, channel{nullptr}, now{nullptr}, position{nullptr}, scheduleError{nullptr},
            correction{nullptr}, repositions{nullptr}, gaps{nullptr}, level{nullptr}, buffered{nullptr};
        Microsoft::UI::Xaml::Controls::TextBlock outputDevice{nullptr}, outputState{nullptr}, sampleRate{nullptr}, buffer{nullptr},
            streamLatency{nullptr}, timestampJitter{nullptr};
    };

    void setUpWindow();
    void buildDiagnostics();
    void show(Page page);
    void tick();
    void refresh(const asyncmusic::GroupSession::Snapshot& state);
    void refreshHosts(const asyncmusic::GroupSession::Snapshot& state);
    void refreshSpeaker(const asyncmusic::GroupSession::Snapshot& state);
    void refreshDiagnostics(const asyncmusic::GroupSession::Snapshot& state);
    void refreshLive(const asyncmusic::GroupSession::Snapshot& state);
    void drawCharts(const asyncmusic::GroupSession::Snapshot& state);
    std::string report() const;

    std::unique_ptr<asyncmusic::windows::Engine> engine_;
    Microsoft::UI::Dispatching::DispatcherQueueTimer timer_{nullptr};
    std::atomic<bool> dirty_{true};
    Page page_ = Page::welcome;
    Page pageBeforeDiagnostics_ = Page::welcome;
    uint64_t drawnRevision_ = 0;
    std::vector<std::wstring> shownLeaders_;
    bool isUpdatingOffset_ = false;
    Rows rows_;
};

}  // namespace winrt::AsyncMusic::implementation

namespace winrt::AsyncMusic::factory_implementation {

struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow> {};

}  // namespace winrt::AsyncMusic::factory_implementation
