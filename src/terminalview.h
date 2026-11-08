#pragma once

#include <QFont>
#include <QPoint>
#include <QQuickPaintedItem>
#include <QTimer>
#include <QVariant>
#include <QVector>

#include <deque>

#include "vterm.h"

class Pty;
class QSerialPort;
class SshSession;
class QFile;

// A terminal: libvterm keeps the screen, a Pty runs the program, and this
// item draws the cells, takes the keyboard and mouse, and keeps scrollback
// and the selection. The same code runs on Linux, Windows, and Android.
class TerminalView : public QQuickPaintedItem {
    Q_OBJECT
    Q_PROPERTY(QFont font READ font WRITE setFont NOTIFY fontChanged)
    // background, foreground, cursor, and palette (the 16 ANSI colors).
    Q_PROPERTY(QVariantMap colors READ colors WRITE setColors NOTIFY colorsChanged)
    Q_PROPERTY(int lineSpacing READ lineSpacing WRITE setLineSpacing NOTIFY fontChanged)
    Q_PROPERTY(bool blinkingCursor READ blinkingCursor WRITE setBlinkingCursor NOTIFY optionsChanged)
    Q_PROPERTY(bool boldText READ boldText WRITE setBoldText NOTIFY optionsChanged)
    // Everything the program prints is also appended here, when set.
    Q_PROPERTY(QString logFile READ logFile WRITE setLogFile NOTIFY optionsChanged)
    Q_PROPERTY(int columns READ columns NOTIFY sizeChanged)
    Q_PROPERTY(int rows READ rows NOTIFY sizeChanged)
    Q_PROPERTY(QString title READ title NOTIFY titleChanged)
    Q_PROPERTY(bool running READ running NOTIFY runningChanged)
    Q_PROPERTY(int scrollbackLines READ scrollbackLines NOTIFY scrollChanged)
    Q_PROPERTY(int scrollOffset READ scrollOffset WRITE setScrollOffset NOTIFY scrollChanged)
    Q_PROPERTY(bool hasSelection READ hasSelection NOTIFY selectionChanged)
    // On-screen Ctrl and Alt for touch keyboards: they apply to the next key.
    Q_PROPERTY(bool stickyCtrl READ stickyCtrl WRITE setStickyCtrl NOTIFY stickyChanged)
    Q_PROPERTY(bool stickyAlt READ stickyAlt WRITE setStickyAlt NOTIFY stickyChanged)

public:
    explicit TerminalView(QQuickItem *parent = nullptr);
    ~TerminalView() override;

    QFont font() const { return m_font; }
    void setFont(const QFont &font);
    QVariantMap colors() const { return m_colorMap; }
    void setColors(const QVariantMap &colors);
    int lineSpacing() const { return m_lineSpacing; }
    void setLineSpacing(int spacing);
    bool blinkingCursor() const { return m_blinkSetting; }
    void setBlinkingCursor(bool blink);
    bool boldText() const { return m_boldText; }
    void setBoldText(bool bold);
    QString logFile() const { return m_logPath; }
    void setLogFile(const QString &path);
    int columns() const { return m_cols; }
    int rows() const { return m_rows; }
    QString title() const { return m_title; }
    bool running() const;
    int scrollbackLines() const { return int(m_scrollback.size()); }
    int scrollOffset() const { return m_scrollOffset; }
    void setScrollOffset(int offset);
    bool hasSelection() const { return m_selecting || m_selectionStart != m_selectionEnd; }

    // environment: NAME=value pairs to add.
    Q_INVOKABLE bool start(const QString &program, const QStringList &args, const QString &workingDirectory = {},
                           const QStringList &environment = {});
    // A serial port opened by Tether itself (where there's no picocom):
    // device, baud, dataBits, parity, stopBits, flow, enter, and echo.
    Q_INVOKABLE bool startSerial(const QVariantMap &settings);
    // An ssh connection Tether makes itself (libssh), where there's no ssh
    // program: host, port, user, identityFile, password, command, knownHosts.
    Q_INVOKABLE bool startSsh(const QVariantMap &settings);
    // Answers the built-in ssh's hostKeyQuestion.
    Q_INVOKABLE void answerHostKey(bool trust);
    Q_INVOKABLE void terminate();
    // Types text as if pasted from the keyboard, without bracketed paste:
    // snippets use it, and \r presses Enter.
    Q_INVOKABLE void sendText(const QString &text);
    // A named key from an on-screen row: esc, tab, up, down, left, right,
    // home, end, pgup, pgdn; honors the sticky Ctrl and Alt.
    Q_INVOKABLE void sendKey(const QString &name);
    bool stickyCtrl() const { return m_stickyCtrl; }
    void setStickyCtrl(bool on) { if (on != m_stickyCtrl) { m_stickyCtrl = on; emit stickyChanged(); } }
    bool stickyAlt() const { return m_stickyAlt; }
    void setStickyAlt(bool on) { if (on != m_stickyAlt) { m_stickyAlt = on; emit stickyChanged(); } }
    Q_INVOKABLE void copyClipboard();
    Q_INVOKABLE void pasteClipboard();
    Q_INVOKABLE void clearScrollback();
    // The last lines on screen as plain text.
    Q_INVOKABLE QString screenText(int lines = 40) const;
    Q_INVOKABLE QString selectedText() const;

    // Feeds output straight in, for tests.
    void feed(const QByteArray &data);
    QString lineText(int absoluteLine) const;

    void paint(QPainter *painter) override;

signals:
    void fontChanged();
    void colorsChanged();
    void optionsChanged();
    void sizeChanged();
    void titleChanged();
    void runningChanged();
    void scrollChanged();
    void selectionChanged();
    void finished(int exitCode);
    void bell();
    void stickyChanged();
    void hostKeyQuestion(const QString &host, const QString &keyType, const QString &fingerprint, bool changed);

protected:
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;
    void keyPressEvent(QKeyEvent *event) override;
    void inputMethodEvent(QInputMethodEvent *event) override;
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    void hoverMoveEvent(QHoverEvent *event) override;

private:
    friend struct TerminalCallbacks;

    void updateMetrics();
    void resizeTerminal();
    void applyColors();
    void write(const QByteArray &data);
    QColor color(const VTermColor &c, bool foreground, bool bold) const;
    bool cellAt(int absoluteLine, int column, VTermScreenCell *cell) const;
    QPoint cellFromPixel(const QPointF &pos, bool clamp = true) const;
    int firstVisibleLine() const { return int(m_scrollback.size()) - m_scrollOffset; }
    void scrollToBottom();
    bool inSelection(int absoluteLine, int column) const;
    void selectWordAt(const QPoint &cell);
    void selectLineAt(const QPoint &cell);
    void finishSelection();
    void clearSelection();
    bool reportMouse(Qt::KeyboardModifiers modifiers) const;
    VTermModifier modifiers(Qt::KeyboardModifiers m) const;

    // libvterm callbacks
    int onDamage(VTermRect rect);
    int onMoveCursor(VTermPos pos, int visible);
    int onTermProp(VTermProp prop, VTermValue *value);
    int onPushLine(int cols, const VTermScreenCell *cells);
    int onPopLine(int cols, VTermScreenCell *cells);
    int onClearScrollback();

    VTerm *m_vt = nullptr;
    VTermScreen *m_screen = nullptr;
    Pty *m_pty = nullptr;
    QSerialPort *m_serial = nullptr;
    SshSession *m_ssh = nullptr;
    bool m_sshRunning = false;
    QString m_serialEnter;   // what Enter sends on a serial port: cr, lf, or crlf
    bool m_serialEcho = false;
    QFile *m_log = nullptr;
    QString m_logPath;

    QFont m_font;
    QFont m_bold, m_italic, m_boldItalic;
    qreal m_cellWidth = 8;
    qreal m_cellHeight = 16;
    qreal m_ascent = 12;
    int m_lineSpacing = 0;
    int m_cols = 80;
    int m_rows = 24;

    QVariantMap m_colorMap;
    QColor m_background{"#121212"};
    QColor m_foreground{"#e6e6e6"};
    QColor m_cursorColor;
    QVector<QColor> m_palette;

    std::deque<QVector<VTermScreenCell>> m_scrollback;
    int m_scrollOffset = 0;
    static constexpr int MaxScrollback = 10000;

    VTermPos m_cursor{0, 0};
    bool m_cursorVisible = true;
    bool m_cursorBlinkRequested = false;
    int m_cursorShape = VTERM_PROP_CURSORSHAPE_BLOCK;
    bool m_blinkSetting = false;
    bool m_blinkOn = true;
    QTimer m_blinkTimer;
    bool m_boldText = true;
    bool m_altScreen = false;
    int m_mouseMode = VTERM_PROP_MOUSE_NONE;
    bool m_reverse = false;
    QString m_title;
    QByteArray m_titleBuffer;

    // Selection, in absolute lines (scrollback first, then the screen).
    QPoint m_selectionStart{-1, -1};
    QPoint m_selectionEnd{-1, -1};
    QPoint m_selectionAnchor{-1, -1};
    bool m_selecting = false;
    bool m_stickyCtrl = false;
    bool m_stickyAlt = false;
    VTermModifier takeSticky();
    void typeCharacters(const QString &text);
    QString m_composing;  // composing text from a touch keyboard, already typed
    int m_selectUnit = 0; // 0 characters, 1 words, 2 lines
    qint64 m_lastDoubleClick = 0;
    int m_mouseButtonsDown = 0;
};
