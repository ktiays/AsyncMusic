//
//  Created by ktiays on 2026/11/11.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

import Foundation
import UserNotifications

/// The iPhone's share alerts. With the app open, new shares are simply listed (as with the Mac's
/// panel open); while it plays in the background, each new share posts a notification with a
/// Join action.
@MainActor
final class ShareNotifications: NSObject, UNUserNotificationCenterDelegate {
    var isAppActive = true {
        didSet {
            // Back in the app, the shares are listed there.
            if isAppActive, !oldValue { UNUserNotificationCenter.current().removeAllDeliveredNotifications() }
        }
    }

    private weak var center: ShareCenter?
    private var posted: Set<UInt64> = []

    private nonisolated static let category = "share"
    private nonisolated static let joinAction = "join"
    private nonisolated static let shareKey = "share"

    func attach(to center: ShareCenter) {
        self.center = center
        let notifications = UNUserNotificationCenter.current()
        notifications.delegate = self
        let join = UNNotificationAction(identifier: Self.joinAction, title: "Join")
        notifications.setNotificationCategories([UNNotificationCategory(identifier: Self.category, actions: [join], intentIdentifiers: [])])
        center.onAlert = { [weak self] in self?.alertsArrived() }
        observeJoined()
    }

    /// Asks once, when alerts can first matter: the first time this device listens to a share
    /// (then it may play in the background while others start sharing).
    static func requestAuthorization() {
        UNUserNotificationCenter.current().requestAuthorization(options: [.alert, .sound]) { _, _ in }
    }

    private func observeJoined() {
        withObservationTracking {
            _ = center?.joined?.id
        } onChange: { [weak self] in
            Task { @MainActor [weak self] in
                guard let self else { return }
                if self.center?.joined != nil, self.center?.settings.alertBehavior == .showAlert { Self.requestAuthorization() }
                self.observeJoined()
            }
        }
    }

    private func alertsArrived() {
        guard let center else { return }
        guard !isAppActive else {
            center.panelOpened()
            return
        }
        for share in center.alerts where !posted.contains(share.id) {
            posted.insert(share.id)
            let content = UNMutableNotificationContent()
            content.title = "\(share.name) is sharing"
            content.body = share.source.isEmpty ? "Join to play along in sync." : "\(share.source) · Join to play along in sync."
            content.categoryIdentifier = Self.category
            content.userInfo = [Self.shareKey: NSNumber(value: share.id)]
            content.threadIdentifier = "shares"
            let request = UNNotificationRequest(identifier: "share-\(share.id)", content: content, trigger: nil)
            UNUserNotificationCenter.current().add(request)
        }
    }

    // MARK: - UNUserNotificationCenterDelegate

    // No `willPresent`: in the app the share is already listed, so nothing is shown there.

    nonisolated func userNotificationCenter(_ center: UNUserNotificationCenter, didReceive response: UNNotificationResponse) async {
        let action = response.actionIdentifier
        let id = (response.notification.request.content.userInfo[Self.shareKey] as? NSNumber)?.uint64Value
        await MainActor.run {
            guard let id, let shares = self.center else { return }
            if action == Self.joinAction {
                shares.join(id)
            }
        }
    }
}
