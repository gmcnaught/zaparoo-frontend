// Zaparoo Frontend
// Copyright (c) 2026 Wizzo Pty Ltd and the Zaparoo Project contributors.
// SPDX-License-Identifier: LicenseRef-PolyForm-Noncommercial-1.0.0

import QtQuick
import QtQuick.Window
import QtQuick.Controls

// The window the frontend runs in on the normal path.
//
// This exists so the visual tree does not have to be a Window. A
// QQuickWindow must be constructed by whoever owns its render control,
// so a QML-declared Window can never be redirected to a render target
// — which is what the FPGA blitter offload needs in order to hand Qt
// Quick a paint device instead of a framebuffer. Keeping the window in
// a thin shell lets `Main` be an Item that either host can mount: this
// window on desktop and MiSTer's framebuffer path, or the offload's
// own window (see src/app/fpga/blitter_render_loop.cpp).
//
// Nothing but geometry and window chrome belongs here. Routing, input,
// and persistence stay in Main.qml.
ApplicationWindow {
    id: window

    // The visual tree this window hosts, so callers (and the smoke
    // test) can reach the app's state without this shell re-exporting
    // every property Main already has.
    property alias ui: content

    // Forwarded to the content. main.cpp sets these as initial
    // properties on the root object, which is now this window rather
    // than the layout, so they have to be declared here too.
    property bool fullScreen: false
    property bool crtNativePath: false
    property bool crtPreview: false
    property int crtPreviewScale: 0
    property bool debugCrtSafeAreaOverlay: false
    property int videoWidth: 0
    property int videoHeight: 0

    // The content's designer default is a 1280x720 canvas; fullscreen
    // embedded builds need the screen dims applied at construction so
    // the first paint matches the framebuffer layout. Component.onCompleted
    // fires after the first frame, so an imperative override there
    // leaves a wrong-size first frame on screen (visible as a zoomed
    // top-left slice on CRT). For windowed builds the binding only
    // evaluates once at construction (Screen.width is constant per
    // session) so it does not fight user resizes.
    width: window.fullScreen ? Screen.width : 1280
    height: window.fullScreen ? Screen.height : 720
    minimumWidth: content._crtPreviewActive ? window.videoWidth * (window.crtPreviewScale > 0 ? content._clampCrtPreviewScale(window.crtPreviewScale) : content._crtPreviewMinScale) : 426
    minimumHeight: content._crtPreviewActive ? window.videoHeight * (window.crtPreviewScale > 0 ? content._clampCrtPreviewScale(window.crtPreviewScale) : content._crtPreviewMinScale) : 240
    maximumWidth: content._crtPreviewActive ? window.videoWidth * (window.crtPreviewScale > 0 ? content._clampCrtPreviewScale(window.crtPreviewScale) : content._crtPreviewMaxScale) : 16777215
    maximumHeight: content._crtPreviewActive ? window.videoHeight * (window.crtPreviewScale > 0 ? content._clampCrtPreviewScale(window.crtPreviewScale) : content._crtPreviewMaxScale) : 16777215
    visible: true
    visibility: window.fullScreen ? Window.FullScreen : Window.Windowed
    title: qsTr("Zaparoo Frontend")

    Main {
        id: content

        anchors.fill: parent
        fullScreen: window.fullScreen
        crtNativePath: window.crtNativePath
        crtPreview: window.crtPreview
        crtPreviewScale: window.crtPreviewScale
        debugCrtSafeAreaOverlay: window.debugCrtSafeAreaOverlay
        videoWidth: window.videoWidth
        videoHeight: window.videoHeight
    }
}
