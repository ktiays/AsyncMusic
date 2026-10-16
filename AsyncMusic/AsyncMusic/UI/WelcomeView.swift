//
//  Created by ktiays on 2026/10/16.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

import SwiftUI

/// Shown while this device is not part of a group: host, or join someone else's group.
struct WelcomeView: View {
    let session: ClockSyncSession

    var body: some View {
        ScrollView {
            VStack(spacing: 28) {
                VStack(spacing: 12) {
                    Image(systemName: "hifispeaker.2.fill")
                        .font(.system(size: 56))
                        .foregroundStyle(.tint)
                        .padding(.top, 24)
                    Text("AsyncMusic")
                        .font(.largeTitle.bold())
                    Text("Play music in perfect sync across your devices — or split one song into true stereo over two of them.")
                        .font(.body)
                        .foregroundStyle(.secondary)
                        .multilineTextAlignment(.center)
                }

                VStack(spacing: 12) {
                    Button {
                        session.setRole(.leader)
                    } label: {
                        Card {
                            ChoiceRow(
                                systemImage: "play.circle.fill",
                                title: "Host Music",
                                subtitle: "Play from this device. Nearby devices join as speakers.",
                                showsChevron: true
                            )
                        }
                    }
                    Button {
                        session.setRole(.follower)
                    } label: {
                        Card {
                            ChoiceRow(
                                systemImage: "hifispeaker.fill",
                                title: "Join as Speaker",
                                subtitle: "Play along with a host on the same Wi-Fi.",
                                showsChevron: true
                            )
                        }
                    }
                }
                .buttonStyle(.plain)

                Text("This device appears to others as “\(session.deviceName)”.")
                    .font(.footnote)
                    .foregroundStyle(.secondary)
                    .multilineTextAlignment(.center)
            }
            .readableColumn()
        }
    }
}

#Preview {
    NavigationStack {
        WelcomeView(session: ClockSyncSession())
    }
}
