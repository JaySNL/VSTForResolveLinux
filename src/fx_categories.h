// Giving our menu entries a category instead of leaving them all under "Uncategorized".
//
// Resolve does not derive a category from the plugin. It looks the effect up in a table that is
// compiled into libFairlightPage.so, so an effect nobody at Blackmagic listed is Uncategorized on
// every platform. See fx_categories.cpp for the format and for why the table is patched in memory.
#ifndef FXBRIDGE_FX_CATEGORIES_H
#define FXBRIDGE_FX_CATEGORIES_H

// Rewrites the compiled category table so every scanned plugin has an entry. Call once, after the
// scan and before Fairlight builds its effect menu. Reports what it did through the logger.
void FxCategoriesApply();

void FxCategoriesSetLogger(void (*logger)(const char*));

#endif
