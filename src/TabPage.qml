import QtQuick
import QtQuick.Controls.Basic
import Tether

// A tab's panes, as a tree of SplitViews with terminals at the leaves, like
// tmux windows. Panes are created with this page as their QObject parent, so
// moving them between SplitViews never lets them be garbage collected.
Item {
    id: page

    required property string launchJson
    property var root: null
    property var activePane: null
    property var zoomed: null
    property int paneCount: 0
    readonly property string title: activePane
        ? activePane.title + (paneCount > 1 ? "  +" + (paneCount - 1) : "") + (zoomed ? "  ⤢" : "")
        : ""

    signal emptied()
    signal failed(string message)

    readonly property int handleSize: 5

    Component {
        id: paneComponent
        TerminalPane {
            onReconnectRequested: page.reconnect(this)
        }
    }

    Component {
        id: filesComponent
        FilesPane {}
    }

    // Splits of a kept-alive host each get their own tmux session.
    property int splitCounter: 1

    Component {
        id: splitComponent
        SplitView {
            property var container: null
            readonly property bool isPane: false
            // A hairline that turns blue while it's dragged.
            handle: Rectangle {
                implicitWidth: page.handleSize
                implicitHeight: page.handleSize
                color: Style.window
                Rectangle {
                    anchors.centerIn: parent
                    width: parent.width > parent.height ? parent.width : (SplitHandle.pressed || SplitHandle.hovered ? 3 : 1)
                    height: parent.width > parent.height ? (SplitHandle.pressed || SplitHandle.hovered ? 3 : 1) : parent.height
                    color: SplitHandle.pressed ? Style.accent
                         : SplitHandle.hovered ? Style.tint(0.6) : Qt.rgba(1, 1, 1, 0.08)
                    Behavior on color { ColorAnimation { duration: Style.fast } }
                }
            }
        }
    }

    Component.onCompleted: {
        const pane = makePane(JSON.parse(launchJson))
        setRoot(pane)
        activePane = pane
    }

    function makePane(launch) {
        paneCount++
        const component = launch.files ? filesComponent : paneComponent
        return component.createObject(page, { launch: launch, page: page })
    }

    function setRoot(item) {
        root = item
        item.container = null
        item.parent = page
        item.anchors.fill = page
    }

    function indexIn(holder, item) {
        for (let i = 0; i < holder.count; ++i)
            if (holder.itemAt(i) === item)
                return i
        return -1
    }

    // Puts item where old was, in its SplitView or as the root.
    function replace(old, item) {
        const holder = old.container
        if (holder) {
            // Attached SplitView sizes only exist on direct children, so read
            // them before taking old out and set them after item goes in.
            const width = old.SplitView.preferredWidth, height = old.SplitView.preferredHeight
            const i = indexIn(holder, old)
            holder.takeItem(i)
            holder.insertItem(i, item)
            item.SplitView.preferredWidth = width
            item.SplitView.preferredHeight = height
            item.container = holder
        } else {
            old.anchors.fill = undefined
            setRoot(item)
        }
    }

    function equalize(holder, total) {
        const horizontal = holder.orientation === Qt.Horizontal
        const each = (total - (holder.count - 1) * handleSize) / holder.count
        for (let i = 0; i < holder.count; ++i) {
            const item = holder.itemAt(i)
            if (horizontal)
                item.SplitView.preferredWidth = each
            else
                item.SplitView.preferredHeight = each
        }
    }

    function panes(item) {
        if (item === undefined)
            item = root
        if (!item)
            return []
        if (item.isPane)
            return [item]
        let found = []
        for (let i = 0; i < item.count; ++i)
            found = found.concat(panes(item.itemAt(i)))
        return found
    }

    // What a new pane next to this one runs: the same host again, or a local
    // shell beside a serial port.
    function launchBeside(pane) {
        let profile = pane && pane.launch.splittable ? pane.launch.profile : ({ kind: "local" })
        if (profile.persistent) {
            profile = Object.assign({}, profile)
            profile.tmuxSession = (profile.name || profile.host) + " " + (++splitCounter)
        }
        return backend.launch(profile)
    }

    // The host's files beside the focused pane, for ssh hosts.
    function filesLaunch() {
        const profile = activePane && activePane.launch.profile
        if (!profile || profile.kind !== "ssh")
            return { error: qsTr("Files are for ssh hosts. Focus a pane connected to one first.") }
        return { files: true, title: profile.name, profile: profile, splittable: true }
    }

    function split(orientation, files) {
        const pane = activePane
        if (!pane)
            return
        const launch = files ? filesLaunch() : launchBeside(pane)
        if (launch.error) {
            failed(launch.error)
            return
        }
        unzoom()
        const horizontal = orientation === Qt.Horizontal
        const fresh = makePane(launch)
        const holder = pane.container
        if (holder && holder.orientation === orientation) {
            holder.insertItem(indexIn(holder, pane) + 1, fresh)
            fresh.container = holder
            equalize(holder, horizontal ? holder.width : holder.height)
        } else {
            const total = horizontal ? pane.width : pane.height
            const view = splitComponent.createObject(page, { orientation: orientation })
            replace(pane, view)
            view.addItem(pane)
            pane.container = view
            view.addItem(fresh)
            fresh.container = view
            equalize(view, total)
        }
        Qt.callLater(fresh.focusTerminal)
    }

    function closePane(pane) {
        if (!pane)
            return
        unzoom()
        const holder = pane.container
        paneCount--
        if (!holder) {
            root = null
            activePane = null
            pane.destroy()
            emptied()
            return
        }
        const total = holder.orientation === Qt.Horizontal ? holder.width : holder.height
        holder.takeItem(indexIn(holder, pane))
        pane.destroy()
        if (holder.count === 1) {
            // A split with one pane left is just that pane.
            const survivor = holder.takeItem(0)
            replace(holder, survivor)
            holder.destroy()
        } else {
            equalize(holder, total)
        }
        const remaining = panes()
        activePane = remaining.length ? remaining[0] : null
        if (activePane)
            Qt.callLater(activePane.focusTerminal)
    }

    function closeAll() {
        for (const pane of panes())
            pane.destroy()
    }

    function reconnect(pane) {
        const launch = backend.launch(pane.launch.profile)
        if (launch.error)
            failed(launch.error)
        else
            pane.restart(launch)
    }

    // The nearest pane in a direction, judged by where panes are on screen
    // rather than by the tree, the way tmux's select-pane does.
    function moveFocus(dx, dy) {
        const from = activePane
        if (!from)
            return
        const a = from.mapToItem(page, 0, 0, from.width, from.height)
        let best = null, bestScore = Infinity
        for (const pane of panes()) {
            if (pane === from || !pane.visible)
                continue
            const r = pane.mapToItem(page, 0, 0, pane.width, pane.height)
            let gap, overlap
            if (dx !== 0) {
                gap = dx > 0 ? r.x - (a.x + a.width) : a.x - (r.x + r.width)
                overlap = Math.min(a.y + a.height, r.y + r.height) - Math.max(a.y, r.y)
            } else {
                gap = dy > 0 ? r.y - (a.y + a.height) : a.y - (r.y + r.height)
                overlap = Math.min(a.x + a.width, r.x + r.width) - Math.max(a.x, r.x)
            }
            if (gap < -handleSize - 2 || overlap <= 0)
                continue
            const score = gap * 10 - overlap
            if (score < bestScore) {
                bestScore = score
                best = pane
            }
        }
        if (best)
            best.focusTerminal()
    }

    // Moves the nearest divider in the direction of the arrow.
    function resize(dx, dy) {
        const horizontal = dx !== 0
        const step = (horizontal ? dx : dy) * 40
        let node = activePane
        while (node && node.container) {
            const holder = node.container
            if ((holder.orientation === Qt.Horizontal) === horizontal) {
                const i = indexIn(holder, node)
                // The last item fills what's left, so size the one before it.
                const target = i < holder.count - 1 ? holder.itemAt(i) : holder.itemAt(i - 1)
                if (horizontal)
                    target.SplitView.preferredWidth = Math.max(80, target.width + step)
                else
                    target.SplitView.preferredHeight = Math.max(60, target.height + step)
                return
            }
            node = holder
        }
    }

    function toggleZoom() {
        if (zoomed) {
            unzoom()
            return
        }
        if (!activePane || !activePane.container)
            return
        zoomed = activePane
        let node = activePane
        while (node.container) {
            const holder = node.container
            for (let i = 0; i < holder.count; ++i)
                if (holder.itemAt(i) !== node)
                    holder.itemAt(i).visible = false
            node = holder
        }
        Qt.callLater(activePane.focusTerminal)
    }

    function unzoom() {
        if (!zoomed)
            return
        zoomed = null
        showAll(root)
    }

    function showAll(item) {
        if (!item)
            return
        item.visible = true
        if (!item.isPane)
            for (let i = 0; i < item.count; ++i)
                showAll(item.itemAt(i))
    }

    function broadcast(text) {
        for (const pane of panes())
            pane.send(text)
    }
}
