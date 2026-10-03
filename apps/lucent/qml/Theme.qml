pragma Singleton

import QtQuick

// Design tokens: a restrained neutral dark-grey interface with generous image space and clear
// typography (blueprint section 2, "Interface character"). Text colours keep at least 4.5:1
// contrast against the surfaces they are used on (tertiary text is for non-essential labels).
QtObject {
    // Surfaces, darkest to lightest.
    readonly property color imageBackground: "#141414"
    readonly property color window: "#1b1b1b"
    readonly property color surface: "#232323"
    readonly property color surfaceRaised: "#2b2b2b"
    readonly property color surfaceHover: "#323232"
    readonly property color control: "#383838"
    readonly property color border: "#3d3d3d"
    readonly property color divider: "#2e2e2e"

    // Text.
    readonly property color textPrimary: "#e8e8e8"
    readonly property color textSecondary: "#ababab"
    readonly property color textTertiary: "#939393"
    readonly property color textOnAccent: "#ffffff"

    // Selection and keyboard focus.
    readonly property color accent: "#4f8fd0"
    readonly property color accentMuted: "#2c4661"
    readonly property color focusRing: "#8cc0f2"

    // Controls. Filled primary buttons use the strong accent so white text keeps 4.5:1; control
    // outlines keep 3:1 against the window (WCAG 1.4.11).
    readonly property color accentStrong: "#2f6fb0"
    readonly property color accentStrongHover: "#3576b8"
    readonly property color accentStrongPressed: "#285f97"
    readonly property color controlHover: "#424242"
    readonly property color controlPressed: "#2e2e2e"
    readonly property color controlBorder: "#666666"
    readonly property color indicatorBorder: "#8a8a8a"

    // Compatibility states (blueprint section 3) and job states.
    readonly property color verified: "#72bf7c"
    readonly property color unverified: "#a3adb8"
    readonly property color limited: "#dcaa52"
    readonly property color unsupported: "#e2736b"
    readonly property color attention: "#dcaa52"

    // Scrim behind labels drawn on top of images.
    readonly property color scrim: "#b3141414"

    // Metrics (logical pixels).
    readonly property int spacing: 8
    readonly property int margin: 16
    readonly property int radius: 4
    readonly property int toolbarHeight: 44
    readonly property int focusRingWidth: 2

    // Typography: the platform font (Segoe UI on Windows) at these pixel sizes.
    readonly property int fontSmall: 11
    readonly property int fontBody: 13
    readonly property int fontTitle: 15
    readonly property int fontHeading: 22

    function supportColor(key) {
        switch (key) {
        case "verified":
            return verified
        case "unverified":
            return unverified
        case "limited":
            return limited
        case "unsupported":
            return unsupported
        }
        return textTertiary
    }

    // Processing states of a photo (presentation::processing_key).
    function processingColor(key) {
        switch (key) {
        case "processed":
            return verified
        case "processing":
        case "queued":
            return accent
        case "failed":
        case "incomplete":
            return unsupported
        }
        return textTertiary
    }

    // Badges of an item's file (presentation::flag_keys).
    function sourceColor(key) {
        switch (key) {
        case "missing":
            return unsupported
        case "changed":
            return attention
        case "duplicate":
            return accent
        case "removable":
            return unverified
        }
        return textTertiary
    }

    function jobStateColor(key) {
        switch (key) {
        case "completed":
            return verified
        case "failed":
            return unsupported
        case "running":
            return accent
        }
        return textTertiary
    }
}
