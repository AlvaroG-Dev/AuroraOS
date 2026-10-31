// Estilo Qt5 "Aurora": los controles se dibujan como el resto de la interfaz de
// Aurora (macOS Tahoe oscuro): esquinas redondeadas, botones con degradado
// suave, acento azul, casillas y radios con marca blanca, deslizadores con
// pomo blanco, scrollbars de superposicion, menus con seleccion redondeada,
// pestanas tipo segmento y "popup buttons" con el par de chevrons.
// Se apoya en Fusion para la geometria y para todo lo que no se redefine.
#include <QApplication>
#include <QComboBox>
#include <QFont>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QProxyStyle>
#include <QStyleFactory>
#include <QStyleOption>
#include <QStylePlugin>
#include <QWidget>

namespace {

const QColor kAccent(0x3E, 0xE8, 0x9B);   // AURORA_ACCENT (verde aurora)
const QColor kAccentHi(0x6B, 0xF0, 0x83); // AURORA_ACCENT_HOVER
const QColor kAccentLo(0x2D, 0xB8, 0x78);
const QColor kOnAccent(0x04, 0x26, 0x1A); // AURORA_ACCENT_TEXT
const QColor kMenuBg(0x2A, 0x2A, 0x2C);
const QColor kBar(0x26, 0x26, 0x28); // AURORA_TITLEBAR_ACTIVE

QColor alpha(QColor c, int a) {
  c.setAlpha(a);
  return c;
}
QColor white(int a) { return alpha(Qt::white, a); }
QColor shade(const QColor &c, int amount) { // >0 aclara, <0 oscurece
  return amount >= 0 ? c.lighter(100 + amount) : c.darker(100 - amount);
}

void drawChevron(QPainter *p, QPointF c, qreal w, bool up, const QColor &col,
                 qreal pen = 1.6) {
  p->save();
  p->setRenderHint(QPainter::Antialiasing);
  p->setPen(QPen(col, pen, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
  p->setBrush(Qt::NoBrush);
  qreal h = w * 0.55, d = up ? -1 : 1;
  QPainterPath path;
  path.moveTo(c.x() - w / 2, c.y() - d * h / 2);
  path.lineTo(c.x(), c.y() + d * h / 2);
  path.lineTo(c.x() + w / 2, c.y() - d * h / 2);
  p->drawPath(path);
  p->restore();
}

void roundRect(QPainter *p, const QRectF &r, qreal rad, const QBrush &fill,
               const QPen &pen = Qt::NoPen) {
  p->save();
  p->setRenderHint(QPainter::Antialiasing);
  p->setPen(pen);
  p->setBrush(fill);
  p->drawRoundedRect(r, rad, rad);
  p->restore();
}

void focusGlow(QPainter *p, const QRectF &r, qreal rad) {
  p->save();
  p->setRenderHint(QPainter::Antialiasing);
  p->setBrush(Qt::NoBrush);
  p->setPen(QPen(alpha(kAccent, 120), 3.0));
  p->drawRoundedRect(r.adjusted(-0.5, -0.5, 0.5, 0.5), rad + 1, rad + 1);
  p->setPen(QPen(kAccent, 1.0));
  p->drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), rad, rad);
  p->restore();
}

class AuroraStyle : public QProxyStyle {
public:
  AuroraStyle() : QProxyStyle(QStyleFactory::create(QStringLiteral("Fusion"))) {}

  void polish(QApplication *app) override {
    QProxyStyle::polish(app);
    QFont f(QStringLiteral("Inter"));
    f.setPointSize(10);
    app->setFont(f);
  }
  using QProxyStyle::polish;
  void polish(QWidget *w) override {
    QProxyStyle::polish(w);
    if (qobject_cast<QComboBox *>(w) || w->inherits("QAbstractButton") ||
        w->inherits("QTabBar") || w->inherits("QAbstractSlider") ||
        w->inherits("QAbstractSpinBox"))
      w->setAttribute(Qt::WA_Hover, true);
  }

  // ---- Metricas --------------------------------------------------------
  int pixelMetric(PixelMetric m, const QStyleOption *o,
                  const QWidget *w) const override {
    switch (m) {
    case PM_IndicatorWidth:
    case PM_IndicatorHeight:
    case PM_ExclusiveIndicatorWidth:
    case PM_ExclusiveIndicatorHeight:
      return 16;
    case PM_ScrollBarExtent:
      return 12;
    case PM_ScrollBarSliderMin:
      return 28;
    case PM_SliderThickness:
      return 20;
    case PM_SliderLength:
      return 16;
    case PM_MenuHMargin:
    case PM_MenuVMargin:
      return 4;
    case PM_MenuPanelWidth:
      return 1;
    case PM_ButtonMargin:
      return 8;
    case PM_DefaultFrameWidth:
      return 1;
    default:
      return QProxyStyle::pixelMetric(m, o, w);
    }
  }

  QSize sizeFromContents(ContentsType t, const QStyleOption *o, const QSize &s,
                         const QWidget *w) const override {
    QSize r = QProxyStyle::sizeFromContents(t, o, s, w);
    switch (t) {
    case CT_PushButton:
      r.setHeight(qMax(r.height(), 24));
      r.setWidth(qMax(r.width(), 64));
      break;
    case CT_MenuItem:
      if (const auto *mi = qstyleoption_cast<const QStyleOptionMenuItem *>(o))
        if (mi->menuItemType != QStyleOptionMenuItem::Separator)
          r.rheight() += 4;
      break;
    case CT_MenuBarItem:
      r.rwidth() += 6;
      break;
    case CT_TabBarTab:
      r.rwidth() += 8;
      break;
    case CT_ComboBox:
      r.setHeight(qMax(r.height(), 24));
      r.rwidth() += 6;
      break;
    default:
      break;
    }
    return r;
  }

  QRect subControlRect(ComplexControl cc, const QStyleOptionComplex *o,
                       SubControl sc, const QWidget *w) const override {
    if (cc == CC_ScrollBar) {
      if (const auto *sb = qstyleoption_cast<const QStyleOptionSlider *>(o)) {
        const QRect r = sb->rect;
        const bool horiz = sb->orientation == Qt::Horizontal;
        const int len = horiz ? r.width() : r.height();
        switch (sc) {
        case SC_ScrollBarSubLine:
        case SC_ScrollBarAddLine:
          return QRect();
        case SC_ScrollBarGroove:
          return r;
        case SC_ScrollBarSlider:
        case SC_ScrollBarSubPage:
        case SC_ScrollBarAddPage: {
          const int range = sb->maximum - sb->minimum;
          int slen = len;
          if (range > 0)
            slen = qBound(pixelMetric(PM_ScrollBarSliderMin, o, w),
                          int(qint64(len) * sb->pageStep / (range + sb->pageStep)),
                          len);
          const int pos =
              range > 0 ? sliderPositionFromValue(sb->minimum, sb->maximum,
                                                  sb->sliderPosition, len - slen,
                                                  sb->upsideDown)
                        : 0;
          QRect slider = horiz ? QRect(r.x() + pos, r.y(), slen, r.height())
                               : QRect(r.x(), r.y() + pos, r.width(), slen);
          if (sc == SC_ScrollBarSlider)
            return slider;
          if (sc == SC_ScrollBarSubPage)
            return horiz ? QRect(r.x(), r.y(), pos, r.height())
                         : QRect(r.x(), r.y(), r.width(), pos);
          return horiz ? QRect(slider.right() + 1, r.y(),
                               r.right() - slider.right(), r.height())
                       : QRect(r.x(), slider.bottom() + 1, r.width(),
                               r.bottom() - slider.bottom());
        }
        default:
          break;
        }
      }
    }
    if (cc == CC_ComboBox) {
      if (const auto *cb = qstyleoption_cast<const QStyleOptionComboBox *>(o)) {
        const QRect r = cb->rect;
        if (sc == SC_ComboBoxArrow)
          return QRect(r.right() - 20, r.top() + 3, 18, r.height() - 6);
        if (sc == SC_ComboBoxEditField)
          return QRect(r.left() + 8, r.top() + 2, r.width() - 32, r.height() - 4);
        if (sc == SC_ComboBoxListBoxPopup)
          return r;
      }
    }
    return QProxyStyle::subControlRect(cc, o, sc, w);
  }

  // ---- Primitivas ------------------------------------------------------
  void drawPrimitive(PrimitiveElement pe, const QStyleOption *o, QPainter *p,
                     const QWidget *w) const override {
    const bool enabled = o->state & State_Enabled;
    const bool hover = o->state & State_MouseOver;
    const bool down = o->state & (State_Sunken | State_On);
    const QRectF r = QRectF(o->rect).adjusted(0.5, 0.5, -0.5, -0.5);
    switch (pe) {
    case PE_PanelButtonCommand: {
      bool def = false;
      if (const auto *b = qstyleoption_cast<const QStyleOptionButton *>(o))
        def = b->features & QStyleOptionButton::DefaultButton;
      paintButton(p, r, enabled, hover, o->state & State_Sunken, o->state & State_On,
                  def);
      if ((o->state & State_HasFocus) && (o->state & State_KeyboardFocusChange))
        focusGlow(p, r, 6);
      return;
    }
    case PE_PanelButtonTool: {
      if (!enabled)
        return;
      const bool raised = o->state & State_AutoRaise;
      if ((o->state & State_Sunken) || (o->state & State_On))
        roundRect(p, r.adjusted(0, 0, 0, 0), 6, white(46));
      else if (hover)
        roundRect(p, r, 6, white(26));
      else if (!raised)
        paintButton(p, r, true, false, false, false, false);
      return;
    }
    case PE_FrameFocusRect:
      return; // el foco se dibuja como halo en cada control
    case PE_PanelLineEdit: {
      roundRect(p, r, 6, o->palette.brush(QPalette::Base),
                QPen(white(31), 1.0));
      if (o->state & State_HasFocus)
        focusGlow(p, r, 6);
      return;
    }
    case PE_FrameLineEdit:
      return; // incluido en PE_PanelLineEdit
    case PE_IndicatorCheckBox: {
      const QRectF b = QRectF(o->rect).adjusted(0.5, 0.5, -0.5, -0.5);
      const bool on = o->state & (State_On | State_NoChange);
      QColor fill = on ? (down && !(o->state & State_On) ? kAccentLo : kAccent)
                       : QColor(0x3A, 0x3A, 0x3C);
      if (hover && !on)
        fill = QColor(0x48, 0x48, 0x4A);
      p->save();
      p->setOpacity(enabled ? 1.0 : 0.4);
      roundRect(p, b, 4, fill, QPen(on ? alpha(Qt::black, 60) : white(64), 1.0));
      if (o->state & State_NoChange) {
        p->setRenderHint(QPainter::Antialiasing);
        p->setPen(QPen(kOnAccent, 1.8, Qt::SolidLine, Qt::RoundCap));
        p->drawLine(QPointF(b.left() + 4, b.center().y()),
                    QPointF(b.right() - 4, b.center().y()));
      } else if (o->state & State_On) {
        p->setRenderHint(QPainter::Antialiasing);
        p->setPen(QPen(kOnAccent, 1.9, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        QPainterPath c;
        c.moveTo(b.left() + b.width() * 0.26, b.top() + b.height() * 0.54);
        c.lineTo(b.left() + b.width() * 0.43, b.top() + b.height() * 0.71);
        c.lineTo(b.left() + b.width() * 0.76, b.top() + b.height() * 0.31);
        p->drawPath(c);
      }
      if (o->state & State_HasFocus && (o->state & State_KeyboardFocusChange))
        focusGlow(p, b, 4);
      p->restore();
      return;
    }
    case PE_IndicatorRadioButton: {
      const QRectF b = QRectF(o->rect).adjusted(0.5, 0.5, -0.5, -0.5);
      const bool on = o->state & State_On;
      QColor fill = on ? kAccent : (hover ? QColor(0x48, 0x48, 0x4A)
                                          : QColor(0x3A, 0x3A, 0x3C));
      p->save();
      p->setOpacity(enabled ? 1.0 : 0.4);
      roundRect(p, b, b.width() / 2, fill,
                QPen(on ? alpha(Qt::black, 60) : white(64), 1.0));
      if (on) {
        p->setRenderHint(QPainter::Antialiasing);
        p->setPen(Qt::NoPen);
        p->setBrush(kOnAccent);
        p->drawEllipse(b.center(), b.width() * 0.19, b.width() * 0.19);
      }
      if (o->state & State_HasFocus && (o->state & State_KeyboardFocusChange))
        focusGlow(p, b, b.width() / 2);
      p->restore();
      return;
    }
    case PE_PanelMenu:
      p->fillRect(o->rect, kMenuBg);
      return;
    case PE_FrameMenu:
      p->save();
      p->setPen(white(36));
      p->setBrush(Qt::NoBrush);
      p->drawRect(QRectF(o->rect).adjusted(0.5, 0.5, -0.5, -0.5));
      p->restore();
      return;
    case PE_PanelTipLabel:
      roundRect(p, r, 6, QColor(0x2E, 0x2E, 0x2E), QPen(white(36), 1.0));
      return;
    case PE_FrameGroupBox:
      roundRect(p, r, 8, white(8), QPen(white(36), 1.0));
      return;
    case PE_FrameTabWidget:
      roundRect(p, r.adjusted(0, 0, 0, 0), 8, white(6), QPen(white(36), 1.0));
      return;
    case PE_FrameTabBarBase:
      return;
    case PE_PanelMenuBar:
      p->fillRect(o->rect, kBar);
      return;
    case PE_IndicatorArrowDown:
    case PE_IndicatorArrowUp: {
      drawChevron(p, o->rect.center(), qMin(o->rect.width(), 9),
                  pe == PE_IndicatorArrowUp,
                  enabled ? o->palette.color(QPalette::ButtonText)
                          : white(90));
      return;
    }
    default:
      break;
    }
    QProxyStyle::drawPrimitive(pe, o, p, w);
  }

  // ---- Elementos de control -------------------------------------------
  void drawControl(ControlElement ce, const QStyleOption *o, QPainter *p,
                   const QWidget *w) const override {
    const bool enabled = o->state & State_Enabled;
    switch (ce) {
    case CE_PushButtonLabel:
      if (const auto *b = qstyleoption_cast<const QStyleOptionButton *>(o)) {
        if (b->features & QStyleOptionButton::DefaultButton) {
          QStyleOptionButton nb(*b);
          nb.palette.setColor(QPalette::ButtonText, kOnAccent);
          QProxyStyle::drawControl(ce, &nb, p, w);
          return;
        }
      }
      break;
    case CE_MenuItem:
      if (const auto *mi = qstyleoption_cast<const QStyleOptionMenuItem *>(o)) {
        if (mi->menuItemType == QStyleOptionMenuItem::Separator) {
          p->fillRect(mi->rect.adjusted(8, mi->rect.height() / 2, -8,
                                        -(mi->rect.height() - mi->rect.height() / 2 - 1)),
                      white(36));
          return;
        }
        QStyleOptionMenuItem nm(*mi);
        if ((mi->state & State_Selected) && enabled) {
          roundRect(p, QRectF(mi->rect).adjusted(4, 1, -4, -1), 5, kAccent);
          nm.state &= ~State_Selected;
          nm.palette.setColor(QPalette::Text, kOnAccent);
          nm.palette.setColor(QPalette::ButtonText, kOnAccent);
          nm.palette.setColor(QPalette::WindowText, kOnAccent);
        } else {
          nm.state &= ~State_Selected;
        }
        // El fondo ya esta pintado: se evita que Fusion lo repinte.
        nm.palette.setColor(QPalette::Window, Qt::transparent);
        nm.palette.setColor(QPalette::Button, Qt::transparent);
        nm.rect = mi->rect.adjusted(6, 0, -6, 0);
        QProxyStyle::drawControl(ce, &nm, p, w);
        return;
      }
      break;
    case CE_MenuBarItem:
      if (const auto *mi = qstyleoption_cast<const QStyleOptionMenuItem *>(o)) {
        if ((mi->state & State_Selected) || (mi->state & State_Sunken))
          roundRect(p, QRectF(mi->rect).adjusted(1, 2, -1, -2), 5, white(36));
        drawItemText(p, mi->rect,
                     Qt::AlignCenter | Qt::TextShowMnemonic | Qt::TextDontClip |
                         Qt::TextSingleLine,
                     mi->palette, enabled, mi->text, QPalette::WindowText);
        return;
      }
      break;
    case CE_MenuBarEmptyArea:
      return; // lo pinta PE_PanelMenuBar
    case CE_ToolBar:
      p->fillRect(o->rect, kBar);
      p->save();
      p->setPen(alpha(Qt::black, 100));
      p->drawLine(o->rect.bottomLeft(), o->rect.bottomRight());
      p->restore();
      return;
    case CE_TabBarTabShape:
      if (const auto *t = qstyleoption_cast<const QStyleOptionTab *>(o)) {
        const bool horiz = t->shape == QTabBar::RoundedNorth ||
                           t->shape == QTabBar::RoundedSouth ||
                           t->shape == QTabBar::TriangularNorth ||
                           t->shape == QTabBar::TriangularSouth;
        if (horiz) {
          const QRectF r = QRectF(t->rect).adjusted(2, 3, -2, -3);
          if (t->state & State_Selected)
            roundRect(p, r, 6, white(41));
          else if ((t->state & State_MouseOver) && enabled)
            roundRect(p, r, 6, white(20));
          return;
        }
      }
      break;
    case CE_TabBarTabLabel:
      if (const auto *t = qstyleoption_cast<const QStyleOptionTab *>(o)) {
        QStyleOptionTab nt(*t);
        const QColor c = (t->state & State_Selected) ? QColor(Qt::white) : white(204);
        nt.palette.setColor(QPalette::WindowText, c);
        nt.palette.setColor(QPalette::ButtonText, c);
        nt.palette.setColor(QPalette::Text, c);
        QProxyStyle::drawControl(ce, &nt, p, w);
        return;
      }
      break;
    case CE_ProgressBarGroove: {
      const QRectF r = QRectF(o->rect);
      const qreal h = qMin<qreal>(r.height(), 8);
      const QRectF g(r.left(), r.center().y() - h / 2, r.width(), h);
      roundRect(p, g, h / 2, QColor(0x3A, 0x3A, 0x3C));
      return;
    }
    case CE_ProgressBarContents:
      if (const auto *pb = qstyleoption_cast<const QStyleOptionProgressBar *>(o)) {
        if (pb->minimum == pb->maximum)
          break; // indeterminada: animacion de Fusion
        const QRectF r = QRectF(pb->rect);
        const qreal h = qMin<qreal>(r.height(), 8);
        const qreal frac = qreal(pb->progress - pb->minimum) /
                           qMax(1, pb->maximum - pb->minimum);
        qreal wd = qMax<qreal>(h, r.width() * qBound<qreal>(0, frac, 1));
        if (pb->progress <= pb->minimum)
          return;
        roundRect(p, QRectF(r.left(), r.center().y() - h / 2, wd, h), h / 2, kAccent);
        return;
      }
      break;
    case CE_HeaderSection: {
      p->fillRect(o->rect, kBar);
      p->save();
      p->setPen(alpha(Qt::black, 100));
      p->drawLine(o->rect.bottomLeft(), o->rect.bottomRight());
      if (o->state & State_MouseOver)
        p->fillRect(o->rect, white(14));
      p->setPen(white(26));
      p->drawLine(o->rect.topRight() + QPoint(0, 4), o->rect.bottomRight() - QPoint(0, 4));
      p->restore();
      return;
    }
    default:
      break;
    }
    QProxyStyle::drawControl(ce, o, p, w);
  }

  // ---- Controles complejos ---------------------------------------------
  void drawComplexControl(ComplexControl cc, const QStyleOptionComplex *o,
                          QPainter *p, const QWidget *w) const override {
    const bool enabled = o->state & State_Enabled;
    const bool hover = o->state & State_MouseOver;
    switch (cc) {
    case CC_ComboBox:
      if (const auto *cb = qstyleoption_cast<const QStyleOptionComboBox *>(o)) {
        const QRectF r = QRectF(cb->rect).adjusted(0.5, 0.5, -0.5, -0.5);
        if (cb->editable) {
          roundRect(p, r, 6, cb->palette.brush(QPalette::Base), QPen(white(31), 1.0));
          if (cb->state & State_HasFocus)
            focusGlow(p, r, 6);
        } else {
          paintButton(p, r, enabled, hover, cb->state & State_Sunken, false, false);
          if ((cb->state & State_HasFocus) && (cb->state & State_KeyboardFocusChange))
            focusGlow(p, r, 6);
        }
        // Popup button de macOS: cuadrado de acento con el par de chevrons.
        const QRectF a = QRectF(subControlRect(CC_ComboBox, cb, SC_ComboBoxArrow, w));
        roundRect(p, a, 4.5, enabled ? kAccent : QColor(0x4A, 0x4A, 0x4C));
        const QColor ch = enabled ? kOnAccent : white(110);
        drawChevron(p, QPointF(a.center().x(), a.center().y() - 3.2), 6.5, true, ch, 1.5);
        drawChevron(p, QPointF(a.center().x(), a.center().y() + 3.2), 6.5, false, ch, 1.5);
        return;
      }
      break;
    case CC_Slider:
      if (const auto *s = qstyleoption_cast<const QStyleOptionSlider *>(o)) {
        const bool horiz = s->orientation == Qt::Horizontal;
        const QRectF groove = QRectF(subControlRect(CC_Slider, s, SC_SliderGroove, w));
        const QRectF handle = QRectF(subControlRect(CC_Slider, s, SC_SliderHandle, w));
        p->save();
        p->setOpacity(enabled ? 1.0 : 0.45);
        const qreal t = 4;
        QRectF g = horiz ? QRectF(groove.left(), groove.center().y() - t / 2, groove.width(), t)
                         : QRectF(groove.center().x() - t / 2, groove.top(), t, groove.height());
        roundRect(p, g, t / 2, QColor(0x3A, 0x3A, 0x3C));
        QRectF fill = g;
        if (horiz)
          fill.setRight(handle.center().x());
        else
          fill.setTop(handle.center().y());
        if (s->upsideDown && horiz) {
          fill = g;
          fill.setLeft(handle.center().x());
        }
        roundRect(p, fill, t / 2, kAccent);
        // Pomo blanco con sombra suave.
        const QPointF c = handle.center();
        const qreal rad = 8;
        p->setRenderHint(QPainter::Antialiasing);
        p->setPen(Qt::NoPen);
        p->setBrush(alpha(Qt::black, 70));
        p->drawEllipse(c + QPointF(0, 1), rad + 0.5, rad + 0.5);
        QLinearGradient gr(c.x(), c.y() - rad, c.x(), c.y() + rad);
        gr.setColorAt(0, QColor(0xFF, 0xFF, 0xFF));
        gr.setColorAt(1, QColor(0xE6, 0xE6, 0xE8));
        p->setBrush(gr);
        p->setPen(QPen(alpha(Qt::black, 50), 0.8));
        p->drawEllipse(c, rad - 0.5, rad - 0.5);
        p->restore();
        return;
      }
      break;
    case CC_ScrollBar:
      if (const auto *sb = qstyleoption_cast<const QStyleOptionSlider *>(o)) {
        const bool horiz = sb->orientation == Qt::Horizontal;
        const QRect sr = subControlRect(CC_ScrollBar, sb, SC_ScrollBarSlider, w);
        if (sb->maximum <= sb->minimum)
          return; // nada que desplazar
        const bool active = (sb->activeSubControls & SC_ScrollBarSlider) &&
                            (sb->state & State_Sunken);
        const int thick = (hover || active) ? 8 : 6;
        QRectF h = QRectF(sr);
        if (horiz)
          h = QRectF(sr.left() + 1, sr.center().y() - thick / 2.0 + 0.5,
                     sr.width() - 2, thick);
        else
          h = QRectF(sr.center().x() - thick / 2.0 + 0.5, sr.top() + 1, thick,
                     sr.height() - 2);
        roundRect(p, h, thick / 2.0, white(active ? 178 : (hover ? 140 : 90)));
        return;
      }
      break;
    case CC_SpinBox:
      if (const auto *sp = qstyleoption_cast<const QStyleOptionSpinBox *>(o)) {
        const QRectF r = QRectF(sp->rect).adjusted(0.5, 0.5, -0.5, -0.5);
        roundRect(p, r, 6, sp->palette.brush(QPalette::Base), QPen(white(31), 1.0));
        if (sp->state & State_HasFocus)
          focusGlow(p, r, 6);
        const QRect up = subControlRect(CC_SpinBox, sp, SC_SpinBoxUp, w);
        const QRect dn = subControlRect(CC_SpinBox, sp, SC_SpinBoxDown, w);
        const QColor c = enabled ? white(190) : white(70);
        drawChevron(p, QRectF(up).center(), 6, true, c, 1.4);
        drawChevron(p, QRectF(dn).center(), 6, false, c, 1.4);
        return;
      }
      break;
    default:
      break;
    }
    QProxyStyle::drawComplexControl(cc, o, p, w);
  }

private:
  // Boton de macOS oscuro: degradado suave, borde fino y brillo superior.
  void paintButton(QPainter *p, const QRectF &r, bool enabled, bool hover,
                   bool pressed, bool checked, bool def) const {
    QColor top, bot;
    if (def && enabled) {
      top = pressed ? kAccentLo : kAccentHi;
      bot = pressed ? QColor(0x2D, 0xB8, 0x78) : kAccent;
    } else if (!enabled) {
      top = QColor(0x2A, 0x2A, 0x2C);
      bot = QColor(0x26, 0x26, 0x28);
    } else {
      top = QColor(0x48, 0x48, 0x4A);
      bot = QColor(0x3A, 0x3A, 0x3C);
      if (hover)
        top = shade(top, 12), bot = shade(bot, 12);
      if (pressed || checked)
        top = QColor(0x30, 0x30, 0x32), bot = QColor(0x3A, 0x3A, 0x3C);
    }
    top = bot; // sin degradados: color solido
    QLinearGradient g(r.topLeft(), r.bottomLeft());
    g.setColorAt(0, top);
    g.setColorAt(1, bot);
    roundRect(p, r, 6, g, QPen(alpha(Qt::black, enabled ? 90 : 50), 1.0));
    if (enabled && !pressed && !checked) {
      p->save();
      p->setRenderHint(QPainter::Antialiasing);
      p->setPen(QPen(white(def ? 60 : 36), 1.0));
      p->drawLine(QPointF(r.left() + 5, r.top() + 1.0), QPointF(r.right() - 5, r.top() + 1.0));
      p->restore();
    }
  }
};

} // namespace

class AuroraStylePlugin : public QStylePlugin {
  Q_OBJECT
  Q_PLUGIN_METADATA(IID QStyleFactoryInterface_iid FILE "auroraqstyle.json")
public:
  QStyle *create(const QString &key) override {
    return key.compare(QLatin1String("aurora"), Qt::CaseInsensitive) == 0
               ? new AuroraStyle
               : nullptr;
  }
};

#include "aurora-style.moc"
