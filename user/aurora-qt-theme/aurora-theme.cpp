// Plugin de tema de plataforma de Qt5 para Aurora (QT_QPA_PLATFORMTHEME=aurora).
//
// Las apps Qt de widgets se ven como el resto de la interfaz: estilo Fusion,
// paleta oscura con los colores de kernel/gfx/theme.h, la misma tipografia que
// el tema GTK (DejaVu Sans 10), iconos Adwaita y una hoja QSS global con los
// radios y el acento de Aurora. La barra de titulo la dibuja el compositor.
#include <QApplication>
#include <QFont>
#include <QPalette>
#include <QTimer>
#include <qpa/qplatformtheme.h>
#include <qpa/qplatformthemeplugin.h>

namespace {

// AURORA_* de kernel/gfx/theme.h
const QColor kWin(0x1E, 0x1E, 0x1E);       // AURORA_WIN_BG
const QColor kField(0x1A, 0x1A, 0x1A);     // AURORA_GRAY_900
const QColor kAlt(0x24, 0x24, 0x24);       // AURORA_GRAY_800
const QColor kRaised(0x2E, 0x2E, 0x2E);    // AURORA_GRAY_700
const QColor kAccent(0x0A, 0x84, 0xFF);    // AURORA_ACCENT
const QColor kAccentHover(0x40, 0x9C, 0xFF);

const char *kStyleSheet = R"QSS(
QPushButton, QToolButton, QComboBox, QAbstractSpinBox {
  border-radius: 6px;
}
QPushButton {
  background-color: #2E2E2E; border: 1px solid rgba(255,255,255,0.10);
  padding: 3px 12px; min-height: 22px;
}
QPushButton:hover { background-color: #3A3A3A; }
QPushButton:pressed, QPushButton:checked { background-color: #454545; }
QPushButton:disabled { background-color: #242424; color: rgba(255,255,255,0.35); }
QPushButton:default { background-color: #0A84FF; border-color: rgba(0,0,0,0.25); color: white; }
QPushButton:default:hover { background-color: #409CFF; }
QToolButton { background: transparent; border: 1px solid transparent; padding: 3px; }
QToolButton:hover { background-color: rgba(255,255,255,0.10); }
QToolButton:pressed, QToolButton:checked { background-color: rgba(255,255,255,0.18); }
QLineEdit, QTextEdit, QPlainTextEdit, QAbstractSpinBox, QComboBox {
  background-color: #1A1A1A; border: 1px solid rgba(255,255,255,0.12);
  border-radius: 6px; padding: 2px 6px; min-height: 20px;
  selection-background-color: rgba(10,132,255,0.60); selection-color: white;
}
QLineEdit:focus, QTextEdit:focus, QPlainTextEdit:focus, QAbstractSpinBox:focus,
QComboBox:focus { border: 1px solid #0A84FF; }
QMenuBar { background-color: #262628; }
QMenuBar::item { background: transparent; padding: 3px 8px; border-radius: 5px; }
QMenuBar::item:selected { background-color: rgba(255,255,255,0.14); }
QMenu {
  background-color: #2A2A2C; border: 1px solid rgba(255,255,255,0.14);
  border-radius: 8px; padding: 4px;
}
QMenu::item { padding: 4px 22px 4px 18px; border-radius: 5px; }
QMenu::item:selected { background-color: #0A84FF; color: white; }
QMenu::item:disabled { color: rgba(255,255,255,0.35); }
QMenu::separator { height: 1px; background: rgba(255,255,255,0.14); margin: 4px 8px; }
QToolBar { background-color: #262628; border: none; spacing: 4px; padding: 2px; }
QStatusBar { background-color: #262628; color: rgba(255,255,255,0.60); }
QTabWidget::pane { border: 1px solid rgba(255,255,255,0.14); border-radius: 6px; top: -1px; }
QTabBar::tab {
  background: transparent; padding: 4px 14px; margin: 2px 1px;
  border-radius: 6px; color: rgba(255,255,255,0.80);
}
QTabBar::tab:hover { background-color: rgba(255,255,255,0.08); }
QTabBar::tab:selected { background-color: rgba(255,255,255,0.16); color: white; }
QHeaderView::section {
  background-color: #262628; color: rgba(255,255,255,0.80);
  border: none; border-right: 1px solid rgba(0,0,0,0.40); padding: 3px 8px;
}
QAbstractItemView {
  background-color: #1A1A1A; alternate-background-color: #202020;
  border: 1px solid rgba(255,255,255,0.14); border-radius: 6px;
  selection-background-color: #0A84FF; selection-color: white;
}
QScrollBar:vertical { background: transparent; width: 12px; margin: 0; }
QScrollBar:horizontal { background: transparent; height: 12px; margin: 0; }
QScrollBar::handle:vertical { background: rgba(255,255,255,0.35); border-radius: 4px; min-height: 24px; margin: 2px; }
QScrollBar::handle:horizontal { background: rgba(255,255,255,0.35); border-radius: 4px; min-width: 24px; margin: 2px; }
QScrollBar::handle:hover { background: rgba(255,255,255,0.55); }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }
QCheckBox::indicator, QRadioButton::indicator, QGroupBox::indicator {
  width: 16px; height: 16px; background-color: #2E2E2E;
  border: 1px solid rgba(255,255,255,0.25);
}
QCheckBox::indicator, QGroupBox::indicator { border-radius: 4px; }
QRadioButton::indicator { border-radius: 9px; }
QCheckBox::indicator:hover, QRadioButton::indicator:hover { border-color: rgba(255,255,255,0.45); }
QCheckBox::indicator:checked, QGroupBox::indicator:checked {
  background-color: #0A84FF; border-color: #0A84FF; image: url(@DATA@/check.svg);
}
QCheckBox::indicator:indeterminate {
  background-color: #0A84FF; border-color: #0A84FF; image: url(@DATA@/dash.svg);
}
QRadioButton::indicator:checked {
  background-color: #0A84FF; border-color: #0A84FF; image: url(@DATA@/radio-dot.svg);
}
QCheckBox::indicator:disabled, QRadioButton::indicator:disabled { opacity: 0.4; border-color: rgba(255,255,255,0.12); }
QProgressBar { background-color: #3A3A3A; border: none; border-radius: 8px; text-align: center; min-height: 16px; }
QProgressBar::chunk { background-color: #0A84FF; border-radius: 8px; }
QSlider::groove:horizontal { background: #3A3A3A; height: 4px; border-radius: 2px; }
QSlider::sub-page:horizontal { background: #0A84FF; border-radius: 2px; }
QSlider::handle:horizontal { background: white; width: 16px; height: 16px; margin: -6px 0; border-radius: 8px; }
QToolTip {
  background-color: #2E2E2E; color: white; border: 1px solid rgba(255,255,255,0.14);
  border-radius: 6px; padding: 3px;
}
QGroupBox { border: 1px solid rgba(255,255,255,0.14); border-radius: 6px; margin-top: 8px; padding-top: 6px; }
QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 4px; color: rgba(255,255,255,0.80); }
QDialog, QMainWindow { background-color: #1E1E1E; }
)QSS";

class AuroraTheme : public QPlatformTheme {
public:
  AuroraTheme() {
    // El QApplication aun no existe del todo: se aplica ya dentro del bucle.
    QTimer::singleShot(0, [] {
      if (auto *app = qobject_cast<QApplication *>(QCoreApplication::instance()))
        if (app->styleSheet().isEmpty())
          {
          // Datos del tema (iconos de casillas y radios); AURORA_QT_DATA sirve
          // para probar en el host sin instalar nada en /usr/share.
          QString data = qEnvironmentVariable("AURORA_QT_DATA",
                                              QStringLiteral("/usr/share/aurora/qt"));
          app->setStyleSheet(
              QString::fromUtf8(kStyleSheet).replace(QLatin1String("@DATA@"), data));
        }
    });
    buildPalette();
    m_font.setFamily(QStringLiteral("DejaVu Sans"));
    m_font.setPointSize(10);
  }

  QVariant themeHint(ThemeHint hint) const override {
    switch (hint) {
    case StyleNames:
      return QStringList{QStringLiteral("Fusion")};
    case SystemIconThemeName:
      return QStringLiteral("Adwaita");
    case SystemIconFallbackThemeName:
      return QStringLiteral("hicolor");
    case IconThemeSearchPaths:
      return QStringList{QStringLiteral("/usr/share/icons")};
    case ToolButtonStyle:
      return int(Qt::ToolButtonIconOnly);
    case KeyboardScheme:
      return int(GnomeKeyboardScheme);
    case CursorFlashTime:
      return 1200;
    case DialogButtonBoxButtonsHaveIcons:
      return false;
    case UseFullScreenForPopupMenu:
      return false;
    case ShowShortcutsInContextMenus:
      return true;
    default:
      return QPlatformTheme::themeHint(hint);
    }
  }

  const QPalette *palette(Palette type = SystemPalette) const override {
    return type == SystemPalette ? &m_palette : nullptr;
  }

  const QFont *font(Font type = SystemFont) const override {
    switch (type) {
    case SystemFont:
    case MenuFont:
    case MenuBarFont:
    case MenuItemFont:
    case MessageBoxFont:
    case LabelFont:
    case TipLabelFont:
    case StatusBarFont:
    case PushButtonFont:
    case ItemViewFont:
    case ListViewFont:
    case HeaderViewFont:
    case ListBoxFont:
    case ComboLineEditFont:
    case SmallFont:
    case MiniFont:
      return &m_font;
    default:
      return nullptr;
    }
  }

private:
  void buildPalette() {
    QPalette p;
    const QColor text(Qt::white);
    const QColor dim(255, 255, 255, 89); // AURORA_TEXT_DISABLED
    p.setColor(QPalette::Window, kWin);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, kField);
    p.setColor(QPalette::AlternateBase, kAlt);
    p.setColor(QPalette::ToolTipBase, kRaised);
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, kRaised);
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::BrightText, Qt::white);
    p.setColor(QPalette::Light, QColor(0x52, 0x52, 0x52));
    p.setColor(QPalette::Midlight, QColor(0x3A, 0x3A, 0x3A));
    p.setColor(QPalette::Mid, QColor(0x2A, 0x2A, 0x2A));
    p.setColor(QPalette::Dark, QColor(0x14, 0x14, 0x14));
    p.setColor(QPalette::Shadow, Qt::black);
    p.setColor(QPalette::Highlight, kAccent);
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::Link, kAccentHover);
    p.setColor(QPalette::LinkVisited, QColor(0xBF, 0x5A, 0xF2));
    p.setColor(QPalette::PlaceholderText, QColor(255, 255, 255, 120));
    for (auto r : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
      p.setColor(QPalette::Disabled, r, dim);
    p.setColor(QPalette::Disabled, QPalette::Highlight, QColor(0x3A, 0x3A, 0x3A));
    m_palette = p;
  }

  QPalette m_palette;
  QFont m_font;
};

} // namespace

class AuroraThemePlugin : public QPlatformThemePlugin {
  Q_OBJECT
  Q_PLUGIN_METADATA(IID QPlatformThemeFactoryInterface_iid FILE "aurora.json")
public:
  QPlatformTheme *create(const QString &key, const QStringList &) override {
    return key.compare(QLatin1String("aurora"), Qt::CaseInsensitive) == 0
               ? new AuroraTheme
               : nullptr;
  }
};

#include "aurora-theme.moc"
