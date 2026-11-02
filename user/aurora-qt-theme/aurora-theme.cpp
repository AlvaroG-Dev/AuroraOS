// Plugin de tema de plataforma de Qt5 para Aurora (QT_QPA_PLATFORMTHEME=aurora).
//
// Las apps Qt de widgets se ven como el resto de la interfaz: estilo "aurora"
// (aurora-style.cpp), paleta oscura con los colores de kernel/gfx/theme.h,
// Inter 10 como en el compositor y el tema GTK, JetBrains Mono para texto
// monoespaciado e iconos del tema Aurora. La barra de titulo la dibuja el
// compositor.
#include <QApplication>
#include <QFont>
#include <QPalette>
#include <QTimer>
#include <cstdlib>
#include <cstring>
#include <sys/syscall.h>
#include <unistd.h>
#include <qpa/qplatformtheme.h>
#include <qpa/qplatformthemeplugin.h>

namespace {

// AURORA_* de kernel/gfx/theme.h
const QColor kWin(0x1E, 0x1E, 0x1E);       // AURORA_WIN_BG
const QColor kField(0x1A, 0x1A, 0x1A);     // AURORA_GRAY_900
const QColor kAlt(0x24, 0x24, 0x24);       // AURORA_GRAY_800
const QColor kRaised(0x2E, 0x2E, 0x2E);    // AURORA_GRAY_700
const QColor kAccent(0x3E, 0xE8, 0x9B);    // AURORA_ACCENT (verde aurora)
const QColor kAccentHover(0x6B, 0xF0, 0x83);

class AuroraTheme : public QPlatformTheme {
public:
  AuroraTheme() {
    buildPalette();
    m_font.setFamily(QStringLiteral("Inter"));
    m_font.setPointSize(10);
    m_mono.setFamily(QStringLiteral("JetBrains Mono"));
    m_mono.setStyleHint(QFont::Monospace);
    m_mono.setPointSize(10);
  }

  QVariant themeHint(ThemeHint hint) const override {
    switch (hint) {
    case StyleNames:
      return QStringList{QStringLiteral("aurora"), QStringLiteral("Fusion")};
    case SystemIconThemeName:
      return QStringLiteral("Aurora");
    case SystemIconFallbackThemeName:
      return QStringLiteral("Adwaita");
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
    case FixedFont:
      return &m_mono;
    default:
      return nullptr;
    }
  }

private:
  static bool pollLight() {
    long v = syscall(0x1070); // ASYS_THEME_GET
    if (v < 0) {
      const char *e = getenv("AURORA_THEME");
      return e && !strcmp(e, "light");
    }
    return v & 1;
  }

  void buildPalette() {
    if (m_light) {
      buildLight();
      return;
    }
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
    p.setColor(QPalette::HighlightedText, QColor(0x04, 0x26, 0x1A)); // AURORA_ACCENT_TEXT
    p.setColor(QPalette::Link, kAccentHover);
    p.setColor(QPalette::LinkVisited, QColor(0xBF, 0x5A, 0xF2));
    p.setColor(QPalette::PlaceholderText, QColor(255, 255, 255, 120));
    for (auto r : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
      p.setColor(QPalette::Disabled, r, dim);
    p.setColor(QPalette::Disabled, QPalette::Highlight, QColor(0x3A, 0x3A, 0x3A));
    m_palette = p;
  }

  void buildLight() {
    QPalette p;
    const QColor text(0x14, 0x21, 0x1B), win(0xF4, 0xF7, 0xF5), white(Qt::white);
    const QColor dim(0x14, 0x21, 0x1B, 89);
    p.setColor(QPalette::Window, win);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, white);
    p.setColor(QPalette::AlternateBase, QColor(0xEE, 0xF2, 0xF0));
    p.setColor(QPalette::ToolTipBase, QColor(0x2E, 0x2E, 0x2E));
    p.setColor(QPalette::ToolTipText, Qt::white);
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, QColor(0xE9, 0xEE, 0xEC));
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::BrightText, Qt::black);
    p.setColor(QPalette::Light, white);
    p.setColor(QPalette::Midlight, QColor(0xDD, 0xE4, 0xE1));
    p.setColor(QPalette::Mid, QColor(0xC9, 0xD3, 0xCE));
    p.setColor(QPalette::Dark, QColor(0xA9, 0xB5, 0xAF));
    p.setColor(QPalette::Shadow, Qt::black);
    p.setColor(QPalette::Highlight, kAccent);
    p.setColor(QPalette::HighlightedText, QColor(0x04, 0x26, 0x1A));
    p.setColor(QPalette::Link, QColor(0x1C, 0x8A, 0x5C));
    p.setColor(QPalette::LinkVisited, QColor(0x7A, 0x3E, 0xB8));
    p.setColor(QPalette::PlaceholderText, QColor(0x14, 0x21, 0x1B, 120));
    for (auto r : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
      p.setColor(QPalette::Disabled, r, dim);
    p.setColor(QPalette::Disabled, QPalette::Highlight, QColor(0xC9, 0xD3, 0xCE));
    m_palette = p;
  }

public:
  // Sondeo del modo: al cambiar, se reconstruye la paleta y se aplica.
  void startPolling() {
    auto *t = new QTimer(qApp);
    QObject::connect(t, &QTimer::timeout, qApp, [this]() {
      const bool l = pollLight();
      if (l == m_light || !qApp)
        return;
      m_light = l;
      buildPalette();
      QApplication::setPalette(m_palette);
    });
    t->start(400);
  }

private:
  bool m_light = pollLight();
  QPalette m_palette;
  QFont m_font, m_mono;
};

} // namespace

static QPlatformTheme *makeTheme() {
  auto *t = new AuroraTheme;
  QTimer::singleShot(0, [t]() { if (qApp) t->startPolling(); });
  return t;
}

class AuroraThemePlugin : public QPlatformThemePlugin {
  Q_OBJECT
  Q_PLUGIN_METADATA(IID QPlatformThemeFactoryInterface_iid FILE "aurora.json")
public:
  QPlatformTheme *create(const QString &key, const QStringList &) override {
    return key.compare(QLatin1String("aurora"), Qt::CaseInsensitive) == 0
               ? makeTheme()
               : nullptr;
  }
};

#include "aurora-theme.moc"
