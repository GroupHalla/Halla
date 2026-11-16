#pragma once

#include <QString>

// Halla visual theme (light/dark) -- centralized so that EVERY widget follows the
// switch at runtime, including on Windows.
namespace HTheme {

// true when the user chose the dark theme (design/theme == 1)
bool isDark();

// global application stylesheet (colors derived from the theme)
QString styleSheet(bool dark);

// aplica estilo (Fusion) + paleta + stylesheet global ao qApp
void apply();

} // namespace HTheme
