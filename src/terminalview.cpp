#include "terminalview.h"

#include "pseudoterminal.h"

#include <QClipboard>
#include <QCursor>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QHash>
#include <QInputMethod>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QPainter>
#include <QStyleHints>

#include <cmath>

#ifdef TETHER_QTSERIAL
#include <QSerialPort>
#endif
#ifdef TETHER_LIBSSH
#include "sshsession.h"
#endif

// libvterm calls back through plain functions; these hand each call to the view.
struct TerminalCallbacks {
    static int damage(VTermRect rect, void *user) { return static_cast<TerminalView *>(user)->onDamage(rect); }
    static int moverect(VTermRect, VTermRect, void *user) {
        static_cast<TerminalView *>(user)->update();
        return 1;
    }
    static int movecursor(VTermPos pos, VTermPos, int visible, void *user) {
        return static_cast<TerminalView *>(user)->onMoveCursor(pos, visible);
    }
    static int settermprop(VTermProp prop, VTermValue *value, void *user) {
        return static_cast<TerminalView *>(user)->onTermProp(prop, value);
    }
    static int bell(void *user) {
        emit static_cast<TerminalView *>(user)->bell();
        return 1;
    }
    static int resize(int, int, void *) { return 1; }
    static int pushline(int cols, const VTermScreenCell *cells, void *user) {
        return static_cast<TerminalView *>(user)->onPushLine(cols, cells);
    }
    static int popline(int cols, VTermScreenCell *cells, void *user) {
        return static_cast<TerminalView *>(user)->onPopLine(cols, cells);
    }
    static int clear(void *user) { return static_cast<TerminalView *>(user)->onClearScrollback(); }
    static void output(const char *bytes, size_t length, void *user) {
        static_cast<TerminalView *>(user)->write(QByteArray(bytes, int(length)));
    }
};

static const VTermScreenCallbacks screenCallbacks = {
    TerminalCallbacks::damage,     TerminalCallbacks::moverect, TerminalCallbacks::movecursor,
    TerminalCallbacks::settermprop, TerminalCallbacks::bell,    TerminalCallbacks::resize,
    TerminalCallbacks::pushline,   TerminalCallbacks::popline,  TerminalCallbacks::clear,
};

// The xterm colors, for when no theme sets them.
static const char *const defaultPalette[16] = {
    "#000000", "#cd3131", "#0dbc79", "#e5e510", "#2472c8", "#bc3fbc", "#11a8cd", "#e5e5e5",
    "#666666", "#f14c4c", "#23d18b", "#f5f543", "#3b8eea", "#d670d6", "#29b8db", "#ffffff"};

TerminalView::TerminalView(QQuickItem *parent) : QQuickPaintedItem(parent) {
    setFlag(ItemAcceptsInputMethod, true);
    setFlag(ItemIsFocusScope, false);
    setAcceptedMouseButtons(Qt::AllButtons);
    setAcceptHoverEvents(true);
    setActiveFocusOnTab(true);
    setCursor(Qt::IBeamCursor);
    setOpaquePainting(true);
    setFillColor(m_background);

    for (const char *c : defaultPalette)
        m_palette << QColor(c);

    m_font = QFont("monospace");
    m_font.setStyleHint(QFont::Monospace);
    m_font.setPixelSize(13);

    m_vt = vterm_new(m_rows, m_cols);
    vterm_set_utf8(m_vt, 1);
    vterm_output_set_callback(m_vt, TerminalCallbacks::output, this);
    m_screen = vterm_obtain_screen(m_vt);
    vterm_screen_set_callbacks(m_screen, &screenCallbacks, this);
    vterm_screen_set_damage_merge(m_screen, VTERM_DAMAGE_SCROLL);
    vterm_screen_enable_altscreen(m_screen, 1);
    vterm_screen_enable_reflow(m_screen, true);
    vterm_screen_reset(m_screen, 1);
    applyColors();
    updateMetrics();

    m_blinkTimer.setInterval(530);
    connect(&m_blinkTimer, &QTimer::timeout, this, [this] {
        m_blinkOn = !m_blinkOn;
        update();
    });
}

TerminalView::~TerminalView() {
#ifdef TETHER_LIBSSH
    // The connection ends on its own thread, in its own time: closing a tab
    // never waits for the network.
    if (m_ssh)
        m_ssh->retire();
#endif
    if (m_pty) {
        m_pty->disconnect(this);
        m_pty->terminate();
    }
    delete m_log;
    vterm_free(m_vt);
}

// ── Settings ───────────────────────────────────────────────────────────────

void TerminalView::setFont(const QFont &font) {
    if (font == m_font)
        return;
    m_font = font;
    m_font.setStyleHint(QFont::Monospace);
    m_font.setKerning(false);
    updateMetrics();
    emit fontChanged();
}

void TerminalView::setLineSpacing(int spacing) {
    spacing = qBound(0, spacing, 20);
    if (spacing == m_lineSpacing)
        return;
    m_lineSpacing = spacing;
    updateMetrics();
    emit fontChanged();
}

void TerminalView::setBlinkingCursor(bool blink) {
    if (blink == m_blinkSetting)
        return;
    m_blinkSetting = blink;
    m_blinkOn = true;
    if (hasActiveFocus() && (m_blinkSetting || m_cursorBlinkRequested))
        m_blinkTimer.start();
    else
        m_blinkTimer.stop();
    update();
    emit optionsChanged();
}

void TerminalView::setBoldText(bool bold) {
    if (bold == m_boldText)
        return;
    m_boldText = bold;
    update();
    emit optionsChanged();
}

void TerminalView::setLogFile(const QString &path) {
    if (path == m_logPath)
        return;
    m_logPath = path;
    delete m_log;
    m_log = nullptr;
    if (!path.isEmpty()) {
        QDir().mkpath(QFileInfo(path).absolutePath());
        m_log = new QFile(path);
        if (!m_log->open(QIODevice::Append)) {
            delete m_log;
            m_log = nullptr;
        } else {
            m_log->setPermissions(QFile::ReadOwner | QFile::WriteOwner);
        }
    }
    emit optionsChanged();
}

void TerminalView::setColors(const QVariantMap &colors) {
    m_colorMap = colors;
    const QColor bg(colors.value("background").toString()), fg(colors.value("foreground").toString());
    if (bg.isValid())
        m_background = bg;
    if (fg.isValid())
        m_foreground = fg;
    m_cursorColor = QColor(colors.value("cursor").toString());
    const QVariantList palette = colors.value("palette").toList();
    for (int i = 0; i < 16; ++i) {
        const QColor c(palette.value(i).toString());
        m_palette[i] = c.isValid() ? c : QColor(defaultPalette[i]);
    }
    setFillColor(m_background);
    applyColors();
    update();
    emit colorsChanged();
}

void TerminalView::applyColors() {
    VTermState *state = vterm_obtain_state(m_vt);
    for (int i = 0; i < 16; ++i) {
        VTermColor c;
        vterm_color_rgb(&c, uint8_t(m_palette[i].red()), uint8_t(m_palette[i].green()), uint8_t(m_palette[i].blue()));
        vterm_state_set_palette_color(state, i, &c);
    }
    VTermColor fg, bg;
    vterm_color_rgb(&fg, uint8_t(m_foreground.red()), uint8_t(m_foreground.green()), uint8_t(m_foreground.blue()));
    vterm_color_rgb(&bg, uint8_t(m_background.red()), uint8_t(m_background.green()), uint8_t(m_background.blue()));
    vterm_screen_set_default_colors(m_screen, &fg, &bg);
}

void TerminalView::updateMetrics() {
    m_bold = m_font;
    m_bold.setBold(true);
    m_italic = m_font;
    m_italic.setItalic(true);
    m_boldItalic = m_bold;
    m_boldItalic.setItalic(true);
    const QFontMetricsF metrics(m_font);
    m_cellWidth = qMax<qreal>(1, metrics.horizontalAdvance(QLatin1Char('M')));
    m_cellHeight = std::ceil(metrics.height()) + m_lineSpacing;
    m_ascent = metrics.ascent() + m_lineSpacing / 2.0;
    resizeTerminal();
    update();
}

void TerminalView::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) {
    QQuickPaintedItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size())
        resizeTerminal();
}

void TerminalView::resizeTerminal() {
    if (width() <= 0 || height() <= 0)
        return;
    const int cols = qMax(2, int(width() / m_cellWidth));
    const int rows = qMax(1, int(height() / m_cellHeight));
    if (cols == m_cols && rows == m_rows)
        return;
    m_cols = cols;
    m_rows = rows;
    vterm_set_size(m_vt, rows, cols);
    vterm_screen_flush_damage(m_screen);
    if (m_pty)
        m_pty->resize(cols, rows);
#ifdef TETHER_LIBSSH
    if (m_ssh)
        m_ssh->resize(cols, rows);
#endif
    m_scrollOffset = qMin(m_scrollOffset, int(m_scrollback.size()));
    emit sizeChanged();
    emit scrollChanged();
    update();
}

// ── Running a program ──────────────────────────────────────────────────────

bool TerminalView::startSsh(const QVariantMap &settings) {
#ifdef TETHER_LIBSSH
    if (m_ssh)
        m_ssh->retire();
    QVariantMap s = settings;
    s.insert("columns", m_cols);
    s.insert("rows", m_rows);
    m_ssh = new SshSession(s, this);
    connect(m_ssh, &SshSession::dataReceived, this, [this](const QByteArray &data) {
        if (m_log) {
            m_log->write(data);
            m_log->flush();
        }
        feed(data);
    }, Qt::QueuedConnection);
    connect(m_ssh, &SshSession::hostKeyQuestion, this, &TerminalView::hostKeyQuestion, Qt::QueuedConnection);
    connect(m_ssh, &SshSession::secretInput, this, &TerminalView::setSecretInput, Qt::QueuedConnection);
    connect(m_ssh, &SshSession::finished, this, [this](int code) {
        m_sshRunning = false;
        emit runningChanged();
        emit finished(code);
    }, Qt::QueuedConnection);
    m_sshRunning = true;
    m_ssh->start();
    emit runningChanged();
    return true;
#else
    Q_UNUSED(settings)
    feed(("\r\n" + tr("This build can't make ssh connections itself.") + "\r\n").toUtf8());
    emit finished(255);
    return false;
#endif
}

void TerminalView::setSecretInput(bool on) {
    if (on == m_secretInput)
        return;
    m_secretInput = on;
    m_composing.clear();
    emit secretInputChanged();
    // Tell the keyboard its kind of field changed, so it switches modes. An
    // on-screen keyboard only takes the new mode when it's shown again, and
    // would otherwise close; a password prompt also brings it up, since the
    // next thing to do is type.
    QInputMethod *keyboard = QGuiApplication::inputMethod();
    if (!keyboard || !hasActiveFocus())
        return;
    const bool shown = keyboard->isVisible();
    keyboard->reset();
    keyboard->update(Qt::ImHints | Qt::ImEnabled);
    if (shown || (on && touchKeyboard()))
        keyboard->show();
}

bool TerminalView::touchKeyboard() {
#if defined(Q_OS_ANDROID) || defined(Q_OS_IOS)
    return true;
#else
    return false;
#endif
}

void TerminalView::answerHostKey(bool trust) {
#ifdef TETHER_LIBSSH
    if (m_ssh)
        m_ssh->answerHostKey(trust);
#else
    Q_UNUSED(trust)
#endif
}

bool TerminalView::running() const {
#ifdef TETHER_LIBSSH
    if (m_ssh)
        return m_sshRunning;
#endif
#ifdef TETHER_QTSERIAL
    if (m_serial)
        return m_serial->isOpen();
#endif
    return m_pty && m_pty->running();
}

bool TerminalView::startSerial(const QVariantMap &settings) {
#ifdef TETHER_QTSERIAL
    if (m_serial) {
        m_serial->disconnect(this);
        m_serial->close();
        m_serial->deleteLater();
    }
    m_serial = new QSerialPort(this);
    m_serial->setPortName(settings.value("device").toString());
    m_serial->setBaudRate(settings.value("baud", 115200).toInt());
    m_serial->setDataBits(QSerialPort::DataBits(settings.value("dataBits", 8).toInt()));
    const QString parity = settings.value("parity").toString();
    m_serial->setParity(parity == "even" ? QSerialPort::EvenParity : parity == "odd" ? QSerialPort::OddParity
                                                                                      : QSerialPort::NoParity);
    m_serial->setStopBits(settings.value("stopBits", 1).toInt() == 2 ? QSerialPort::TwoStop : QSerialPort::OneStop);
    const QString flow = settings.value("flow").toString();
    m_serial->setFlowControl(flow == "hardware" ? QSerialPort::HardwareControl
                             : flow == "software" ? QSerialPort::SoftwareControl : QSerialPort::NoFlowControl);
    m_serialEnter = settings.value("enter", "cr").toString();
    m_serialEcho = settings.value("echo").toBool();

    connect(m_serial, &QSerialPort::readyRead, this, [this] {
        const QByteArray data = m_serial->readAll();
        if (m_log) {
            m_log->write(data);
            m_log->flush();
        }
        feed(data);
    });
    connect(m_serial, &QSerialPort::errorOccurred, this, [this](QSerialPort::SerialPortError error) {
        // Unplugging the adapter ends the session.
        if (error == QSerialPort::ResourceError && m_serial->isOpen()) {
            m_serial->close();
            feed(("\r\n" + tr("The serial port went away.") + "\r\n").toUtf8());
            emit runningChanged();
            emit finished(1);
        }
    });
    if (!m_serial->open(QIODevice::ReadWrite)) {
        feed(("\r\n" + tr("Couldn't open %1: %2").arg(m_serial->portName(), m_serial->errorString()) + "\r\n").toUtf8());
        emit runningChanged();
        emit finished(1);
        return false;
    }
    feed(("\x1b[2m" + tr("Connected to %1 at %2 baud.").arg(m_serial->portName()).arg(m_serial->baudRate())
          + "\x1b[0m\r\n").toUtf8());
    emit runningChanged();
    return true;
#else
    Q_UNUSED(settings)
    feed(("\r\n" + tr("This build can't open serial ports itself.") + "\r\n").toUtf8());
    emit finished(1);
    return false;
#endif
}

bool TerminalView::start(const QString &program, const QStringList &args, const QString &workingDirectory,
                         const QStringList &environment) {
    if (m_pty) {
        m_pty->disconnect(this);
        m_pty->terminate();
        m_pty->deleteLater();
    }
    m_pty = new Pty(this);
    connect(m_pty, &Pty::dataReceived, this, [this](const QByteArray &data) {
        if (m_log) {
            m_log->write(data);
            m_log->flush();
        }
        feed(data);
    });
    connect(m_pty, &Pty::finished, this, [this](int code) {
        emit runningChanged();
        emit finished(code);
    });
    if (!m_pty->start(program, args, workingDirectory, environment, m_cols, m_rows)) {
        feed(("\r\n" + m_pty->error() + "\r\n").toUtf8());
        emit runningChanged();
        emit finished(127);
        return false;
    }
    emit runningChanged();
    return true;
}

void TerminalView::terminate() {
#ifdef TETHER_LIBSSH
    if (m_ssh)
        m_ssh->stop();
#endif
#ifdef TETHER_QTSERIAL
    if (m_serial && m_serial->isOpen()) {
        m_serial->close();
        emit runningChanged();
        emit finished(0);
    }
#endif
    if (m_pty)
        m_pty->terminate();
}

void TerminalView::write(const QByteArray &data) {
#ifdef TETHER_LIBSSH
    if (m_ssh) {
        if (m_sshRunning)
            m_ssh->write(data);
        return;
    }
#endif
#ifdef TETHER_QTSERIAL
    if (m_serial) {
        if (!m_serial->isOpen())
            return;
        QByteArray out = data;
        // Enter arrives as CR; the device may want LF or CR LF instead.
        if (m_serialEnter == "lf")
            out.replace('\r', '\n');
        else if (m_serialEnter == "crlf")
            out.replace("\r", "\r\n");
        m_serial->write(out);
        if (m_serialEcho)
            feed(QByteArray(data).replace("\r", "\r\n"));
        return;
    }
#endif
    if (m_pty)
        m_pty->write(data);
}

void TerminalView::feed(const QByteArray &data) {
    vterm_input_write(m_vt, data.constData(), size_t(data.size()));
    vterm_screen_flush_damage(m_screen);
}

VTermModifier TerminalView::takeSticky() {
    int mod = VTERM_MOD_NONE;
    if (m_stickyCtrl)
        mod |= VTERM_MOD_CTRL;
    if (m_stickyAlt)
        mod |= VTERM_MOD_ALT;
    setStickyCtrl(false);
    setStickyAlt(false);
    return VTermModifier(mod);
}

void TerminalView::sendKey(const QString &name) {
    static const QHash<QString, VTermKey> keys{
        {"esc", VTERM_KEY_ESCAPE}, {"tab", VTERM_KEY_TAB}, {"up", VTERM_KEY_UP}, {"down", VTERM_KEY_DOWN},
        {"left", VTERM_KEY_LEFT}, {"right", VTERM_KEY_RIGHT}, {"home", VTERM_KEY_HOME}, {"end", VTERM_KEY_END},
        {"pgup", VTERM_KEY_PAGEUP}, {"pgdn", VTERM_KEY_PAGEDOWN}, {"enter", VTERM_KEY_ENTER},
        {"backspace", VTERM_KEY_BACKSPACE}};
    if (!keys.contains(name))
        return;
    scrollToBottom();
    vterm_keyboard_key(m_vt, keys.value(name), takeSticky());
}

void TerminalView::sendText(const QString &text) {
    scrollToBottom();
    write(text.toUtf8());
}

void TerminalView::copyClipboard() {
    const QString text = selectedText();
    if (!text.isEmpty())
        QGuiApplication::clipboard()->setText(text);
}

void TerminalView::pasteClipboard() {
    QString text = QGuiApplication::clipboard()->text();
    if (text.isEmpty())
        return;
    text.replace("\r\n", "\r").replace('\n', '\r');
    scrollToBottom();
    // Bracketed paste, when the program asks for it, so a pasted command
    // isn't run before it's looked at.
    vterm_keyboard_start_paste(m_vt);
    write(text.toUtf8());
    vterm_keyboard_end_paste(m_vt);
}

void TerminalView::clearScrollback() {
    m_scrollback.clear();
    m_scrollOffset = 0;
    clearSelection();
    emit scrollChanged();
    update();
}

// ── libvterm callbacks ─────────────────────────────────────────────────────

int TerminalView::onDamage(VTermRect) {
    update();
    return 1;
}

int TerminalView::onMoveCursor(VTermPos pos, int visible) {
    m_cursor = pos;
    m_cursorVisible = visible;
    m_blinkOn = true;
    update();
    return 1;
}

int TerminalView::onTermProp(VTermProp prop, VTermValue *value) {
    switch (prop) {
    case VTERM_PROP_CURSORVISIBLE:
        m_cursorVisible = value->boolean;
        break;
    case VTERM_PROP_CURSORBLINK:
        m_cursorBlinkRequested = value->boolean;
        if (hasActiveFocus() && (m_blinkSetting || m_cursorBlinkRequested))
            m_blinkTimer.start();
        break;
    case VTERM_PROP_CURSORSHAPE:
        m_cursorShape = value->number;
        break;
    case VTERM_PROP_ALTSCREEN:
        m_altScreen = value->boolean;
        if (m_altScreen)
            scrollToBottom();
        break;
    case VTERM_PROP_MOUSE:
        m_mouseMode = value->number;
        setCursor(m_mouseMode != VTERM_PROP_MOUSE_NONE ? Qt::ArrowCursor : Qt::IBeamCursor);
        break;
    case VTERM_PROP_REVERSE:
        m_reverse = value->boolean;
        break;
    case VTERM_PROP_TITLE:
        if (value->string.initial)
            m_titleBuffer.clear();
        m_titleBuffer.append(value->string.str, int(value->string.len));
        if (value->string.final) {
            m_title = QString::fromUtf8(m_titleBuffer);
            emit titleChanged();
        }
        break;
    default:
        break;
    }
    update();
    return 1;
}

int TerminalView::onPushLine(int cols, const VTermScreenCell *cells) {
    // Trailing blanks aren't worth keeping.
    int used = cols;
    while (used > 0 && cells[used - 1].chars[0] == 0 && VTERM_COLOR_IS_DEFAULT_BG(&cells[used - 1].bg))
        --used;
    m_scrollback.emplace_back(cells, cells + used);
    if (int(m_scrollback.size()) > MaxScrollback) {
        m_scrollback.pop_front();
        // The selection is in absolute lines, which just moved up by one.
        m_selectionStart.ry()--;
        m_selectionEnd.ry()--;
        m_selectionAnchor.ry()--;
    } else if (m_scrollOffset > 0) {
        // Keep the lines being read in place while output arrives.
        ++m_scrollOffset;
    }
    emit scrollChanged();
    return 1;
}

int TerminalView::onPopLine(int cols, VTermScreenCell *cells) {
    if (m_scrollback.empty())
        return 0;
    const QVector<VTermScreenCell> &line = m_scrollback.back();
    VTermScreenCell blank{};
    blank.width = 1;
    vterm_color_indexed(&blank.fg, 0);
    blank.fg.type = VTERM_COLOR_DEFAULT_FG;
    blank.bg.type = VTERM_COLOR_DEFAULT_BG;
    for (int i = 0; i < cols; ++i)
        cells[i] = i < line.size() ? line[i] : blank;
    m_scrollback.pop_back();
    m_scrollOffset = qMin(m_scrollOffset, int(m_scrollback.size()));
    emit scrollChanged();
    return 1;
}

int TerminalView::onClearScrollback() {
    m_scrollback.clear();
    m_scrollOffset = 0;
    emit scrollChanged();
    return 1;
}

// ── Reading cells ──────────────────────────────────────────────────────────

bool TerminalView::cellAt(int absoluteLine, int column, VTermScreenCell *cell) const {
    const int history = int(m_scrollback.size());
    if (absoluteLine < 0 || column < 0)
        return false;
    if (absoluteLine < history) {
        const QVector<VTermScreenCell> &line = m_scrollback[size_t(absoluteLine)];
        if (column >= line.size())
            return false;
        *cell = line[column];
        return true;
    }
    const int row = absoluteLine - history;
    if (row >= m_rows || column >= m_cols)
        return false;
    return vterm_screen_get_cell(m_screen, VTermPos{row, column}, cell);
}

QString TerminalView::lineText(int absoluteLine) const {
    QString text;
    VTermScreenCell cell;
    for (int col = 0; col < m_cols && cellAt(absoluteLine, col, &cell); ++col) {
        if (cell.chars[0] == 0) {
            text += ' ';
            continue;
        }
        for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL && cell.chars[i]; ++i)
            text += QString::fromUcs4(reinterpret_cast<const char32_t *>(&cell.chars[i]), 1);
        if (cell.width > 1)
            col += cell.width - 1;
    }
    while (text.endsWith(' '))
        text.chop(1);
    return text;
}

QString TerminalView::screenText(int lines) const {
    // Up to the last line with text on it, or the cursor, whichever is lower:
    // short output sits at the top of an otherwise blank screen.
    const int history = int(m_scrollback.size());
    int last = history + m_cursor.row + 1;
    for (int line = history + m_rows - 1; line >= last; --line)
        if (!lineText(line).isEmpty()) {
            last = line + 1;
            break;
        }
    QStringList out;
    for (int line = qMax(0, last - lines); line < last; ++line)
        out << lineText(line);
    while (!out.isEmpty() && out.last().isEmpty())
        out.removeLast();
    return out.join('\n');
}

QColor TerminalView::color(const VTermColor &c, bool foreground, bool bold) const {
    if (foreground && VTERM_COLOR_IS_DEFAULT_FG(&c))
        return m_foreground;
    if (!foreground && VTERM_COLOR_IS_DEFAULT_BG(&c))
        return m_background;
    if (VTERM_COLOR_IS_INDEXED(&c)) {
        int index = c.indexed.idx;
        // Bold text in one of the 8 basic colors shows its bright twin.
        if (bold && foreground && index < 8)
            index += 8;
        if (index < 16)
            return m_palette[index];
        VTermColor rgb = c;
        vterm_screen_convert_color_to_rgb(m_screen, &rgb);
        return QColor(rgb.rgb.red, rgb.rgb.green, rgb.rgb.blue);
    }
    return QColor(c.rgb.red, c.rgb.green, c.rgb.blue);
}

// ── Drawing ────────────────────────────────────────────────────────────────

void TerminalView::paint(QPainter *painter) {
    painter->fillRect(QRectF(0, 0, width(), height()), m_background);
    const int first = firstVisibleLine();
    const QColor selectionColor = QColor(m_foreground.red(), m_foreground.green(), m_foreground.blue(), 70);

    VTermScreenCell cell;
    for (int row = 0; row < m_rows; ++row) {
        const int line = first + row;
        const qreal y = row * m_cellHeight;

        // Backgrounds first, in runs of one color.
        int runStart = 0;
        QColor runColor;
        auto flushBackground = [&](int end) {
            if (end > runStart && runColor.isValid() && runColor != m_background)
                painter->fillRect(QRectF(runStart * m_cellWidth, y, (end - runStart) * m_cellWidth, m_cellHeight), runColor);
        };
        for (int col = 0; col < m_cols; ++col) {
            QColor bg = m_background;
            if (cellAt(line, col, &cell)) {
                const bool reverse = bool(cell.attrs.reverse) != m_reverse;
                bg = reverse ? color(cell.fg, true, false) : color(cell.bg, false, false);
            } else if (m_reverse) {
                bg = m_foreground;
            }
            if (bg != runColor) {
                flushBackground(col);
                runStart = col;
                runColor = bg;
            }
        }
        flushBackground(m_cols);

        // Selection on top of the backgrounds.
        if (hasSelection()) {
            for (int col = 0; col < m_cols; ++col)
                if (inSelection(line, col))
                    painter->fillRect(QRectF(col * m_cellWidth, y, m_cellWidth, m_cellHeight), selectionColor);
        }

        // Then text: ASCII in runs of one style, everything else one cell at
        // a time so fallback fonts can't push the columns out of line.
        QString run;
        int runCol = 0;
        QColor runFg;
        int runStyle = -1;
        int runDecor = 0;
        auto flushText = [&]() {
            if (run.isEmpty())
                return;
            painter->setFont(runStyle == 3 ? m_boldItalic : runStyle == 2 ? m_italic : runStyle == 1 ? m_bold : m_font);
            painter->setPen(runFg);
            const qreal x = runCol * m_cellWidth;
            painter->drawText(QPointF(x, y + m_ascent), run);
            if (runDecor & 1)
                painter->fillRect(QRectF(x, y + m_ascent + 2, run.size() * m_cellWidth, 1), runFg);
            if (runDecor & 2)
                painter->fillRect(QRectF(x, y + m_ascent - m_cellHeight * 0.28, run.size() * m_cellWidth, 1), runFg);
            run.clear();
        };
        for (int col = 0; col < m_cols; ++col) {
            if (!cellAt(line, col, &cell) || cell.chars[0] == 0 || cell.attrs.conceal) {
                flushText();
                continue;
            }
            const bool reverse = bool(cell.attrs.reverse) != m_reverse;
            const bool bold = cell.attrs.bold && m_boldText;
            QColor fg = reverse ? color(cell.bg, false, false) : color(cell.fg, true, bold);
            const int style = (bold ? 1 : 0) | (cell.attrs.italic ? 2 : 0);
            const int decor = (cell.attrs.underline ? 1 : 0) | (cell.attrs.strike ? 2 : 0);
            const bool ascii = cell.chars[0] < 0x7f && cell.chars[1] == 0 && cell.width == 1;
            if (!ascii || fg != runFg || style != runStyle || decor != runDecor || run.isEmpty())
                flushText();
            if (run.isEmpty()) {
                runCol = col;
                runFg = fg;
                runStyle = style;
                runDecor = decor;
            }
            if (ascii) {
                run += QChar(char16_t(cell.chars[0]));
            } else {
                for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL && cell.chars[i]; ++i)
                    run += QString::fromUcs4(reinterpret_cast<const char32_t *>(&cell.chars[i]), 1);
                flushText();
                if (cell.width > 1)
                    col += cell.width - 1;
            }
        }
        flushText();
    }

    // The cursor, on the live screen only.
    if (m_scrollOffset == 0 && m_cursorVisible && running()) {
        const bool focused = hasActiveFocus();
        const bool blinking = m_blinkSetting || m_cursorBlinkRequested;
        if (!focused || !blinking || m_blinkOn) {
            const QRectF r(m_cursor.col * m_cellWidth, m_cursor.row * m_cellHeight, m_cellWidth, m_cellHeight);
            const QColor c = m_cursorColor.isValid() ? m_cursorColor : m_foreground;
            if (!focused) {
                painter->setPen(QPen(c, 1));
                painter->setBrush(Qt::NoBrush);
                painter->drawRect(r.adjusted(0.5, 0.5, -0.5, -0.5));
            } else if (m_cursorShape == VTERM_PROP_CURSORSHAPE_UNDERLINE) {
                painter->fillRect(QRectF(r.left(), r.bottom() - 2, r.width(), 2), c);
            } else if (m_cursorShape == VTERM_PROP_CURSORSHAPE_BAR_LEFT) {
                painter->fillRect(QRectF(r.left(), r.top(), 2, r.height()), c);
            } else {
                painter->fillRect(r, c);
                // The character under the block, in the background color.
                if (vterm_screen_get_cell(m_screen, m_cursor, &cell) && cell.chars[0]) {
                    painter->setFont(cell.attrs.bold && m_boldText ? m_bold : m_font);
                    painter->setPen(m_background);
                    QString ch;
                    for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL && cell.chars[i]; ++i)
                        ch += QString::fromUcs4(reinterpret_cast<const char32_t *>(&cell.chars[i]), 1);
                    painter->drawText(QPointF(r.left(), r.top() + m_ascent), ch);
                }
            }
        }
    }
}

// ── Scrolling ──────────────────────────────────────────────────────────────

void TerminalView::setScrollOffset(int offset) {
    offset = qBound(0, offset, int(m_scrollback.size()));
    if (offset == m_scrollOffset)
        return;
    m_scrollOffset = offset;
    emit scrollChanged();
    update();
}

void TerminalView::scrollToBottom() { setScrollOffset(0); }

// ── Keyboard ───────────────────────────────────────────────────────────────

VTermModifier TerminalView::modifiers(Qt::KeyboardModifiers m) const {
    int mod = VTERM_MOD_NONE;
    if (m & Qt::ShiftModifier)
        mod |= VTERM_MOD_SHIFT;
    if (m & Qt::AltModifier)
        mod |= VTERM_MOD_ALT;
    if (m & Qt::ControlModifier)
        mod |= VTERM_MOD_CTRL;
    return VTermModifier(mod);
}

void TerminalView::keyPressEvent(QKeyEvent *event) {
    m_composing.clear();
    const VTermModifier mod = modifiers(event->modifiers());
    const bool keypad = event->modifiers() & Qt::KeypadModifier;

    // Shift+PgUp/PgDn/Home/End page through the scrollback, as most
    // terminals do; the program never sees them.
    if ((event->modifiers() & ~Qt::KeypadModifier) == Qt::ShiftModifier) {
        const int page = qMax(1, m_rows - 1);
        switch (event->key()) {
        case Qt::Key_PageUp: setScrollOffset(m_scrollOffset + page); return;
        case Qt::Key_PageDown: setScrollOffset(m_scrollOffset - page); return;
        case Qt::Key_Home: if (!m_altScreen) { setScrollOffset(int(m_scrollback.size())); return; } break;
        case Qt::Key_End: if (!m_altScreen) { scrollToBottom(); return; } break;
        default: break;
        }
    }

    VTermKey key = VTERM_KEY_NONE;
    switch (event->key()) {
    case Qt::Key_Return: key = VTERM_KEY_ENTER; break;
    case Qt::Key_Enter: key = keypad ? VTERM_KEY_KP_ENTER : VTERM_KEY_ENTER; break;
    case Qt::Key_Tab: key = VTERM_KEY_TAB; break;
    case Qt::Key_Backtab: key = VTERM_KEY_TAB; break;
    case Qt::Key_Backspace: key = VTERM_KEY_BACKSPACE; break;
    case Qt::Key_Escape: key = VTERM_KEY_ESCAPE; break;
    case Qt::Key_Up: key = VTERM_KEY_UP; break;
    case Qt::Key_Down: key = VTERM_KEY_DOWN; break;
    case Qt::Key_Left: key = VTERM_KEY_LEFT; break;
    case Qt::Key_Right: key = VTERM_KEY_RIGHT; break;
    case Qt::Key_Insert: key = VTERM_KEY_INS; break;
    case Qt::Key_Delete: key = VTERM_KEY_DEL; break;
    case Qt::Key_Home: key = VTERM_KEY_HOME; break;
    case Qt::Key_End: key = VTERM_KEY_END; break;
    case Qt::Key_PageUp: key = VTERM_KEY_PAGEUP; break;
    case Qt::Key_PageDown: key = VTERM_KEY_PAGEDOWN; break;
    default:
        if (event->key() >= Qt::Key_F1 && event->key() <= Qt::Key_F35)
            key = VTermKey(VTERM_KEY_FUNCTION(event->key() - Qt::Key_F1 + 1));
        break;
    }

    if (key != VTERM_KEY_NONE) {
        clearSelection();
        scrollToBottom();
        // Shift+Tab is "back tab", sent as Tab with Shift.
        const VTermModifier m = event->key() == Qt::Key_Backtab ? VTermModifier(mod | VTERM_MOD_SHIFT) : mod;
        vterm_keyboard_key(m_vt, key, m);
        event->accept();
        return;
    }

    // Ctrl and Alt with a letter or symbol: send the key's own character,
    // since Qt's text for Ctrl+A is already a control code.
    if (mod & (VTERM_MOD_CTRL | VTERM_MOD_ALT)) {
        uint32_t c = 0;
        const int k = event->key();
        if (k >= Qt::Key_A && k <= Qt::Key_Z)
            c = uint32_t('a' + (k - Qt::Key_A));
        else if (k >= 0x20 && k < 0x7f)
            c = uint32_t(k);
        else if (!event->text().isEmpty())
            c = event->text().toUcs4().value(0);
        if (c) {
            // Alt+Shift+a should send ESC A.
            if ((mod & VTERM_MOD_SHIFT) && c >= 'a' && c <= 'z' && !(mod & VTERM_MOD_CTRL))
                c -= 32;
            clearSelection();
            scrollToBottom();
            vterm_keyboard_unichar(m_vt, c, mod);
            event->accept();
            return;
        }
    }

    const QString text = event->text();
    if (!text.isEmpty() && text.at(0).isPrint()) {
        clearSelection();
        scrollToBottom();
        for (const char32_t c : text.toUcs4())
            vterm_keyboard_unichar(m_vt, uint32_t(c), takeSticky());
        event->accept();
        return;
    }
    if (!text.isEmpty()) {
        // Control codes Qt produced on its own (dead keys and the like).
        write(text.toUtf8());
        event->accept();
        return;
    }
    event->ignore();
}

void TerminalView::typeCharacters(const QString &text) {
    for (const char32_t c : text.toUcs4()) {
        // Touch keyboards send Enter as a newline character.
        if (c == '\n') {
            vterm_keyboard_key(m_vt, VTERM_KEY_ENTER, takeSticky());
            continue;
        }
        uint32_t ch = uint32_t(c);
        const VTermModifier mod = takeSticky();
        if ((mod & VTERM_MOD_CTRL) && ch >= 'A' && ch <= 'Z')
            ch += 32;
        vterm_keyboard_unichar(m_vt, ch, mod);
    }
}

// Touch keyboards like Gboard hold a word as "composing" text and commit it
// later, at a space or Enter. A terminal can't wait for that: a password or
// "yes" would never arrive. So composing text is typed as it grows, and when
// the keyboard changes it, backspaces take back what differs.
void TerminalView::inputMethodEvent(QInputMethodEvent *event) {
    // Some keyboards delete by editing the text before the cursor instead of
    // pressing Backspace. All they can see there is the padding (see
    // inputMethodQuery), so each character they remove is a Backspace.
    if (event->replacementStart() < 0 && event->replacementLength() > 0) {
        const int erase = qMin(event->replacementLength(), -event->replacementStart());
        clearSelection();
        scrollToBottom();
        for (int i = 0; i < erase; ++i)
            vterm_keyboard_key(m_vt, VTERM_KEY_BACKSPACE, VTERM_MOD_NONE);
    }
    const QString target = event->commitString() + event->preeditString();
    int common = 0;
    while (common < m_composing.size() && common < target.size() && m_composing.at(common) == target.at(common))
        ++common;
    const int erase = int(m_composing.mid(common).toUcs4().size());
    const QString add = target.mid(common);
    if (erase > 0 || !add.isEmpty()) {
        clearSelection();
        scrollToBottom();
        for (int i = 0; i < erase; ++i)
            vterm_keyboard_key(m_vt, VTERM_KEY_BACKSPACE, VTERM_MOD_NONE);
        typeCharacters(add);
    }
    // What's committed is final; only the composing part may still change.
    m_composing = event->preeditString();
    event->accept();
}

Qt::InputMethodHints TerminalView::inputMethodHints() const {
    // Sensitive data keeps touch keyboards from holding words back for
    // suggestions, so each letter reaches the terminal as it's typed.
    if (m_secretInput)
        return Qt::ImhHiddenText | Qt::ImhSensitiveData | Qt::ImhNoPredictiveText | Qt::ImhNoAutoUppercase;
    return Qt::ImhNoPredictiveText | Qt::ImhNoAutoUppercase | Qt::ImhSensitiveData | Qt::ImhPreferLatin;
}

QVariant TerminalView::inputMethodQuery(Qt::InputMethodQuery query) const {
    switch (query) {
    case Qt::ImEnabled:
        return true;
    case Qt::ImCursorRectangle:
        return QRectF(m_cursor.col * m_cellWidth, m_cursor.row * m_cellHeight, m_cellWidth, m_cellHeight);
    case Qt::ImFont:
        return m_font;
    // Keyboards ask what surrounds the cursor; the word being composed is all
    // they can really edit, since what's sent is out of their hands. A few
    // spaces of padding come before it, so a keyboard that deletes by
    // removing the character before the cursor always finds one to remove.
    case Qt::ImSurroundingText:
        return QString(InputPadding, u' ') + m_composing;
    case Qt::ImCursorPosition:
    case Qt::ImAnchorPosition:
        return int(InputPadding + m_composing.size());
    case Qt::ImCurrentSelection:
        return QString();
    // A Return key that types Enter; the default "Done" closes the keyboard
    // after every command.
    case Qt::ImEnterKeyType:
        return int(Qt::EnterKeyReturn);
    case Qt::ImHints:
        return int(inputMethodHints());
    default:
        return QQuickPaintedItem::inputMethodQuery(query);
    }
}

void TerminalView::focusInEvent(QFocusEvent *event) {
    QQuickPaintedItem::focusInEvent(event);
    if (m_secretInput && touchKeyboard() && QGuiApplication::inputMethod())
        QGuiApplication::inputMethod()->show();
    vterm_state_focus_in(vterm_obtain_state(m_vt));
    m_blinkOn = true;
    if (m_blinkSetting || m_cursorBlinkRequested)
        m_blinkTimer.start();
    update();
}

void TerminalView::focusOutEvent(QFocusEvent *event) {
    m_composing.clear();
    QQuickPaintedItem::focusOutEvent(event);
    vterm_state_focus_out(vterm_obtain_state(m_vt));
    m_blinkTimer.stop();
    m_blinkOn = true;
    update();
}

// ── Mouse ──────────────────────────────────────────────────────────────────

QPoint TerminalView::cellFromPixel(const QPointF &pos, bool clamp) const {
    int col = int(pos.x() / m_cellWidth);
    int row = int(pos.y() / m_cellHeight);
    if (clamp) {
        col = qBound(0, col, m_cols - 1);
        row = qBound(0, row, m_rows - 1);
    }
    return QPoint(col, row);
}

// Programs like vim and htop that ask for the mouse get it, unless Shift is
// held, which always selects text.
bool TerminalView::reportMouse(Qt::KeyboardModifiers modifiers) const {
    return m_mouseMode != VTERM_PROP_MOUSE_NONE && !(modifiers & Qt::ShiftModifier);
}

static int vtermButton(Qt::MouseButton button) {
    switch (button) {
    case Qt::LeftButton: return 1;
    case Qt::MiddleButton: return 2;
    case Qt::RightButton: return 3;
    default: return 0;
    }
}

void TerminalView::mousePressEvent(QMouseEvent *event) {
    forceActiveFocus(Qt::MouseFocusReason);
    const QPoint cell = cellFromPixel(event->position());
    if (reportMouse(event->modifiers())) {
        const VTermModifier mod = modifiers(event->modifiers());
        vterm_mouse_move(m_vt, cell.y(), cell.x(), mod);
        if (const int b = vtermButton(event->button()))
            vterm_mouse_button(m_vt, b, true, mod);
        ++m_mouseButtonsDown;
        event->accept();
        return;
    }
    if (event->button() == Qt::MiddleButton) {
        // Paste the primary selection, as X11 and Wayland terminals do.
        QString text = QGuiApplication::clipboard()->text(QClipboard::Selection);
        if (!text.isEmpty()) {
            text.replace("\r\n", "\r").replace('\n', '\r');
            scrollToBottom();
            vterm_keyboard_start_paste(m_vt);
            write(text.toUtf8());
            vterm_keyboard_end_paste(m_vt);
        }
        event->accept();
        return;
    }
    if (event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }
    const QPoint absolute(cell.x(), firstVisibleLine() + cell.y());
    // A third click soon after a double click selects the line.
    if (QDateTime::currentMSecsSinceEpoch() - m_lastDoubleClick < QGuiApplication::styleHints()->mouseDoubleClickInterval()) {
        m_selectUnit = 2;
        selectLineAt(absolute);
    } else if (event->modifiers() & Qt::ShiftModifier && hasSelection()) {
        m_selectionEnd = absolute;
        m_selecting = true;
    } else {
        m_selectUnit = 0;
        m_selectionAnchor = m_selectionStart = m_selectionEnd = absolute;
        m_selecting = true;
    }
    emit selectionChanged();
    update();
    event->accept();
}

void TerminalView::mouseDoubleClickEvent(QMouseEvent *event) {
    if (reportMouse(event->modifiers()) || event->button() != Qt::LeftButton) {
        mousePressEvent(event);
        return;
    }
    const QPoint cell = cellFromPixel(event->position());
    m_selectUnit = 1;
    selectWordAt(QPoint(cell.x(), firstVisibleLine() + cell.y()));
    m_lastDoubleClick = QDateTime::currentMSecsSinceEpoch();
    event->accept();
}

void TerminalView::mouseMoveEvent(QMouseEvent *event) {
    const QPoint cell = cellFromPixel(event->position());
    if (m_mouseButtonsDown > 0 && reportMouse(event->modifiers())) {
        vterm_mouse_move(m_vt, cell.y(), cell.x(), modifiers(event->modifiers()));
        event->accept();
        return;
    }
    if (!m_selecting)
        return;
    // Dragging past the edges scrolls through the history.
    if (event->position().y() < 0)
        setScrollOffset(m_scrollOffset + 1);
    else if (event->position().y() > height())
        setScrollOffset(m_scrollOffset - 1);
    const QPoint absolute(cell.x(), firstVisibleLine() + cell.y());
    if (m_selectUnit == 0) {
        m_selectionStart = m_selectionAnchor;
        m_selectionEnd = absolute;
    } else {
        // Extend by whole words or lines from where the double or triple click was.
        const QPoint anchorStart = m_selectionAnchor;
        if (m_selectUnit == 1)
            selectWordAt(absolute);
        else
            selectLineAt(absolute);
        const QPoint s = m_selectionStart, e = m_selectionEnd;
        const bool after = absolute.y() > anchorStart.y() || (absolute.y() == anchorStart.y() && absolute.x() >= anchorStart.x());
        m_selectionAnchor = anchorStart;
        if (after)
            m_selectionStart = anchorStart, m_selectionEnd = e;
        else
            m_selectionStart = s, m_selectionEnd = anchorStart;
    }
    emit selectionChanged();
    update();
    event->accept();
}

void TerminalView::mouseReleaseEvent(QMouseEvent *event) {
#if defined(Q_OS_ANDROID) || defined(Q_OS_IOS)
    // A tap brings up the on-screen keyboard.
    if (!m_selecting || m_selectionStart == m_selectionEnd)
        QGuiApplication::inputMethod()->show();
#endif
    if (m_mouseButtonsDown > 0 && reportMouse(event->modifiers())) {
        const QPoint cell = cellFromPixel(event->position());
        const VTermModifier mod = modifiers(event->modifiers());
        vterm_mouse_move(m_vt, cell.y(), cell.x(), mod);
        if (const int b = vtermButton(event->button()))
            vterm_mouse_button(m_vt, b, false, mod);
        m_mouseButtonsDown = qMax(0, m_mouseButtonsDown - 1);
        event->accept();
        return;
    }
    m_mouseButtonsDown = 0;
    if (m_selecting)
        finishSelection();
    event->accept();
}

void TerminalView::hoverMoveEvent(QHoverEvent *event) {
    if (m_mouseMode == VTERM_PROP_MOUSE_MOVE && !(event->modifiers() & Qt::ShiftModifier)) {
        const QPoint cell = cellFromPixel(event->position());
        vterm_mouse_move(m_vt, cell.y(), cell.x(), modifiers(event->modifiers()));
    }
}

void TerminalView::wheelEvent(QWheelEvent *event) {
    const int notches = event->angleDelta().y() / 120;
    if (notches == 0) {
        event->accept();
        return;
    }
    if (reportMouse(event->modifiers())) {
        const QPoint cell = cellFromPixel(event->position());
        const VTermModifier mod = modifiers(event->modifiers());
        vterm_mouse_move(m_vt, cell.y(), cell.x(), mod);
        for (int i = 0; i < qAbs(notches); ++i)
            vterm_mouse_button(m_vt, notches > 0 ? 4 : 5, true, mod);
    } else if (m_altScreen) {
        // Full-screen programs without mouse support, like less, get arrows.
        for (int i = 0; i < qAbs(notches) * 3; ++i)
            vterm_keyboard_key(m_vt, notches > 0 ? VTERM_KEY_UP : VTERM_KEY_DOWN, VTERM_MOD_NONE);
    } else {
        setScrollOffset(m_scrollOffset + notches * 3);
    }
    event->accept();
}

// ── Selection ──────────────────────────────────────────────────────────────

bool TerminalView::inSelection(int line, int column) const {
    QPoint a = m_selectionStart, b = m_selectionEnd;
    if (a.y() > b.y() || (a.y() == b.y() && a.x() > b.x()))
        std::swap(a, b);
    if (a == b)
        return false;
    if (line < a.y() || line > b.y())
        return false;
    if (line == a.y() && column < a.x())
        return false;
    if (line == b.y() && column >= b.x() + (m_selectUnit == 0 ? 0 : 1))
        return false;
    return true;
}

static bool wordCharacter(uint32_t c) {
    if (c == 0 || c == ' ')
        return false;
    // Paths, URLs, and options stay whole.
    static const QString punctuation = QStringLiteral("-_./~:@%+=?&#");
    return QChar::isLetterOrNumber(char32_t(c)) || (c < 0x80 && punctuation.contains(QChar(char16_t(c)))) || c > 0x2000;
}

void TerminalView::selectWordAt(const QPoint &cell) {
    VTermScreenCell c;
    auto isWord = [&](int col) { return cellAt(cell.y(), col, &c) && wordCharacter(c.chars[0]); };
    int start = cell.x(), end = cell.x();
    if (isWord(cell.x())) {
        while (start > 0 && isWord(start - 1))
            --start;
        while (end < m_cols - 1 && isWord(end + 1))
            ++end;
    }
    m_selectionAnchor = QPoint(start, cell.y());
    m_selectionStart = QPoint(start, cell.y());
    m_selectionEnd = QPoint(end, cell.y());
    emit selectionChanged();
    update();
}

void TerminalView::selectLineAt(const QPoint &cell) {
    m_selectionAnchor = QPoint(0, cell.y());
    m_selectionStart = QPoint(0, cell.y());
    m_selectionEnd = QPoint(m_cols - 1, cell.y());
    m_selecting = true;
    emit selectionChanged();
    update();
}

void TerminalView::finishSelection() {
    m_selecting = false;
    const QString text = selectedText();
    if (text.isEmpty()) {
        clearSelection();
        return;
    }
    // Selecting puts the text in the primary selection, for middle-click paste.
    if (QGuiApplication::clipboard()->supportsSelection())
        QGuiApplication::clipboard()->setText(text, QClipboard::Selection);
    emit selectionChanged();
}

void TerminalView::clearSelection() {
    if (!hasSelection())
        return;
    m_selecting = false;
    m_selectionStart = m_selectionEnd = m_selectionAnchor = QPoint(-1, -1);
    emit selectionChanged();
    update();
}

QString TerminalView::selectedText() const {
    QPoint a = m_selectionStart, b = m_selectionEnd;
    if (a.y() > b.y() || (a.y() == b.y() && a.x() > b.x()))
        std::swap(a, b);
    if (a == b || a.y() < 0)
        return {};
    QStringList lines;
    for (int line = a.y(); line <= b.y(); ++line) {
        QString text;
        VTermScreenCell cell;
        for (int col = 0; col < m_cols; ++col) {
            if (!inSelection(line, col))
                continue;
            if (!cellAt(line, col, &cell) || cell.chars[0] == 0) {
                text += ' ';
                continue;
            }
            for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL && cell.chars[i]; ++i)
                text += QString::fromUcs4(reinterpret_cast<const char32_t *>(&cell.chars[i]), 1);
            if (cell.width > 1)
                col += cell.width - 1;
        }
        while (text.endsWith(' '))
            text.chop(1);
        lines << text;
    }
    return lines.join('\n');
}
