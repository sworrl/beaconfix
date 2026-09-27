#pragma once
#include <QDialog>
#include <QJsonObject>

class Locator;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;

// Place / edit an anchor (docs/RANGING.md §4): a surveyed transmitter or place. Used by the map's
// "Place an antenna here…" and the Anchors list in Settings. Produces the frozen JSON contract.
class AnchorDialog : public QDialog {
    Q_OBJECT
public:
    // existing: an anchor's JSON to edit ({} = new one at lat/lon)
    AnchorDialog(Locator *loc, double lat, double lon, const QJsonObject &existing, QWidget *parent = nullptr);
    QJsonObject anchor() const;             // what to hand to Locator::setAnchor

private:
    void kindChanged();
    Locator *m_loc;
    QJsonObject m_base;
    double m_lat, m_lon;
    QLineEdit *m_name, *m_bssids, *m_lat_e, *m_lon_e;
    QComboBox *m_kind, *m_heard;
    QDoubleSpinBox *m_acc, *m_height;
    QCheckBox *m_hasHeight, *m_rv, *m_ref;
    QLabel *m_hint;
};
