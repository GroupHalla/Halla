#pragma once

#include <QString>

// Tema visual do Halla (claro/escuro) — centralizado para que TODOS os
// widgets acompanhem a troca em tempo de execução, inclusive no Windows.
namespace HTheme {

// true se o usuário escolheu o tema escuro (design/theme == 1)
bool isDark();

// stylesheet global do aplicativo (cores derivadas do tema)
QString styleSheet(bool dark);

// aplica estilo (Fusion) + paleta + stylesheet global ao qApp
void apply();

} // namespace HTheme
