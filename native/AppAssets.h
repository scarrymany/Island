#pragma once

#include <QIcon>
#include <QString>
#include <QStringList>

namespace AppAssets {
QString settingsFontFamily();
QStringList bundledFontFamilies();
QIcon icon();
QIcon githubIcon(bool darkBackground);
}
