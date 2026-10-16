//
//  Created by ktiays on 2026/10/16.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#include "pch.h"

#include "MainWindow.xaml.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

#include <asyncmusic/host_clock.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <format>

#include "Settings.h"

using namespace std::chrono_literals;
using namespace winrt;
using Windows::Foundation::IInspectable;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace Microsoft::UI::Xaml::Media;
using asyncmusic::GroupSession;

namespace winrt::AsyncMusic::implementation {

namespace {

hstring text(const std::string& utf8) {
    return to_hstring(utf8);
}

Brush themeBrush(const wchar_t* key) {
    return Application::Current().Resources().Lookup(box_value(key)).as<Brush>();
}

SolidColorBrush colorBrush(uint8_t r, uint8_t g, uint8_t b) {
    return SolidColorBrush(Microsoft::UI::ColorHelper::FromArgb(255, r, g, b));
}

enum class Tone { good, pending, bad, neutral };

Brush toneBrush(Tone tone) {
    switch (tone) {
    case Tone::good: return themeBrush(L"SystemFillColorSuccessBrush");
    case Tone::pending: return themeBrush(L"SystemFillColorCautionBrush");
    case Tone::bad: return themeBrush(L"SystemFillColorCriticalBrush");
    case Tone::neutral: break;
    }
    return themeBrush(L"TextFillColorTertiaryBrush");
}

const char* layoutTitle(asyncmusic::SpeakerLayout layout) {
    switch (layout) {
    case asyncmusic::SpeakerLayout::everywhere: return "Everywhere";
    case asyncmusic::SpeakerLayout::stereoPair: return "Stereo Pair";
    }
    return "Unknown";
}

const char* channelTitle(asyncmusic::SpeakerChannel channel) {
    switch (channel) {
    case asyncmusic::SpeakerChannel::full: return "Full Mix";
    case asyncmusic::SpeakerChannel::left: return "Left";
    case asyncmusic::SpeakerChannel::right: return "Right";
    }
    return "Unknown";
}

const char* roleTitle(GroupSession::Role role) {
    switch (role) {
    case GroupSession::Role::off: return "Off";
    case GroupSession::Role::leader: return "Leader";
    case GroupSession::Role::follower: return "Follower";
    }
    return "?";
}

const char* verdictTitle(asyncmusic::ClockVerdict verdict) {
    switch (verdict) {
    case asyncmusic::ClockVerdict::accepted: return "accepted";
    case asyncmusic::ClockVerdict::noReplies: return "no replies";
    case asyncmusic::ClockVerdict::slowRoundTrip: return "slow round trip";
    case asyncmusic::ClockVerdict::outlier: return "outlier";
    case asyncmusic::ClockVerdict::reset: return "reset";
    }
    return "?";
}

std::string timeOfDay(std::chrono::system_clock::time_point date) {
    const std::time_t time = std::chrono::system_clock::to_time_t(date);
    std::tm local{};
    localtime_s(&local, &time);
    char buffer[16];
    std::strftime(buffer, sizeof(buffer), "%H:%M:%S", &local);
    return buffer;
}

/// A row of a diagnostics card: the label on the left, the returned value on the right.
TextBlock addRow(StackPanel const& panel, hstring const& label) {
    Grid row;
    row.ColumnSpacing(12);
    ColumnDefinition labelColumn;
    labelColumn.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
    ColumnDefinition valueColumn;
    valueColumn.Width(GridLengthHelper::Auto());
    row.ColumnDefinitions().Append(labelColumn);
    row.ColumnDefinitions().Append(valueColumn);
    TextBlock name;
    name.Text(label);
    TextBlock value;
    value.Foreground(themeBrush(L"TextFillColorSecondaryBrush"));
    value.TextAlignment(TextAlignment::Right);
    value.IsTextSelectionEnabled(true);
    Grid::SetColumn(value, 1);
    row.Children().Append(name);
    row.Children().Append(value);
    panel.Children().Append(row);
    return value;
}

std::string minutesAndSeconds(double seconds) {
    const int total = static_cast<int>(std::max(seconds, 0.0));
    return std::format("{}:{:02}", total / 60, total % 60);
}

Style resourceStyle(const wchar_t* key) {
    return Application::Current().Resources().Lookup(box_value(key)).as<Style>();
}

/// A speaker badge's color: blue for left, orange for right, gray for the full mix.
Brush channelBrush(asyncmusic::SpeakerChannel channel) {
    switch (channel) {
    case asyncmusic::SpeakerChannel::left: return colorBrush(0x00, 0x78, 0xD4);
    case asyncmusic::SpeakerChannel::right: return colorBrush(0xF7, 0x63, 0x0C);
    case asyncmusic::SpeakerChannel::full: break;
    }
    return themeBrush(L"SubtleFillColorSecondaryBrush");
}

void addHeader(StackPanel const& panel, hstring const& title) {
    TextBlock header;
    header.Text(title);
    header.Style(Application::Current().Resources().Lookup(box_value(L"BodyStrongTextBlockStyle")).as<Style>());
    panel.Children().Append(header);
}

}  // namespace

MainWindow::MainWindow() {
    InitializeComponent();
    engine_ = std::make_unique<asyncmusic::windows::Engine>();
    engine_->onChange = [this] { dirty_ = true; };
    setUpWindow();
    buildDiagnostics();

    isUpdatingOffset_ = true;
    offsetSlider().Value(engine_->manualOffset());
    isUpdatingOffset_ = false;
    nameBox().Text(text(asyncmusic::windows::settings::customDeviceName().value_or("")));
    nameBox().PlaceholderText(text(engine_->session().snapshot().identity.name));

    timer_ = DispatcherQueue().CreateTimer();
    timer_.Interval(100ms);
    timer_.Tick([this](auto&&, auto&&) { tick(); });
    timer_.Start();

    Closed([this](auto&&, auto&&) {
        timer_.Stop();
        engine_.reset();
    });
    show(Page::welcome);
    openFileFromCommandLine();
    tick();
}

/// `AsyncMusic.exe <audio file>` (as with "Open with"): host, with that file chosen.
void MainWindow::openFileFromCommandLine() {
    int count = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (arguments == nullptr) return;
    std::wstring path = count >= 2 ? arguments[1] : L"";
    LocalFree(arguments);
    const DWORD attributes = path.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) return;
    engine_->setRole(GroupSession::Role::leader);
    show(Page::host);
    engine_->loadTrack(path);
}

void MainWindow::setUpWindow() {
    ExtendsContentIntoTitleBar(true);
    SetTitleBar(dragRegion());
    auto window = AppWindow();
    window.TitleBar().PreferredHeightOption(Microsoft::UI::Windowing::TitleBarHeightOption::Tall);

    // Size the window in effective pixels for the monitor it opens on.
    HWND handle = nullptr;
    this->try_as<::IWindowNative>()->get_WindowHandle(&handle);
    const double scale = (handle != nullptr ? GetDpiForWindow(handle) : 96) / 96.0;
    window.Resize({static_cast<int32_t>(520 * scale), static_cast<int32_t>(780 * scale)});
    if (auto presenter = window.Presenter().try_as<Microsoft::UI::Windowing::OverlappedPresenter>()) {
        presenter.PreferredMinimumWidth(static_cast<int32_t>(400 * scale));
        presenter.PreferredMinimumHeight(static_cast<int32_t>(520 * scale));
    }
}

void MainWindow::buildDiagnostics() {
    addHeader(generalSection(), L"General");
    rows_.role = addRow(generalSection(), L"Role");
    rows_.device = addRow(generalSection(), L"Device");
    rows_.deviceID = addRow(generalSection(), L"Device ID");
    rows_.status = addRow(generalSection(), L"Status");

    addHeader(leaderSection(), L"Group");
    rows_.followers = addRow(leaderSection(), L"Followers");
    rows_.members = addRow(leaderSection(), L"Members");
    rows_.clockPort = addRow(leaderSection(), L"Clock port (UDP)");
    rows_.advertising = addRow(leaderSection(), L"Advertising");

    addHeader(estimateSection(), L"Clock");
    rows_.connectedTo = addRow(estimateSection(), L"Connected to");
    rows_.discovered = addRow(estimateSection(), L"Discovered hosts");
    rows_.offset = addRow(estimateSection(), L"Offset");
    rows_.skew = addRow(estimateSection(), L"Skew");
    rows_.leaderClock = addRow(estimateSection(), L"Leader clock");
    rows_.leaderClock.FontFamily(FontFamily(L"Cascadia Mono, Consolas"));
    rows_.fitJitter = addRow(estimateSection(), L"Fit jitter (RMS)");
    rows_.minimumRoundTrip = addRow(estimateSection(), L"Min RTT");
    rows_.asymmetry = addRow(estimateSection(), L"Asymmetry bound");
    rows_.rounds = addRow(estimateSection(), L"Rounds");
    rows_.lastRound = addRow(estimateSection(), L"Last round");

    addHeader(playbackSection(), L"Speakers and Playback");
    rows_.layout = addRow(playbackSection(), L"Layout");
    rows_.channel = addRow(playbackSection(), L"This device plays");
    rows_.now = addRow(playbackSection(), L"Now");
    rows_.position = addRow(playbackSection(), L"Position");
    rows_.scheduleError = addRow(playbackSection(), L"Schedule error");
    rows_.correction = addRow(playbackSection(), L"Rate correction");
    rows_.repositions = addRow(playbackSection(), L"Repositions");
    rows_.gaps = addRow(playbackSection(), L"Stream gaps");
    rows_.level = addRow(playbackSection(), L"Output level");
    rows_.buffered = addRow(playbackSection(), L"Buffered ahead");

    addHeader(outputRows(), L"Output");
    rows_.outputDevice = addRow(outputRows(), L"Device");
    rows_.outputState = addRow(outputRows(), L"State");
    rows_.sampleRate = addRow(outputRows(), L"Sample rate");
    rows_.buffer = addRow(outputRows(), L"Buffer");
    rows_.streamLatency = addRow(outputRows(), L"Stream latency");
    rows_.timestampJitter = addRow(outputRows(), L"Timestamp jitter");
}

// MARK: - Navigation

void MainWindow::show(Page page) {
    if (page == Page::diagnostics && page_ != Page::diagnostics) pageBeforeDiagnostics_ = page_;
    page_ = page;
    welcomePage().Visibility(page == Page::welcome ? Visibility::Visible : Visibility::Collapsed);
    joinPage().Visibility(page == Page::join ? Visibility::Visible : Visibility::Collapsed);
    hostPage().Visibility(page == Page::host ? Visibility::Visible : Visibility::Collapsed);
    diagnosticsPage().Visibility(page == Page::diagnostics ? Visibility::Visible : Visibility::Collapsed);
    backButton().Visibility(page == Page::diagnostics ? Visibility::Visible : Visibility::Collapsed);
    diagnosticsButton().Visibility(page == Page::diagnostics ? Visibility::Collapsed : Visibility::Visible);
    titleText().Text(page == Page::diagnostics ? L"AsyncMusic · Diagnostics" : L"AsyncMusic");
    dirty_ = true;
    drawnRevision_ = 0;
}

void MainWindow::OnBack(IInspectable const&, RoutedEventArgs const&) {
    show(pageBeforeDiagnostics_);
}

void MainWindow::OnShowDiagnostics(IInspectable const&, RoutedEventArgs const&) {
    show(Page::diagnostics);
}

void MainWindow::OnJoin(IInspectable const&, RoutedEventArgs const&) {
    shownLeaders_.clear();
    hostList().Children().Clear();
    engine_->setRole(GroupSession::Role::follower);
    show(Page::join);
}

void MainWindow::OnLeave(IInspectable const&, RoutedEventArgs const&) {
    engine_->setRole(GroupSession::Role::off);
    show(Page::welcome);
}

void MainWindow::OnHost(IInspectable const&, RoutedEventArgs const&) {
    shownDevices_.clear();
    engine_->setRole(GroupSession::Role::leader);
    show(Page::host);
}

void MainWindow::OnChooseMusic(IInspectable const&, RoutedEventArgs const&) {
    if (const auto path = pickAudioFile()) {
        engine_->loadTrack(*path);
        dirty_ = true;
    }
}

void MainWindow::OnPlayStop(IInspectable const&, RoutedEventArgs const&) {
    const auto state = engine_->session().snapshot();
    if (state.playback.isPlaying() || state.isPreparingPlayback) {
        engine_->session().stopPlayback();
    } else if (state.selectedTrack) {
        engine_->session().play(state.selectedTrack->id);
    }
}

void MainWindow::OnEverywhere(IInspectable const&, RoutedEventArgs const&) {
    engine_->session().setSpeakerLayout(asyncmusic::SpeakerLayout::everywhere);
}

void MainWindow::OnStereoPair(IInspectable const&, RoutedEventArgs const&) {
    engine_->session().setSpeakerLayout(asyncmusic::SpeakerLayout::stereoPair);
}

void MainWindow::OnLeftDeviceChanged(IInspectable const&, SelectionChangedEventArgs const&) {
    assignFromPicker(leftDevice(), asyncmusic::SpeakerChannel::left);
}

void MainWindow::OnRightDeviceChanged(IInspectable const&, SelectionChangedEventArgs const&) {
    assignFromPicker(rightDevice(), asyncmusic::SpeakerChannel::right);
}

void MainWindow::assignFromPicker(ComboBox const& picker, asyncmusic::SpeakerChannel channel) {
    if (isUpdatingPickers_ || !engine_) return;
    const int index = picker.SelectedIndex();
    if (index < 0 || index >= static_cast<int>(candidates_.size())) return;
    engine_->session().assign(candidates_[static_cast<size_t>(index)].first, channel);
}

void MainWindow::OnSwap(IInspectable const&, RoutedEventArgs const&) {
    engine_->session().swapChannels();
}

void MainWindow::OnTestChannels(IInspectable const&, RoutedEventArgs const&) {
    if (engine_->session().snapshot().playback.source == asyncmusic::kPlaybackSourceChannelTest) {
        engine_->session().stopPlayback();
    } else {
        engine_->session().play(asyncmusic::kPlaybackSourceChannelTest);
    }
}

/// Asks for an audio file with the system's file dialog.
std::optional<std::wstring> MainWindow::pickAudioFile() {
    com_ptr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.put())))) return std::nullopt;
    const COMDLG_FILTERSPEC filters[] = {
        {L"Audio files", L"*.mp3;*.m4a;*.aac;*.wav;*.flac;*.aif;*.aiff;*.wma;*.caf"},
        {L"All files", L"*.*"},
    };
    dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
    dialog->SetTitle(L"Choose Music");
    FILEOPENDIALOGOPTIONS options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM);
    HWND owner = nullptr;
    this->try_as<::IWindowNative>()->get_WindowHandle(&owner);
    if (FAILED(dialog->Show(owner))) return std::nullopt;
    com_ptr<IShellItem> item;
    PWSTR path = nullptr;
    if (FAILED(dialog->GetResult(item.put())) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return std::nullopt;
    std::wstring result = path;
    CoTaskMemFree(path);
    return result;
}

void MainWindow::OnOffsetChanged(IInspectable const&, Controls::Primitives::RangeBaseValueChangedEventArgs const& args) {
    if (isUpdatingOffset_ || !engine_) return;
    engine_->setManualOffset(args.NewValue());
    dirty_ = true;
}

void MainWindow::OnRename(IInspectable const&, RoutedEventArgs const&) {
    std::string name = to_string(nameBox().Text());
    name.erase(0, name.find_first_not_of(" \t"));
    name.erase(name.find_last_not_of(" \t") + 1);
    engine_->rename(name);
    nameBox().Text(text(name));
    dirty_ = true;
}

void MainWindow::OnClearLog(IInspectable const&, RoutedEventArgs const&) {
    engine_->session().clearLog();
}

void MainWindow::OnCopyReport(IInspectable const&, RoutedEventArgs const&) {
    Windows::ApplicationModel::DataTransfer::DataPackage package;
    package.SetText(text(report()));
    Windows::ApplicationModel::DataTransfer::Clipboard::SetContent(package);
}

// MARK: - Refresh

void MainWindow::tick() {
    if (!engine_) return;
    // A glitch in drawing must never take the app, and with it the group's audio, down: an
    // exception escaping this timer would end the process.
    try {
        // Nothing to draw while minimized; the engine plays on, and everything is redrawn on
        // restore. (A minimized window also reports a caption inset that is not a valid width.)
        const auto presenter = AppWindow().Presenter().try_as<Microsoft::UI::Windowing::OverlappedPresenter>();
        if (presenter && presenter.State() == Microsoft::UI::Windowing::OverlappedPresenterState::Minimized) {
            dirty_ = true;
            return;
        }
        // The caption buttons' width depends on the DPI; keep the title bar clear of them.
        if (auto root = Content().XamlRoot(); root && root.RasterizationScale() > 0) {
            const double inset = AppWindow().TitleBar().RightInset() / root.RasterizationScale();
            if (std::isfinite(inset) && inset >= 0 && inset < 1000) {
                captionButtonsColumn().Width(GridLengthHelper::FromPixels(inset));
            }
        }
        const GroupSession::Snapshot state = engine_->session().snapshot();
        if (dirty_.exchange(false)) {
            refresh(state);
        }
        refreshLive(state);
    } catch (hresult_error const& error) {
        OutputDebugStringW((L"AsyncMusic: drawing failed: " + error.message() + L"\n").c_str());
    }
}

void MainWindow::refresh(const GroupSession::Snapshot& state) {
    // Back to the welcome screen when the session leaves its group. (Only on that change: a role
    // just requested is still off here until the session has applied it.)
    if (state.role == GroupSession::Role::off && shownRole_ != GroupSession::Role::off && (page_ == Page::join || page_ == Page::host)) {
        show(Page::welcome);
    }
    shownRole_ = state.role;
    deviceNameText().Text(text(std::format("This device appears to others as “{}”.", state.identity.name)));
    refreshHosts(state);
    refreshSpeaker(state);
    if (state.role == GroupSession::Role::leader) refreshHost(state);
    if (page_ == Page::diagnostics) refreshDiagnostics(state);
}

void MainWindow::refreshHosts(const GroupSession::Snapshot& state) {
    const bool connected = state.connectedLeader.has_value();
    hostListPanel().Visibility(connected ? Visibility::Collapsed : Visibility::Visible);
    speakerPanel().Visibility(connected ? Visibility::Visible : Visibility::Collapsed);
    leaveButton().Content(box_value(connected ? L"Leave" : L"Cancel"));
    if (connected) return;

    const auto leaders = engine_->browser().leaders();
    const std::string error = engine_->browser().error();
    searchingPanel().Visibility(leaders.empty() ? Visibility::Visible : Visibility::Collapsed);
    searchingText().Text(error.empty() ? hstring(L"Looking for hosts…") : text(error));

    std::vector<std::wstring> instances;
    for (const auto& leader : leaders) instances.push_back(leader.instance);
    if (instances == shownLeaders_) return;
    shownLeaders_ = instances;
    hostList().Children().Clear();
    for (const auto& leader : leaders) {
        Button row;
        row.Style(Application::Current().Resources().Lookup(box_value(L"CardButtonStyle")).as<Style>());
        Grid content;
        content.ColumnSpacing(14);
        for (auto width : {GridLengthHelper::Auto(), GridLengthHelper::FromValueAndType(1, GridUnitType::Star), GridLengthHelper::Auto()}) {
            ColumnDefinition column;
            column.Width(width);
            content.ColumnDefinitions().Append(column);
        }
        Border tile;
        tile.Style(Application::Current().Resources().Lookup(box_value(L"IconTileStyle")).as<Style>());
        FontIcon icon;
        icon.Glyph(L"");
        icon.FontSize(20);
        icon.Foreground(themeBrush(L"AccentTextFillColorPrimaryBrush"));
        tile.Child(icon);
        StackPanel labels;
        labels.Spacing(2);
        labels.VerticalAlignment(VerticalAlignment::Center);
        TextBlock name;
        name.Text(text(leader.name));
        name.Style(Application::Current().Resources().Lookup(box_value(L"BodyStrongTextBlockStyle")).as<Style>());
        TextBlock hint;
        hint.Text(L"Click to join");
        hint.Style(Application::Current().Resources().Lookup(box_value(L"SecondaryCaptionStyle")).as<Style>());
        labels.Children().Append(name);
        labels.Children().Append(hint);
        Grid::SetColumn(labels, 1);
        FontIcon chevron;
        chevron.Glyph(L"");
        chevron.FontSize(12);
        chevron.Foreground(themeBrush(L"TextFillColorTertiaryBrush"));
        Grid::SetColumn(chevron, 2);
        content.Children().Append(tile);
        content.Children().Append(labels);
        content.Children().Append(chevron);
        row.Content(content);
        Automation::AutomationProperties::SetName(row, text(leader.name));
        row.Click([this, leader](auto&&, auto&&) {
            engine_->follow(leader);
            dirty_ = true;
        });
        hostList().Children().Append(row);
    }
}

void MainWindow::refreshSpeaker(const GroupSession::Snapshot& state) {
    if (!state.connectedLeader) return;
    const std::string& host = *state.connectedLeader;
    const bool isPlaying = state.playback.isPlaying();

    switch (state.channel) {
    case asyncmusic::SpeakerChannel::left:
        badgeCircle().Fill(colorBrush(0x00, 0x78, 0xD4));
        badgeText().Text(L"L");
        badgeText().Foreground(colorBrush(0xFF, 0xFF, 0xFF));
        speakerTitle().Text(L"Left Speaker");
        break;
    case asyncmusic::SpeakerChannel::right:
        badgeCircle().Fill(colorBrush(0xF7, 0x63, 0x0C));
        badgeText().Text(L"R");
        badgeText().Foreground(colorBrush(0xFF, 0xFF, 0xFF));
        speakerTitle().Text(L"Right Speaker");
        break;
    case asyncmusic::SpeakerChannel::full:
        badgeCircle().Fill(themeBrush(L"SubtleFillColorSecondaryBrush"));
        badgeText().Text(L"LR");
        badgeText().Foreground(themeBrush(L"TextFillColorSecondaryBrush"));
        speakerTitle().Text(L"Speaker");
        break;
    }
    badgeText().FontSize(state.channel == asyncmusic::SpeakerChannel::full ? 38 : 50);
    playingBadge().Visibility(isPlaying ? Visibility::Visible : Visibility::Collapsed);

    std::string explanation;
    if (state.channel != asyncmusic::SpeakerChannel::full) {
        explanation = std::format("This device plays the {} channel of a stereo pair with {}.", state.channel == asyncmusic::SpeakerChannel::left ? "left" : "right", host);
    } else if (state.remoteLayout == asyncmusic::SpeakerLayout::stereoPair) {
        explanation = "Plays the full mix until both sides of the stereo pair are connected.";
    } else {
        explanation = std::format("This device plays the full mix in sync with {}.", host);
    }
    speakerExplanation().Text(text(explanation));
    hostValue().Text(text(host));

    using State = asyncmusic::ClockSyncFollower::State;
    hstring sync = L"Connecting…";
    Tone syncTone = Tone::pending;
    if (state.followerState == State::syncing) {
        sync = state.clockMap ? L"In sync" : L"Syncing…";
        syncTone = state.clockMap ? Tone::good : Tone::pending;
    } else if (state.followerState == State::reconnecting) {
        sync = L"Reconnecting…";
    } else if (state.followerState == State::failed) {
        sync = L"Connection failed";
        syncTone = Tone::bad;
    }
    syncValue().Text(sync);
    syncDot().Fill(toneBrush(syncTone));

    playbackValue().Text(!isPlaying ? L"Waiting for the host" : (state.isWaitingForAudio ? L"Receiving audio…" : L"Playing"));
    playbackDot().Fill(toneBrush(isPlaying ? (state.isWaitingForAudio ? Tone::pending : Tone::good) : Tone::neutral));

    const auto output = engine_->output().status();
    outputInfoBar().IsOpen(!output.error.empty());
    outputInfoBar().Message(text(output.error));
}

void MainWindow::refreshHost(const GroupSession::Snapshot& state) {
    const bool isPlaying = state.playback.isPlaying();
    const bool isLoading = engine_->isLoadingTrack();

    // Now playing.
    std::string title = state.selectedTrack ? state.selectedTrack->name : "No Music Chosen";
    if (isPlaying) {
        title = state.playback.source == asyncmusic::kPlaybackSourceMetronome ? "Metronome"
            : state.playback.source == asyncmusic::kPlaybackSourceChannelTest ? "Left & Right Test"
            : state.selectedTrack && state.selectedTrack->id == state.playback.source ? state.selectedTrack->name
            : "Music";
    }
    trackTitle().Text(text(title));
    std::string subtitle;
    if (isLoading) {
        subtitle = "Loading…";
    } else if (state.isPreparingPlayback) {
        subtitle = "Getting speakers ready…";
    } else if (isPlaying) {
        const auto active = std::count_if(state.members.begin(), state.members.end(), [](const auto& member) { return member.isActive; });
        subtitle = active == 0 ? "Playing on this device" : std::format("Playing on {} devices", active + 1);
    } else {
        subtitle = state.selectedTrack ? "Ready to play" : "Choose a song to start.";
    }
    trackSubtitle().Text(text(subtitle));
    loadingRing().IsActive(isLoading);
    artworkIcon().Visibility(isLoading ? Visibility::Collapsed : Visibility::Visible);
    artworkIcon().Glyph(isPlaying ? L"\uE995" : L"\uEC4F");
    chooseMusicButton().IsEnabled(!isLoading);
    const bool canStop = isPlaying || state.isPreparingPlayback;
    playIcon().Glyph(canStop ? L"\uE71A" : L"\uE768");
    Automation::AutomationProperties::SetName(playButton(), canStop ? L"Stop" : L"Play");
    playButton().IsEnabled(canStop || state.selectedTrack.has_value());

    std::string problem = engine_->loadError();
    if (problem.empty()) problem = engine_->advertisingError();
    hostInfoBar().IsOpen(!problem.empty());
    hostInfoBar().Message(text(problem));

    // Sound.
    const bool stereo = state.speakers.layout == asyncmusic::SpeakerLayout::stereoPair;
    everywhereCheck().Visibility(stereo ? Visibility::Collapsed : Visibility::Visible);
    stereoCheck().Visibility(stereo ? Visibility::Visible : Visibility::Collapsed);
    stereoCard().Visibility(stereo ? Visibility::Visible : Visibility::Collapsed);

    // Stereo pair.
    refreshDevicePickers(state);
    const auto slot = [&](asyncmusic::SpeakerChannel channel, Shapes::Ellipse const& badge, TextBlock const& caption) {
        const auto device = state.speakers.assignments.find(channel);
        const bool present = device != state.speakers.assignments.end() && state.isDevicePresent(device->second);
        badge.Fill(channelBrush(channel));
        badge.Opacity(present ? 1.0 : 0.4);
        caption.Text(device == state.speakers.assignments.end() ? hstring(L" ")
            : device->second == state.identity.id ? hstring(L"This device")
            : present ? hstring(L"Connected") : hstring(L"Not connected"));
    };
    slot(asyncmusic::SpeakerChannel::left, leftBadge(), leftCaption());
    slot(asyncmusic::SpeakerChannel::right, rightBadge(), rightCaption());
    if (state.isLayoutComplete) {
        stereoStatusIcon().Glyph(L"\uE930");
        stereoStatusIcon().Foreground(toneBrush(Tone::good));
        stereoStatus().Text(L"Stereo is on. For the best effect, sit midway between the two devices.");
    } else {
        stereoStatusIcon().Glyph(L"\uE946");
        stereoStatusIcon().Foreground(themeBrush(L"TextFillColorSecondaryBrush"));
        std::string waiting = "Choose a device for each side. Meanwhile every device plays the full mix.";
        if (state.members.empty()) {
            waiting = "Open AsyncMusic on a second device on this network and choose Join as Speaker. Until then, this device plays the full mix.";
        } else {
            for (const auto& [channel, device] : state.speakers.assignments) {
                if (!state.isDevicePresent(device)) {
                    waiting = "Waiting for " + state.nameOfDevice(device).value_or("the other device") + ". Meanwhile every device plays the full mix.";
                    break;
                }
            }
        }
        stereoStatus().Text(text(waiting));
    }
    const bool testing = state.playback.source == asyncmusic::kPlaybackSourceChannelTest;
    testIcon().Glyph(testing ? L"\uE71A" : L"\uE7F5");
    testText().Text(testing ? L"Stop Test" : L"Test Left & Right");

    // Devices, rebuilt only when something about them changed.
    std::string devices = std::to_string(static_cast<int>(state.channel)) + state.identity.name;
    for (const auto& member : state.members) {
        const auto channel = state.activeChannels.find(member.id);
        devices += std::format("|{}{}{}", member.name, member.isActive ? 1 : 0, channel == state.activeChannels.end() ? 0 : static_cast<int>(channel->second));
    }
    if (devices == shownDevices_) return;
    shownDevices_ = devices;
    deviceList().Children().Clear();
    const auto addDevice = [&](const std::string& name, const std::string& detail, const wchar_t* glyph, bool isActive, asyncmusic::SpeakerChannel channel) {
        if (deviceList().Children().Size() > 0) {
            Shapes::Rectangle divider;
            divider.Style(resourceStyle(L"DividerStyle"));
            deviceList().Children().Append(divider);
        }
        Grid row;
        row.ColumnSpacing(12);
        for (auto width : {GridLengthHelper::FromPixels(28), GridLengthHelper::FromValueAndType(1, GridUnitType::Star), GridLengthHelper::Auto()}) {
            ColumnDefinition column;
            column.Width(width);
            row.ColumnDefinitions().Append(column);
        }
        FontIcon icon;
        icon.Glyph(glyph);
        icon.FontSize(18);
        icon.Foreground(themeBrush(L"TextFillColorSecondaryBrush"));
        StackPanel labels;
        labels.Spacing(2);
        Grid::SetColumn(labels, 1);
        TextBlock title;
        title.Text(text(name));
        title.TextTrimming(TextTrimming::CharacterEllipsis);
        StackPanel status;
        status.Orientation(Orientation::Horizontal);
        status.Spacing(6);
        Shapes::Ellipse dot;
        dot.Width(8);
        dot.Height(8);
        dot.VerticalAlignment(VerticalAlignment::Center);
        dot.Fill(toneBrush(isActive ? Tone::good : Tone::pending));
        TextBlock caption;
        caption.Text(text(detail));
        caption.Style(resourceStyle(L"SecondaryCaptionStyle"));
        status.Children().Append(dot);
        status.Children().Append(caption);
        labels.Children().Append(title);
        labels.Children().Append(status);
        Grid badge;
        Grid::SetColumn(badge, 2);
        Shapes::Ellipse circle;
        circle.Width(30);
        circle.Height(30);
        circle.Fill(channelBrush(channel));
        circle.Opacity(isActive ? 1.0 : 0.4);
        TextBlock letters;
        letters.Text(channel == asyncmusic::SpeakerChannel::left ? L"L" : channel == asyncmusic::SpeakerChannel::right ? L"R" : L"LR");
        letters.FontSize(channel == asyncmusic::SpeakerChannel::full ? 10 : 13);
        letters.FontWeight(Microsoft::UI::Text::FontWeights::Bold());
        letters.Foreground(channel == asyncmusic::SpeakerChannel::full ? themeBrush(L"TextFillColorSecondaryBrush") : colorBrush(0xFF, 0xFF, 0xFF));
        letters.HorizontalAlignment(HorizontalAlignment::Center);
        letters.VerticalAlignment(VerticalAlignment::Center);
        badge.Children().Append(circle);
        badge.Children().Append(letters);
        row.Children().Append(icon);
        row.Children().Append(labels);
        row.Children().Append(badge);
        deviceList().Children().Append(row);
    };
    addDevice(state.identity.name, "This device · Host", L"\uE7F8", true, state.channel);
    for (const auto& member : state.members) {
        const auto channel = state.activeChannels.find(member.id);
        addDevice(member.name, member.isActive ? "Connected" : "Not responding", L"\uE7F5", member.isActive,
            channel == state.activeChannels.end() ? asyncmusic::SpeakerChannel::full : channel->second);
    }
    if (state.members.empty()) {
        TextBlock hint;
        hint.Text(L"No other devices yet. Open AsyncMusic on another device on this network and choose Join as Speaker.");
        hint.Style(resourceStyle(L"SecondaryCaptionStyle"));
        deviceList().Children().Append(hint);
    }
}

/// Offers this device and every member for each side, selecting the assigned one.
void MainWindow::refreshDevicePickers(const GroupSession::Snapshot& state) {
    std::vector<std::pair<uint64_t, std::string>> candidates{{state.identity.id, state.identity.name}};
    for (const auto& member : state.members) candidates.emplace_back(member.id, member.name);
    isUpdatingPickers_ = true;
    for (auto [picker, channel] : {std::pair{leftDevice(), asyncmusic::SpeakerChannel::left}, std::pair{rightDevice(), asyncmusic::SpeakerChannel::right}}) {
        if (candidates != candidates_) {
            picker.Items().Clear();
            for (const auto& [id, name] : candidates) picker.Items().Append(box_value(text(name)));
        }
        int selected = -1;
        const auto device = state.speakers.assignments.find(channel);
        for (size_t i = 0; device != state.speakers.assignments.end() && i < candidates.size(); ++i) {
            if (candidates[i].first == device->second) selected = static_cast<int>(i);
        }
        if (picker.SelectedIndex() != selected) picker.SelectedIndex(selected);
    }
    candidates_ = candidates;
    isUpdatingPickers_ = false;
}

void MainWindow::refreshDiagnostics(const GroupSession::Snapshot& state) {
    rows_.role.Text(text(roleTitle(state.role)));
    rows_.device.Text(text(state.identity.name));
    rows_.deviceID.Text(text(std::format("{:016x}", state.identity.id)));
    rows_.status.Text(text(state.status));

    const bool leading = state.role == GroupSession::Role::leader;
    leaderSection().Visibility(leading ? Visibility::Visible : Visibility::Collapsed);
    rows_.followers.Text(text(std::to_string(state.followerCount)));
    std::string members;
    for (const auto& member : state.members) {
        members += (members.empty() ? "" : "\n") + member.name + (member.isActive ? "" : " (not responding)");
    }
    rows_.members.Text(text(members.empty() ? "–" : members));
    rows_.clockPort.Text(state.leaderPort != 0 ? text(std::to_string(state.leaderPort)) : hstring(L"–"));
    const std::string advertisingError = engine_->advertisingError();
    rows_.advertising.Text(text(!advertisingError.empty() ? advertisingError : state.leaderPort != 0 ? "As “" + state.identity.name + "”" : "–"));

    const bool following = state.role == GroupSession::Role::follower;
    estimateSection().Visibility(following ? Visibility::Visible : Visibility::Collapsed);
    chartsSection().Visibility(following && !state.rounds.empty() ? Visibility::Visible : Visibility::Collapsed);
    rows_.connectedTo.Text(text(state.connectedLeader.value_or("–")));
    rows_.discovered.Text(text(std::to_string(engine_->browser().leaders().size())));
    if (const auto& round = state.latestRound) {
        rows_.offset.Text(round->map ? text(std::format("{:.3f} ms", static_cast<double>(round->map->offsetAtLocal(round->localTime)) / 1e6)) : hstring(L"–"));
        rows_.skew.Text(round->map ? text(std::format("{:+.2f} ppm", round->map->skewPPM())) : hstring(L"–"));
        rows_.fitJitter.Text(text(std::format("{:.0f} µs", round->residualRMS / 1e3)));
        rows_.minimumRoundTrip.Text(round->minimumRoundTrip > 0 ? text(std::format("{:.2f} ms", static_cast<double>(round->minimumRoundTrip) / 1e6)) : hstring(L"–"));
        rows_.asymmetry.Text(round->minimumRoundTrip > 0 ? text(std::format("± {:.2f} ms", static_cast<double>(round->minimumRoundTrip) / 2e6)) : hstring(L"–"));
        rows_.rounds.Text(text(std::format("{} accepted · {} rejected · {} in window", round->acceptedCount, round->rejectedCount, round->windowCount)));
        rows_.lastRound.Text(text(std::format("{}/{} replies · {}", round->probesReceived, round->probesSent, verdictTitle(round->verdict))));
    } else {
        for (auto& row : {rows_.offset, rows_.skew, rows_.fitJitter, rows_.minimumRoundTrip, rows_.asymmetry, rows_.rounds, rows_.lastRound}) row.Text(L"–");
    }

    playbackSection().Visibility(state.role != GroupSession::Role::off ? Visibility::Visible : Visibility::Collapsed);
    outputSection().Visibility(state.role != GroupSession::Role::off ? Visibility::Visible : Visibility::Collapsed);
    rows_.layout.Text(text(layoutTitle(leading ? state.speakers.layout : state.remoteLayout)));
    rows_.channel.Text(text(channelTitle(state.channel)));
    rows_.now.Text(text(state.playback.isPlaying() ? engine_->session().describe(state.playback.source) + (state.isWaitingForAudio ? " (waiting for audio)" : "") : "Stopped"));

    const auto output = engine_->output().status();
    rows_.outputDevice.Text(text(output.deviceName.empty() ? "–" : output.deviceName));
    rows_.outputState.Text(text(output.isRunning ? "Running" : (output.error.empty() ? "Stopped" : output.error)));
    rows_.sampleRate.Text(output.sampleRate > 0 ? text(std::format("{:.0f} Hz", output.sampleRate)) : hstring(L"–"));
    rows_.buffer.Text(output.bufferDuration > 0 ? text(std::format("{:.1f} ms", output.bufferDuration * 1e3)) : hstring(L"–"));
    rows_.streamLatency.Text(text(std::format("{:.1f} ms", output.streamLatency * 1e3)));
    rows_.timestampJitter.Text(text(std::format("{:.0f} µs", output.timestampJitter)));
    offsetValue().Text(text(std::format("{:+.1f} ms", engine_->manualOffset())));

    // The log, newest first.
    std::string log;
    const size_t first = state.log.size() > 60 ? state.log.size() - 60 : 0;
    for (size_t i = state.log.size(); i > first; --i) {
        log += timeOfDay(state.log[i - 1].date) + "  " + state.log[i - 1].text + "\n";
    }
    logText().Text(text(log));

    if (state.revision != drawnRevision_) {
        drawnRevision_ = state.revision;
        drawCharts(state);
    }
}

/// Values that change continuously: render statistics and the leader's clock.
void MainWindow::refreshLive(const GroupSession::Snapshot& state) {
    if (page_ == Page::host) {
        // How far the playing file has got.
        const bool showsProgress = state.playback.isPlaying() && state.selectedTrack && state.selectedTrack->id == state.playback.source;
        progressPanel().Visibility(showsProgress ? Visibility::Visible : Visibility::Collapsed);
        if (showsProgress) {
            const double duration = std::max(state.selectedTrack->duration, 1.0);
            const asyncmusic::RenderStats stats = engine_->session().renderer().latestStats();
            const double elapsed = stats.isPlaying ? std::clamp(stats.mediaSeconds, 0.0, duration) : 0;
            trackProgress().Maximum(duration);
            trackProgress().Value(elapsed);
            elapsedText().Text(text(minutesAndSeconds(elapsed)));
            remainingText().Text(text("-" + minutesAndSeconds(duration - elapsed)));
        }
        return;
    }
    if (page_ != Page::diagnostics) return;
    if (state.clockMap) {
        const int64_t leaderTime = state.clockMap->leaderTimeFromLocal(asyncmusic::hostClockNanoseconds());
        rows_.leaderClock.Text(text(std::format("{:03d}.{:03d} s", leaderTime / 1'000'000'000 % 1000, leaderTime / 1'000'000 % 1000)));
    } else {
        rows_.leaderClock.Text(L"–");
    }
    const asyncmusic::RenderStats stats = engine_->session().renderer().latestStats();
    rows_.position.Text(stats.isPlaying ? text(std::format("{:.3f} s", stats.mediaSeconds)) : hstring(L"–"));
    rows_.scheduleError.Text(stats.isPlaying ? text(std::format("{:+.0f} µs", stats.errorMicroseconds)) : hstring(L"–"));
    rows_.correction.Text(text(std::format("{:+.1f} ppm", stats.correctionPPM)));
    rows_.repositions.Text(text(std::to_string(stats.jumps)));
    rows_.gaps.Text(text(std::to_string(stats.gaps)));
    rows_.level.Text(!stats.isPlaying ? hstring(L"–") : stats.outputPeak > 0 ? text(std::format("{:.1f} dBFS", 20 * std::log10(stats.outputPeak))) : hstring(L"silent"));
    rows_.buffered.Text(stats.isPlaying && stats.bufferedSeconds > 0 ? text(std::format("{:.1f} s", stats.bufferedSeconds)) : hstring(L"–"));
}

void MainWindow::drawCharts(const GroupSession::Snapshot& state) {
    using namespace Microsoft::UI::Xaml::Shapes;
    const auto residual = residualChart();
    const auto roundTrip = roundTripChart();
    residual.Children().Clear();
    roundTrip.Children().Clear();
    if (state.rounds.empty()) return;
    const double width = std::max(residual.ActualWidth(), 100.0);
    const int firstIndex = state.rounds.front().index;
    const int lastIndex = std::max(state.rounds.back().index, firstIndex + 1);
    const auto x = [&](int index) { return (index - firstIndex) * (width - 8) / (lastIndex - firstIndex) + 4; };
    const auto label = [](Canvas const& canvas, const std::string& value, double left, double top) {
        TextBlock block;
        block.Text(text(value));
        block.FontSize(11);
        block.Foreground(themeBrush(L"TextFillColorTertiaryBrush"));
        Canvas::SetLeft(block, left);
        Canvas::SetTop(block, top);
        canvas.Children().Append(block);
    };

    // Residuals around the fitted line, in µs; orange for rounds the estimator rejected.
    double limit = 100;
    for (const auto& round : state.rounds) {
        if (round.residual) limit = std::max(limit, std::abs(static_cast<double>(*round.residual)) / 1e3);
    }
    const double height = residual.Height();
    Line zero;
    zero.X1(0);
    zero.X2(width);
    zero.Y1(height / 2);
    zero.Y2(height / 2);
    zero.Stroke(themeBrush(L"DividerStrokeColorDefaultBrush"));
    residual.Children().Append(zero);
    const Brush accepted = themeBrush(L"AccentFillColorDefaultBrush");
    const Brush rejected = themeBrush(L"SystemFillColorCautionBrush");
    for (const auto& round : state.rounds) {
        if (!round.residual) continue;
        Ellipse point;
        point.Width(4);
        point.Height(4);
        point.Fill(round.verdict == asyncmusic::ClockVerdict::accepted ? accepted : rejected);
        Canvas::SetLeft(point, x(round.index) - 2);
        Canvas::SetTop(point, height / 2 - static_cast<double>(*round.residual) / 1e3 / limit * (height / 2 - 4) - 2);
        residual.Children().Append(point);
    }
    label(residual, std::format("+{:.0f}", limit), 0, 0);
    label(residual, std::format("−{:.0f}", limit), 0, height - 16);

    // Best round trip per round, in ms.
    double maximum = 1;
    for (const auto& round : state.rounds) {
        if (round.best) maximum = std::max(maximum, static_cast<double>(round.best->roundTrip()) / 1e6);
    }
    maximum *= 1.1;
    const double chartHeight = roundTrip.Height();
    Polyline line;
    line.Stroke(accepted);
    line.StrokeThickness(1.5);
    for (const auto& round : state.rounds) {
        if (!round.best) continue;
        line.Points().Append(Windows::Foundation::Point{static_cast<float>(x(round.index)),
            static_cast<float>(chartHeight - static_cast<double>(round.best->roundTrip()) / 1e6 / maximum * chartHeight)});
    }
    roundTrip.Children().Append(line);
    label(roundTrip, std::format("{:.1f}", maximum), 0, 0);
}

std::string MainWindow::report() const {
    const GroupSession::Snapshot state = engine_->session().snapshot();
    const auto output = engine_->output().status();
    const asyncmusic::RenderStats stats = engine_->session().renderer().latestStats();
    std::string report = "AsyncMusic diagnostics (Windows)\n";
    report += std::format("Device: {} ({:016x})\n", state.identity.name, state.identity.id);
    report += std::format("Role: {} · {}\n", roleTitle(state.role), state.status);
    report += std::format("Output: {} · {} · {:.0f} Hz, buffer {:.1f} ms, timestamp jitter {:.0f} µs, extra delay {:+.1f} ms\n", output.deviceName,
        output.isRunning ? "running" : ("stopped " + output.error), output.sampleRate, output.bufferDuration * 1e3, output.timestampJitter, engine_->manualOffset());
    report += std::format("Playing: {}{} · channel {}\n", state.playback.isPlaying() ? engine_->session().describe(state.playback.source) : "nothing",
        state.isWaitingForAudio ? " (waiting for audio)" : "", channelTitle(state.channel));
    report += std::format("Render: playing {}, position {:.3f} s, error {:+.0f} µs, correction {:+.1f} ppm, buffered {:.1f} s, gaps {}, repositions {}, peak {:.4f}, host time {}\n",
        stats.isPlaying ? "yes" : "no", stats.mediaSeconds, stats.errorMicroseconds, stats.correctionPPM, stats.bufferedSeconds, stats.gaps, stats.jumps, stats.outputPeak,
        stats.hostTimeValid ? "valid" : "INVALID");
    report += "\nLog (oldest first):\n";
    for (const auto& line : state.log) report += timeOfDay(line.date) + "  " + line.text + "\n";
    return report;
}

}  // namespace winrt::AsyncMusic::implementation
