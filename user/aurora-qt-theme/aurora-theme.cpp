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
  QFont m_font, m_mono;
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
