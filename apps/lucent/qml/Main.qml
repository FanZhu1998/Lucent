import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import QtQuick.Window
import Lucent

// The application window: Library, Edit and Exports are three views of one window (blueprint
// section 2). Global keys: Ctrl+O add photos, Ctrl+Shift+O add a folder, Ctrl+Alt+1/2/3 switch
// views, Esc goes back (Edit to Library, Exports to the previous view).
ApplicationWindow {
    id: window

    required property AppController controller

    objectName: "mainWindow"
    width: Math.max(minimumWidth, Math.min(1440, Screen.desktopAvailableWidth - 80))
    height: Math.max(minimumHeight, Math.min(900, Screen.desktopAvailableHeight - 80))
    minimumWidth: 960
    minimumHeight: 600
    visible: true
    title: controller.windowTitle
    color: Theme.window
    font.pixelSize: Theme.fontBody

    // Dark palette for the Fusion style (controls, dialogs drawn by Qt, tooltips).
    palette.window: Theme.window
    palette.windowText: Theme.textPrimary
    palette.base: Theme.surface
    palette.alternateBase: Theme.surfaceRaised
    palette.text: Theme.textPrimary
    palette.button: Theme.control
    palette.buttonText: Theme.textPrimary
    palette.brightText: Theme.textOnAccent
    palette.highlight: Theme.accent
    palette.highlightedText: Theme.textOnAccent
    palette.placeholderText: Theme.textTertiary
    palette.toolTipBase: Theme.surfaceRaised
    palette.toolTipText: Theme.textPrimary
    palette.light: "#4a4a4a"
    palette.midlight: "#404040"
    palette.mid: "#303030"
    palette.dark: "#161616"
    palette.shadow: "#000000"
    palette.link: Theme.focusRing

    function focusCurrentView() {
        const views = [libraryView, editView, exportsView]
        const view = views[controller.currentView]
        if (view)
            view.takeFocus()
    }

    header: AppHeader {
        controller: window.controller
    }

    StackLayout {
        id: stack

        anchors.fill: parent
        currentIndex: window.controller.currentView
        onCurrentIndexChanged: Qt.callLater(window.focusCurrentView)

        LibraryView {
            id: libraryView

            objectName: "libraryView"
            controller: window.controller
            onAddPhotosRequested: photosDialog.open()
            onAddFolderRequested: folderDialog.open()
        }

        EditView {
            id: editView

            objectName: "editView"
            controller: window.controller
            onExportRequested: exportDialog.open()
        }

        ExportsView {
            id: exportsView

            objectName: "exportsView"
            controller: window.controller
        }
    }

    // Drag and drop of photos and folders from Explorer (blueprint section 2). The Import dialog
    // then offers Include subfolders and Copy into library folder.
    DropArea {
        id: dropArea

        objectName: "dropArea"
        anchors.fill: stack
        keys: ["text/uri-list"]
        onEntered: drag => drag.accepted = drag.hasUrls
        onDropped: drop => {
            if (drop.hasUrls && window.controller.requestImport(drop.urls)) {
                drop.acceptProposedAction()
                window.controller.showView(AppController.Library)
            }
        }
    }

    Rectangle {
        anchors.fill: stack
        visible: dropArea.containsDrag
        color: "#cc141414"
        border.width: 2
        border.color: Theme.accent

        Text {
            anchors.centerIn: parent
            text: qsTr("Drop photos or folders to add them")
            color: Theme.textPrimary
            font.pixelSize: Theme.fontHeading
        }
    }

    Component.onCompleted: Qt.callLater(focusCurrentView)

    Shortcut {
        sequences: [StandardKey.Open]
        onActivated: photosDialog.open()
    }

    Shortcut {
        sequence: "Ctrl+Shift+O"
        onActivated: folderDialog.open()
    }

    Shortcut {
        sequence: "Ctrl+Alt+1"
        onActivated: window.controller.showView(AppController.Library)
    }

    Shortcut {
        sequence: "Ctrl+Alt+2"
        enabled: window.controller.library.count > 0
        onActivated: window.controller.showView(AppController.Edit)
    }

    Shortcut {
        sequence: "Ctrl+Alt+3"
        onActivated: window.controller.showView(AppController.Exports)
    }

    Shortcut {
        sequence: "Esc"
        enabled: window.controller.currentView !== AppController.Library
        onActivated: window.controller.goBack()
    }

    FileDialog {
        id: photosDialog

        title: qsTr("Add photos")
        fileMode: FileDialog.OpenFiles
        nameFilters: window.controller.photoNameFilters
        onAccepted: window.controller.requestImport(selectedFiles)
    }

    FolderDialog {
        id: folderDialog

        title: qsTr("Add folder")
        onAccepted: window.controller.requestImport([selectedFolder])
    }

    ExportDialog {
        id: exportDialog

        controller: window.controller
    }

    ImportDialog {
        id: importDialog

        controller: window.controller
    }
}
