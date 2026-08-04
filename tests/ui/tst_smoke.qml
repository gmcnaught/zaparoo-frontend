// Zaparoo Frontend
// Copyright (c) 2026 Wizzo Pty Ltd and the Zaparoo Project contributors.
// SPDX-License-Identifier: LicenseRef-PolyForm-Noncommercial-1.0.0
// cxx-qt 0.8 patches `isFinal: true` on singleton properties but the
// qmltypes schema has no `isFinal` slot for plain reads either, so any
// `Browse.<Singleton>.<prop>` access trips the "Member can be shadowed"
// check. Suppress at file level the same way every other QML file in
// the tree does.
// qmllint disable compiler

import QtQuick
import QtTest
import Zaparoo.App
import Zaparoo.Browse as Browse

TestCase {
    function test_window_loads() {
        verify(mainWindow.visible, "Main window should be visible");
        compare(mainWindow.title, "Zaparoo Frontend");
    }

    function test_initial_state() {
        compare(mainWindow.ui.activeScreen, "hub");
    }

    function test_system_status_properties_exist() {
        compare(typeof Browse.SystemStatus.has_nfc, "boolean");
        compare(typeof Browse.SystemStatus.has_wifi_internet, "boolean");
        compare(typeof Browse.SystemStatus.has_lan_internet, "boolean");
        compare(typeof Browse.SystemStatus.has_bluetooth, "boolean");
    }

    function test_restart_prompt_covers_window() {
        mainWindow.ui.openSettingNeedsRestartModal();
        tryCompare(mainWindow.ui, "settingNeedsRestartModalVisible", true);
        verify(mainWindow.ui.settingNeedsRestartModal !== null);
        compare(mainWindow.ui.settingNeedsRestartModal.width, mainWindow.ui.width);
        compare(mainWindow.ui.settingNeedsRestartModal.height, mainWindow.ui.height);
        mainWindow.ui.cancelPendingRestart();
    }

    name: "UiWindow"
    when: windowShown

    // AppWindow is the application's real root: a thin window shell
    // around the Item-rooted visual tree (`ui`), which is the shape
    // that lets the FPGA offload host the same tree without a
    // QML-declared Window.
    AppWindow {
        id: mainWindow

        fullScreen: false
        width: 1280
        height: 720
    }
}
